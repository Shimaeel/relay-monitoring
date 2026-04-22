// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file relay_pipeline.hpp
 * @brief Per-relay processing pipeline for on-demand relay connections.
 *
 * @details Bundles all per-relay components (TelnetClient, workers, ring
 * buffer) into a single RelayPipeline object.  The pipeline is created
 * on demand when the user clicks a relay card in the browser, and torn
 * down when the relay is disconnected.
 *
 * ## Architecture (per pipeline)
 *
 * @code
 * Relay Device (TCP:23)
 *       │
 *       ▼
 * TelnetClient (owned)
 *       │
 *       ▼
 * ReceptionWorker → RawDataRingBuffer → ProcessingWorker
 *                                              │
 *                                  ┌───────────┼───────────┐
 *                                  ▼           ▼           ▼
 *                         SharedDB (ref)  WS broadcast  SHM ring
 * @endcode
 *
 * ## Shared vs. Owned Resources
 *
 * | Resource             | Ownership  | Notes                       |
 * |----------------------|------------|-----------------------------|
 * | TelnetClient         | Owned      | One per relay               |
 * | RawDataRingBuffer    | Owned      | One per relay               |
 * | ReceptionWorker      | Owned      | One per relay               |
 * | ProcessingWorker     | Owned      | One per relay               |
 * | SERDatabase          | **Shared** | Single DB, relay_id column  |
 * | SERWebSocketServer   | **Shared** | Single WS server, port 8765 |
 * | SharedRingBuffer     | **Shared** | Single SHM ring             |
 *
 * @see relay_config.hpp   Relay configuration definitions
 * @see relay_manager.hpp  Coordinates multiple pipelines
 * @see telnet_sml_app.cpp Uses RelayManager to start/stop pipelines
 *
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "client.hpp"
#include "relay_config.hpp"
#include "raw_data_ring_buffer.hpp"
#include "ser_database.hpp"
#include "ser_record.hpp"
#include "asn_tlv_codec.hpp"
#include "shared_memory/shared_ring_buffer.hpp"
#include "telnet_fsm.hpp"
#include "ws_server.hpp"

// ============================================================================
//                     PER-RELAY RECEPTION WORKER
// ============================================================================

/**
 * @class PipelineReceptionWorker
 * @brief Non-blocking Telnet I/O worker for a single relay pipeline.
 *
 * @details Handles TCP connection, login, command execution with retries,
 * and pushes responses to relay-owned ring buffer.  Identical in logic
 * to the original ReceptionWorker but used as part of a RelayPipeline.
 *
 * @see RelayPipeline      Owner of this worker
 * @see PipelineProcessingWorker  Consumer of the ring buffer
 */
class PipelineReceptionWorker
{
    std::atomic<bool> stop_flag_{false};
    std::thread worker_thread_;
    std::mutex queue_mutex_;
    std::mutex sync_cmd_mutex_;          ///< Guards synchronous command execution
    std::condition_variable queue_cv_;
    std::deque<std::string> command_queue_;

    // ── Dependencies (order matters: fsm_ must be declared after its deps) ──
    TelnetClient& client_;
    ConnectionConfig conn_;
    LoginConfig creds_;
    RetryState retry_;                                ///< Login retry state (3 max)
    sml::sm<RelayConnectionFSM> fsm_;                 ///< Connection lifecycle FSM

    // ── Command FSM (FSM #2 — user-initiated commands) ──
    CmdResponseHolder cmdResponse_;                   ///< Shared response holder
    sml::sm<RelayCommandFSM> cmdFsm_;                 ///< Command execution FSM

    RawDataRingBuffer& rawBuffer_;
    std::atomic<bool>& app_running_;
    std::string relay_tag_;       ///< Log prefix, e.g. "[Rx:SEL-751]"
    bool screen_size_configured_{false};  ///< True after SET S sent post-login

    static constexpr int MAX_RETRIES = 3;             ///< Per-command retry limit
    static constexpr int RETRY_DELAY_SEC = 5;         ///< Interruptible wait (seconds)
    static constexpr int ERROR_RECOVERY_DELAY_SEC = 60; ///< Wait before auto-recovery from Error (24/7)

    // ─── FSM Driver ─────────────────────────────────────────────────────────

    /**
     * @brief Drive the RelayConnectionFSM to Operational state.
     *
     * @details Repeatedly fires step_event.  When the FSM enters a wait
     * state (ConnectWait / LoginRetryWait) the worker sleeps in 1-second
     * chunks so that stop() can interrupt it.
     *
     * @return true  FSM reached Operational — ready for commands
     * @return false FSM reached Error, or stop was requested
     */
    bool driveToOperational()
    {
        using namespace sml;

        while (!stop_flag_.load())
        {
            if (fsm_.is("Operational"_s))
            {
                // Configure large screen size once after login so the relay
                // sends more rows per page, reducing "Press RETURN" pagination.
                if (!screen_size_configured_)
                {
                    std::string discarded;
                    client_.SendCmdReceiveData("SET S 132 99", discarded);
                    screen_size_configured_ = true;
                    std::cout << relay_tag_ << " Screen size set to 132x99\n";
                }
                return true;
            }

            if (fsm_.is("Error"_s))
            {
                // 24/7 auto-recovery: wait before retrying instead of giving up
                std::cout << relay_tag_ << " FSM in Error state — waiting "
                          << ERROR_RECOVERY_DELAY_SEC << "s before auto-recovery...\n";
                screen_size_configured_ = false;
                for (int s = 0; s < ERROR_RECOVERY_DELAY_SEC && !stop_flag_.load(); ++s)
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                if (stop_flag_.load())
                    return false;
                // Trigger reconnection from Error state
                fsm_.process_event(disconnect_event{});
                continue;
            }

            // Interruptible wait for retry states
            if (fsm_.is("ConnectWait"_s) || fsm_.is("LoginRetryWait"_s))
            {
                screen_size_configured_ = false;  // Reset on reconnect
                std::cout << relay_tag_ << " Waiting " << RETRY_DELAY_SEC
                          << "s before retry...\n";
                for (int s = 0; s < RETRY_DELAY_SEC && !stop_flag_.load(); ++s)
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                if (stop_flag_.load())
                    return false;
            }

            fsm_.process_event(step_event{});
        }
        return false;
    }

    // ─── Command FSM Event Dispatch ──────────────────────────────────────────

    /**
     * @brief Parse a command string and fire the corresponding Command FSM event.
     *
     * @details Maps string commands to typed FSM events:
     * - "SER"     → cmd_ser_event
     * - "TAR ..." → cmd_tar_event{args}
     * - "EVE"     → cmd_eve_event
     * - "CTRL+C"  → cmd_ctrlc_event
     * - "CTRL+D"  → cmd_ctrld_event
     * - "SET ..." → cmd_set_event{args}
     *
     * @param cmd  Command string from queue or user request
     */
    void fireCommandEvent(const std::string& cmd)
    {
        cmdResponse_.success = false;
        cmdResponse_.response.clear();

        if (cmd == "SER")
            cmdFsm_.process_event(cmd_ser_event{});
        else if (cmd == "FILE DIR EVENTS")
            cmdFsm_.process_event(cmd_file_dir_events_event{});
        else if (cmd == "FILE DIR SETTINGS")
            cmdFsm_.process_event(cmd_file_dir_settings_event{});
        else if (cmd.size() >= 3 && cmd.substr(0, 3) == "TAR")
            cmdFsm_.process_event(cmd_tar_event{cmd.size() > 4 ? cmd.substr(4) : ""});
        else if (cmd == "EVE")
        {
            // EVE response can be multi-page — use direct multi-page collection
            client_.clearLastResponse();
            std::string response;
            bool ok = client_.SendCmdMultiPage("EVE", response);
            cmdResponse_.success = ok && !response.empty();
            cmdResponse_.response = std::move(response);
            std::cout << "[CmdFSM] EVE " << (cmdResponse_.success ? "OK" : "FAIL") << "\n";
        }
        else if (cmd == "CTRL+C")
            cmdFsm_.process_event(cmd_ctrlc_event{});
        else if (cmd == "CTRL+D")
            cmdFsm_.process_event(cmd_ctrld_event{});
        else if (cmd.size() >= 3 && cmd.substr(0, 3) == "SET")
            cmdFsm_.process_event(cmd_set_event{cmd.size() > 4 ? cmd.substr(4) : ""});
        else if (cmd.size() >= 3 && cmd.substr(0, 3) == "PAS")
        {
            // Password change — requires Level 2 elevation.
            // Sequence:
            //   1) 2AC  + l2 password   → Level 2 (=>> prompt)
            //   2) PAS LEVEL<n> <value> → actual change
            //   3) ACC                  → back to Level 1 for safety
            client_.clearLastResponse();
            std::string response;
            bool elevated = client_.LoginLevel2Function(creds_.l2_pass);
            if (!elevated)
            {
                cmdResponse_.success = false;
                cmdResponse_.response = "L2 elevation failed — check Level-2 password";
                std::cout << "[CmdFSM] PAS failed: could not elevate to L2\n";
                return;
            }

            bool ok = client_.SendCmdReceiveData(cmd, response);
            // Always demote back to Level 1, even on PAS failure.
            client_.LogoutLevel2Function();

            // Detect relay-side rejection (response contains "Invalid" / "Denied")
            bool rejected = response.find("Invalid") != std::string::npos
                         || response.find("invalid") != std::string::npos
                         || response.find("Denied")  != std::string::npos
                         || response.find("denied")  != std::string::npos;

            cmdResponse_.success = ok && !response.empty() && !rejected;
            cmdResponse_.response = std::move(response);
            std::cout << "[CmdFSM] PAS "
                      << (cmdResponse_.success ? "OK" : "FAIL")
                      << (rejected ? " (relay rejected)" : "") << "\n";
        }
        else
        {
            // Generic / unknown command — send directly.
            // Multi-page commands (SHOSET, FIL DIR, CTR C/D) may produce
            // "Press RETURN to continue" pagination even with large screen.
            client_.clearLastResponse();
            std::string response;
            bool ok;
            if (cmd == "SHOSET" ||
                (cmd.size() >= 3 && cmd.substr(0, 3) == "FIL") ||
                (cmd.size() >= 3 && cmd.substr(0, 3) == "CTR"))
                ok = client_.SendCmdMultiPage(cmd, response);
            else
                ok = client_.SendCmdReceiveData(cmd, response);
            cmdResponse_.success = ok && !response.empty();
            cmdResponse_.response = std::move(response);
            std::cout << "[CmdFSM] Generic '" << cmd << "' "
                      << (cmdResponse_.success ? "OK" : "FAIL") << "\n";
        }
    }

    // ─── Command Execution ──────────────────────────────────────────────────

    /**
     * @brief Execute a Telnet command via Command FSM with smart retry.
     *
     * @details Up to MAX_RETRIES attempts with failure-aware retry:
     * - TIMEOUT:   Retry directly (no reconnect needed)
     * - CONN_LOST: Trigger reconnect cycle, then retry
     *
     * @param cmd  Telnet command string (e.g. "SER", "FIL DIR")
     */
    void executeCommand(const std::string& cmd)
    {
        std::lock_guard<std::mutex> lock(sync_cmd_mutex_);
        for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt)
        {
            if (stop_flag_.load())
                return;

            // Phase 1 — ensure Connection FSM is in Operational state
            if (!driveToOperational())
            {
                std::cout << relay_tag_ << " FSM not operational — aborting '"
                          << cmd << "'\n";
                return;
            }

            // Phase 2 — send command through Command FSM
            fireCommandEvent(cmd);

            if (cmdResponse_.success)
            {
                rawBuffer_.push({cmd, cmdResponse_.response});
                std::cout << relay_tag_ << " Pushed " << cmdResponse_.response.size()
                          << " bytes for '" << cmd << "'\n";
                return;
            }

            // Phase 3 — smart retry based on failure reason
            std::cout << relay_tag_ << " Command '" << cmd << "' failed ("
                      << (cmdResponse_.failReason == CmdFailReason::CONN_LOST ? "CONN_LOST" : "TIMEOUT")
                      << ", attempt " << attempt << "/" << MAX_RETRIES << ")\n";

            if (cmdResponse_.failReason == CmdFailReason::CONN_LOST)
            {
                // Connection dropped — full reconnect needed
                screen_size_configured_ = false;
                fsm_.process_event(disconnect_event{});
            }
            // TIMEOUT — retry directly without reconnect
        }

        std::cout << relay_tag_ << " All retries failed for '" << cmd << "'\n";
    }

    // ─── Worker Thread ──────────────────────────────────────────────────────

    void runLoop()
    {
        std::cout << relay_tag_ << " Worker thread started (FSM-driven)\n";

        bool first_iter = true;
        while (!stop_flag_.load())
        {
            try
            {
                // Kick the FSM out of Idle → Connecting on first run, or
                // after a crash attempt a clean reconnect.
                {
                    std::lock_guard<std::mutex> lock(sync_cmd_mutex_);
                    if (first_iter)
                        fsm_.process_event(start_event{});
                    else
                        fsm_.process_event(disconnect_event{});
                }
                first_iter = false;

                while (!stop_flag_.load())
                {
                    std::string cmd;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex_);
                        queue_cv_.wait(lock, [&]() {
                            return stop_flag_.load() || !command_queue_.empty();
                        });

                        if (stop_flag_.load())
                            break;

                        if (command_queue_.empty())
                            continue;

                        cmd = std::move(command_queue_.front());
                        command_queue_.pop_front();
                    }

                    executeCommand(cmd);
                }
            }
            catch (const std::exception& e)
            {
                std::cerr << relay_tag_ << " Reception worker threw: "
                          << e.what() << " — restarting in 5s\n";
            }
            catch (...)
            {
                std::cerr << relay_tag_ << " Reception worker threw (unknown) — restarting in 5s\n";
            }

            // Backoff before auto-restart; honor stop_flag during sleep.
            for (int s = 0; s < 5 && !stop_flag_.load(); ++s)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        std::cout << relay_tag_ << " Worker thread exiting\n";
    }

public:
    /**
     * @brief Construct a PipelineReceptionWorker with FSM-driven lifecycle.
     *
     * @param client       TelnetClient (owned by RelayPipeline)
     * @param conn         Connection parameters (host, port, timeout)
     * @param creds        Level-1 credentials
     * @param rawBuffer    Per-relay ring buffer (producer side)
     * @param app_running  Global running flag
     * @param relayName    Display name for log tagging
     */
    PipelineReceptionWorker(TelnetClient& client,
                            const ConnectionConfig& conn,
                            const LoginConfig& creds,
                            RawDataRingBuffer& rawBuffer,
                            std::atomic<bool>& app_running,
                            const std::string& relayName)
        : client_(client)
        , conn_(conn)
        , creds_(creds)
        , retry_{3, 0, std::chrono::seconds(5)}
        , fsm_{client_, conn_, creds_, retry_}
        , cmdFsm_{client_, cmdResponse_}
        , rawBuffer_(rawBuffer)
        , app_running_(app_running)
        , relay_tag_("[Rx:" + relayName + "]")
    {
    }

    /**
     * @brief Start the reception worker thread.
     * @post Worker thread is running and ready to accept queued commands.
     */
    void start()
    {
        stop_flag_ = false;
        worker_thread_ = std::thread([this]() { runLoop(); });
    }

    /**
     * @brief Stop the reception worker and join its thread.
     * @post Worker thread has exited and all resources are released.
     */
    void stop()
    {
        stop_flag_ = true;
        queue_cv_.notify_all();
        rawBuffer_.notifyAll();
        if (worker_thread_.joinable())
            worker_thread_.join();
    }

    /**
     * @brief Enqueue a Telnet command for asynchronous execution.
     * @param cmd Command string (e.g. "SER", "ACC", "FIL DIR").
     */
    void queueCommand(const std::string& cmd)
    {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            command_queue_.push_back(cmd);
        }
        queue_cv_.notify_one();
    }

    /**
     * @brief Execute a user-initiated command via Command FSM and return response.
     *
     * @details Grabs the command mutex so it doesn't race with the async
     * queue loop, drives Connection FSM to Operational, fires the command
     * through the Command FSM, and returns the raw response text.
     *
     * Called when user clicks a button in the browser UI.
     *
     * @param cmd Telnet command (e.g. "SER", "TAR 0", "EVE", "SET ...",
     *            "CTRL+C", "CTRL+D", "FIL DIR")
     * @return Raw relay response text, or empty string on failure
     */
    std::string handleUserCommand(const std::string& cmd)
    {
        std::lock_guard<std::mutex> lock(sync_cmd_mutex_);
        return executeUserCommandLocked(cmd);
    }

private:
    /// Execute a single command (caller must already hold sync_cmd_mutex_).
    std::string executeUserCommandLocked(const std::string& cmd)
    {
        for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt)
        {
            if (stop_flag_.load())
                return "";

            if (!driveToOperational())
                return "";

            // Route through Command FSM
            fireCommandEvent(cmd);

            if (cmdResponse_.success)
            {
                std::cout << relay_tag_ << " User cmd '" << cmd
                          << "' OK (" << cmdResponse_.response.size() << " bytes)\n";
                return cmdResponse_.response;
            }

            std::cout << relay_tag_ << " User cmd '" << cmd << "' failed ("
                      << (cmdResponse_.failReason == CmdFailReason::CONN_LOST ? "CONN_LOST" : "TIMEOUT")
                      << ", attempt " << attempt << "/" << MAX_RETRIES << ")\n";

            if (cmdResponse_.failReason == CmdFailReason::CONN_LOST)
            {
                screen_size_configured_ = false;
                fsm_.process_event(disconnect_event{});
            }
            // TIMEOUT — retry directly without reconnect
        }
        return "";
    }

};

// ============================================================================
//                    PER-RELAY PROCESSING WORKER
// ============================================================================

/**
 * @class PipelineProcessingWorker
 * @brief Processing thread for a single relay pipeline.
 *
 * @details Reads from the relay's RawDataRingBuffer, parses SER responses,
 * stamps each record with the relay's identity (relay_id + relay_name),
 * inserts into the shared SQLite database, and broadcasts via the shared
 * WebSocket server.
 *
 * The key difference from the original ProcessingWorker is the relay_id /
 * relay_name stamping on every parsed SERRecord before DB insertion.
 *
 * @see PipelineReceptionWorker  Producer of the ring buffer data
 * @see RelayPipeline            Owner of this worker
 */
class PipelineProcessingWorker
{
    std::atomic<bool> stop_flag_{false};
    std::thread worker_thread_;

    RawDataRingBuffer& rawBuffer_;
    SERDatabase& db_;
    SERWebSocketServer& wsServer_;
    SharedRingBuffer& shmRing_;
    std::atomic<bool>& app_running_;
    RawDataRingBuffer::ReaderId readerId_{RawDataRingBuffer::kInvalidReader};

    std::string relay_id_;       ///< Relay identifier to stamp on records
    std::string relay_name_;     ///< Relay display name to stamp on records
    std::string relay_tag_;      ///< Log prefix
    int prune_cycle_count_{0};   ///< Counter for periodic DB pruning

    void runLoop()
    {
        std::cout << relay_tag_ << " Worker thread started\n";
        while (!stop_flag_.load() && app_running_.load())
        {
        try
        {
            while (!stop_flag_.load() && app_running_.load())
            {
                auto msg = rawBuffer_.waitPop(readerId_, stop_flag_);
                if (!msg)
                    continue;

                // Non-SER command handling
                if (msg->command == "FILE DIR EVENTS") {
                    // Broadcast as COMTRADE_DIR:<relay_id>:<payload>
                    std::string payload = "COMTRADE_DIR:" + relay_id_ + ":" + msg->response;
                    wsServer_.broadcastText(payload);
                    std::cout << relay_tag_ << " Broadcasted COMTRADE_DIR (" << msg->response.size() << " bytes)\n";
                    continue;
                } else if (msg->command == "FILE DIR SETTINGS") {
                    // Broadcast as SETTINGS_DIR:<relay_id>:<payload>
                    std::string payload = "SETTINGS_DIR:" + relay_id_ + ":" + msg->response;
                    wsServer_.broadcastText(payload);
                    std::cout << relay_tag_ << " Broadcasted SETTINGS_DIR (" << msg->response.size() << " bytes)\n";
                    continue;
                } else if (msg->command != "SER") {
                    std::cout << relay_tag_ << " Non-SER command '" << msg->command
                              << "', broadcasting text (" << msg->response.size() << " bytes)\n";
                    wsServer_.broadcastText(msg->response);
                    continue;
                }

                // Parse SER response
                auto records = parseSERResponse(msg->response);
                if (records.empty())
                {
                    std::cout << relay_tag_ << " No records parsed\n";
                    continue;
                }

                // *** STAMP relay identity on every record ***
                for (auto& rec : records)
                {
                    rec.relay_id   = relay_id_;
                    rec.relay_name = relay_name_;
                }

                std::cout << relay_tag_ << " Parsed " << records.size() << " SER records\n";

                // Step 1: SQLite insert (shared DB, relay_id prevents cross-relay duplicates)
                int inserted = 0;
                auto newRecords = db_.insertAndGetNewRecords(records, inserted);
                std::cout << relay_tag_ << " SQLite: " << inserted << " new records\n";

                // Periodic DB housekeeping: prune records older than 90 days
                // Run every 50 insert cycles to avoid overhead on every poll
                if (++prune_cycle_count_ >= 50)
                {
                    prune_cycle_count_ = 0;
                    db_.pruneOldRecords(90);
                }

                // Step 2: WebSocket broadcast — full DB refresh for all clients
                wsServer_.broadcastAll();

                // Step 3: Shared memory ring for JSON file writer
                if (inserted > 0)
                {
                    auto payload = asn_tlv::encodeSerRecordsToTlv(newRecords);
                    if (!payload.empty())
                        shmRing_.write(payload.data(), payload.size());
                }
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << relay_tag_ << " Processing worker threw: "
                      << e.what() << " — restarting in 5s\n";
        }
        catch (...)
        {
            std::cerr << relay_tag_ << " Processing worker threw (unknown) — restarting in 5s\n";
        }

        // Backoff before auto-restart; honor stop/app flags during sleep.
        for (int s = 0; s < 5 && !stop_flag_.load() && app_running_.load(); ++s)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        std::cout << relay_tag_ << " Worker thread exiting\n";
    }

public:
    /**
     * @brief Construct a processing worker for one relay.
     *
     * @param rawBuffer    Per-relay ring buffer (consumer side)
     * @param db           Shared SQLite database
     * @param wsServer     Shared WebSocket server
     * @param shmRing      Shared ring buffer for JSON file writer
     * @param app_running  Global running flag
     * @param relayId      Relay identifier stamped on parsed records
     * @param relayName    Display name for log tagging
     */
    PipelineProcessingWorker(RawDataRingBuffer& rawBuffer,
                             SERDatabase& db,
                             SERWebSocketServer& wsServer,
                             SharedRingBuffer& shmRing,
                             std::atomic<bool>& app_running,
                             const std::string& relayId,
                             const std::string& relayName)
        : rawBuffer_(rawBuffer)
        , db_(db)
        , wsServer_(wsServer)
        , shmRing_(shmRing)
        , app_running_(app_running)
        , relay_id_(relayId)
        , relay_name_(relayName)
        , relay_tag_("[Proc:" + relayName + "]")
    {
    }

    /**
     * @brief Start the processing worker: register a reader and launch its thread.
     */
    void start()
    {
        stop_flag_ = false;
        readerId_ = rawBuffer_.registerReader();
        if (readerId_ == RawDataRingBuffer::kInvalidReader)
        {
            std::cerr << relay_tag_ << " Failed to register reader\n";
            return;
        }
        worker_thread_ = std::thread([this]() { runLoop(); });
    }

    /**
     * @brief Stop the processing worker: join thread and unregister reader.
     */
    void stop()
    {
        stop_flag_ = true;
        rawBuffer_.notifyAll();
        if (worker_thread_.joinable())
            worker_thread_.join();
        if (readerId_ != RawDataRingBuffer::kInvalidReader)
        {
            rawBuffer_.unregisterReader(readerId_);
            readerId_ = RawDataRingBuffer::kInvalidReader;
        }
    }
};

// ============================================================================
//                         RELAY PIPELINE
// ============================================================================

/**
 * @class RelayPipeline
 * @brief Complete processing pipeline for a single relay device.
 *
 * @details Owns a TelnetClient, RawDataRingBuffer, ReceptionWorker, and
 * ProcessingWorker.  Holds references to the shared SERDatabase,
 * SERWebSocketServer, and SharedRingBuffer.
 *
 * Created on demand by RelayManager when the user activates a relay
 * from the dashboard.  Destroyed when the user deactivates the relay
 * or the application shuts down.
 *
 * ## Lifecycle
 *
 * @code
 * auto pipeline = std::make_unique<RelayPipeline>(config, db, ws, shm, running);
 * pipeline->start();       // Connects + starts polling
 * pipeline->queueCommand("SER");
 * // ...
 * pipeline->stop();        // Graceful shutdown
 * @endcode
 *
 * @see RelayConfig          Configuration for the relay
 * @see relay_manager.hpp    Manages pipeline lifecycle
 */
class RelayPipeline
{
    RelayConfig config_;
    SERDatabase& db_;
    SERWebSocketServer& wsServer_;
    SharedRingBuffer& shmRing_;
    std::atomic<bool>& app_running_;

    // Owned per-relay resources
    TelnetClient client_;
    RawDataRingBuffer rawBuffer_{100};

    std::unique_ptr<PipelineReceptionWorker>  rxWorker_;
    std::unique_ptr<PipelineProcessingWorker> procWorker_;

    std::atomic<bool> running_{false};

public:
    /**
     * @brief Construct a pipeline for a specific relay.
     *
     * @param config     Relay configuration (host, port, creds, etc.)
     * @param db         Shared SQLite database reference
     * @param wsServer   Shared WebSocket server reference
     * @param shmRing    Shared ring buffer for JSON file writer
     * @param app_running Global application running flag
     */
    RelayPipeline(const RelayConfig& config,
                  SERDatabase& db,
                  SERWebSocketServer& wsServer,
                  SharedRingBuffer& shmRing,
                  std::atomic<bool>& app_running)
        : config_(config)
        , db_(db)
        , wsServer_(wsServer)
        , shmRing_(shmRing)
        , app_running_(app_running)
    {
    }

    /// Non-copyable, non-movable (owns threads)
    RelayPipeline(const RelayPipeline&) = delete;
    RelayPipeline& operator=(const RelayPipeline&) = delete;

    /**
     * @brief Start the pipeline (connect, start workers).
     *
     * @details Creates and starts the ReceptionWorker and ProcessingWorker
     * threads.  The ReceptionWorker connects to the relay on the first
     * queued command.
     *
     * @return true on success, false if already running
     */
    bool start()
    {
        if (running_)
            return true;

        ConnectionConfig conn{config_.host, config_.port, config_.timeout};
        LoginConfig creds{config_.username, config_.password, config_.password_l2};

        rxWorker_ = std::make_unique<PipelineReceptionWorker>(
            client_, conn, creds, rawBuffer_, app_running_, config_.name);
        rxWorker_->start();

        procWorker_ = std::make_unique<PipelineProcessingWorker>(
            rawBuffer_, db_, wsServer_, shmRing_, app_running_,
            config_.id, config_.name);
        procWorker_->start();

        running_ = true;
        std::cout << "[Pipeline:" << config_.name << "] Started\n";


        // Queue initial SER, FILE DIR EVENTS, and FILE DIR SETTINGS commands
        queueCommand("SER");
        queueCommand("FILE DIR EVENTS");
        queueCommand("FILE DIR SETTINGS");

        return true;
    }

    /**
     * @brief Stop the pipeline (disconnect, stop workers).
     */
    void stop()
    {
        if (!running_)
            return;

        if (rxWorker_)
            rxWorker_->stop();
        if (procWorker_)
            procWorker_->stop();

        running_ = false;
        std::cout << "[Pipeline:" << config_.name << "] Stopped\n";
    }

    /**
     * @brief Queue a command for execution by this relay's ReceptionWorker.
     *
     * @param cmd Telnet command string (e.g. "SER", "ACC", "FIL DIR")
     */
    void queueCommand(const std::string& cmd)
    {
        if (rxWorker_)
            rxWorker_->queueCommand(cmd);
    }

    /**
     * @brief Execute a user-initiated command via Command FSM and return response.
     *
     * @param cmd Telnet command (e.g. "SER", "TAR 0", "EVE", "SET ...",
     *            "CTRL+C", "CTRL+D")
     * @return Raw relay response, or empty string on failure
     */
    std::string handleUserCommand(const std::string& cmd)
    {
        if (rxWorker_)
            return rxWorker_->handleUserCommand(cmd);
        return "";
    }

};
