// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file telnet_sml_app.cpp
 * @brief Telnet SML Application - Main orchestrator implementation.
 *
 * @details This file implements the TelnetSmlApp class, which orchestrates
 * the multi-threaded Telnet-SML system for substation relay communication.
 *
 * ## Architecture Overview
 * The application follows a sequential pipeline architecture:
 * @code
 * Relay Device (TCP:23)
 *       │
 *       ▼
 * TelnetClient Thread (ReceptionWorker)
 *       │
 *       ▼
 * C++ SPSC Ring Buffer (RawDataRingBuffer)
 *       │
 *       ▼
 * FSM/Processing Thread (ProcessingWorker)
 *       │
 *       ▼
 * SQLite (Persistent Storage)
 *       │
 *       ▼
 * WebSocket Server (Push to Browser)
 * @endcode
 *
 * ## Thread Model
 * | Thread | Component | Responsibility |
 * |--------|-----------|----------------|
 * | 1 | ReceptionWorker | Non-blocking Telnet I/O |
 * | 2 | ProcessingWorker | Parse + DB + WebSocket |
 * | 3 | SERWebSocketServer | Push notifications |
 * | 4 | ThreadManager | 2-minute polling |
 * | 5 | SharedSerReader | JSON file writer |
 *
 * @author Telnet-SML Team
 * @date 2026
 * @version 2.0
 * @see telnet_sml_app.hpp
 * @see architecture-daigram.mmd
 */

#include "telnet_sml_app.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

#include "client.hpp"
#include "asn_tlv_codec.hpp"
#include "raw_data_ring_buffer.hpp"
#include "relay_config.hpp"
#include "relay_manager.hpp"
#include "relay_pipeline.hpp"
#include "relay_service.hpp"
#include "ser_database.hpp"
#include "ser_json_writer.hpp"
#include "shared_memory/shared_ring_buffer.hpp"
#include "telnet_fsm.hpp"
#include "thread_manager.hpp"
#include "password_manager.hpp"
#include "time_sync_manager.hpp"
#include "ws_server.hpp"
#include "ws_db_server.hpp"

using namespace sml;

namespace
{

// ReceptionWorker and ProcessingWorker have been moved to relay_pipeline.hpp
// as PipelineReceptionWorker and PipelineProcessingWorker, used per-relay
// by RelayPipeline and coordinated by RelayManager.

/**
 * @brief Escape a string for safe JSON embedding.
 */
inline std::string escapeTarJson(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s)
    {
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}

}  // namespace

/**
 * @class SharedSerReader
 * @brief Thread 5 - Reads from SharedRingBuffer and writes JSON to file.
 *
 * @details Consumes ASN.1 TLV-encoded SER records from the shared ring buffer,
 * decodes them, and writes to data.json for the web UI.
 *
 * ## Thread Role
 * This is Thread 5 in the architecture, running independently to provide
 * file-based data export without blocking the main processing pipeline.
 *
 * ## Data Flow
 * @code
 * SharedRingBuffer (from ProcessingWorker)
 *       │
 *       ▼
 * ASN.1 TLV Decode
 *       │
 *       ▼
 * JSON Serialization
 *       │
 *       ▼
 * ui/data.json (file output)
 * @endcode
 *
 * @see ProcessingWorker Produces data for this consumer
 * @see asn_tlv::decodeSerRecordsFromTlv Decoding function
 */
class SharedSerReader
{
    SharedRingBuffer& ring_;                    ///< Shared ring buffer input
    std::string output_path_;                   ///< Output file path (e.g., "ui/data.json")
    std::atomic<bool> stop_flag_{false};        ///< Flag to signal worker termination
    std::thread worker_thread_;                  ///< Worker thread handle
    SharedRingBuffer::ReaderId readerId_{SharedRingBuffer::kInvalidReader}; ///< Registered reader ID

    /**
     * @brief Main worker thread loop.
     *
     * @details Continuously reads from shared ring buffer:
     * 1. Wait for TLV payload (blocking)
     * 2. Decode ASN.1 TLV to SERRecord vector
     * 3. Convert records to JSON string
     * 4. Write JSON to output file
     */
    void runLoop()
    {
        std::vector<uint8_t> payload;
        while (!stop_flag_.load())
        {
            if (!ring_.waitRead(readerId_, payload, stop_flag_))
                continue;

            std::vector<SERRecord> records;
            std::string error;
            if (!asn_tlv::decodeSerRecordsFromTlv(payload.data(), payload.size(), records, &error))
            {
                std::cout << "[SHM] Decode error: " << error << "\n";
                continue;
            }

            std::string json = recordsToJsonTable(records);
            std::string writeError;
            if (!writeJsonToFile(output_path_, json, &writeError))
            {
                std::cout << "[SHM] JSON write error: " << writeError << "\n";
            }
        }
    }

public:
    /**
     * @brief Constructs a SharedSerReader instance.
     *
     * @param ring Shared ring buffer to read TLV payloads from
     * @param output_path File path for JSON output (e.g., "ui/data.json")
     */
    SharedSerReader(SharedRingBuffer& ring, std::string output_path)
        : ring_(ring), output_path_(std::move(output_path))
    {
    }

    /**
     * @brief Starts the worker thread.
     */
    void start()
    {
        stop_flag_ = false;
        readerId_ = ring_.registerReader();
        if (readerId_ == SharedRingBuffer::kInvalidReader)
        {
            std::cerr << "[SHM] Failed to register reader — max readers reached\n";
            return;
        }
        worker_thread_ = std::thread([this]() { runLoop(); });
    }

    /**
     * @brief Stops the worker thread gracefully.
     */
    void stop()
    {
        stop_flag_ = true;
        ring_.notifyAll();
        if (worker_thread_.joinable())
            worker_thread_.join();
        if (readerId_ != SharedRingBuffer::kInvalidReader)
        {
            ring_.unregisterReader(readerId_);
            readerId_ = SharedRingBuffer::kInvalidReader;
        }
    }
};

/**
 * @class TelnetSmlApp::Impl
 * @brief Private implementation (PIMPL) of TelnetSmlApp.
 *
 * @details Contains the multi-relay on-demand architecture.  Shared
 * resources (DB, WS server, SHM ring) are owned here.  Per-relay
 * resources (TelnetClient, workers, ring buffer) live inside
 * RelayPipeline objects managed by RelayManager.
 *
 * ## Architecture (Multi-Relay, On-Demand)
 *
 * @code
 * Browser Dashboard
 *       │ "start_relay" / "stop_relay"
 *       ▼
 * SERWebSocketServer (shared, port 8765)
 *       │
 *       ▼
 * RelayManager
 *       │
 *       ├── RelayPipeline[1] (SEL-751)
 *       │      TelnetClient → RingBuffer → ProcessingWorker
 *       │                                    │
 *       │                          ┌─────────┼─────────┐
 *       │                          ▼         ▼         ▼
 *       │                    SharedDB   WS broadcast  SHM ring
 *       │
 *       ├── RelayPipeline[2] (SEL-421)   ... same ...
 *       │
 *       └── RelayPipeline[3] (SEL-451)   ... same ...
 * @endcode
 *
 * ## Component Ownership
 *
 * | Component       | Type                 | Ownership |
 * |-----------------|----------------------|-----------|
 * | serDb           | SERDatabase          | Impl      |
 * | wsServer        | SERWebSocketServer   | Impl      |
 * | shmRing         | SharedRingBuffer     | Impl      |
 * | threadMgr       | ThreadManager        | Impl      |
 * | relayMgr        | RelayManager         | Impl      |
 * | shmReader       | SharedSerReader      | Impl      |
 * | per-relay stuff | RelayPipeline        | relayMgr  |
 *
 * @see TelnetSmlApp      Public interface
 * @see RelayManager       Pipeline lifecycle coordination
 * @see RelayPipeline      Per-relay component bundle
 * @see relay_config.hpp   Static relay configurations
 */
class TelnetSmlApp::Impl
{
public:
    std::atomic<bool> app_running{true};                 ///< Global application running flag

    // ─── Shared resources (owned by Impl) ───────────────────────────
    SERDatabase serDb{"ser_records.db"};                 ///< Single shared SQLite database
    SERWebSocketServer wsServer{serDb, 8765};            ///< Single shared WebSocket server
    std::unique_ptr<WSDBServer> dbApiServer;             ///< Generic DB WebSocket API (port 8766)
    SharedRingBuffer shmRing{"TelnetSmlShmRing", 500U * 1024U}; ///< Shared ring buffer (500KB)

    ThreadManager threadMgr{serDb, std::chrono::seconds(120)};   ///< Poller (2 min interval)
    SharedSerReader shmReader{shmRing, "ui/data.json"};          ///< JSON file writer thread

    // ─── Multi-relay management ─────────────────────────────────────
    std::unique_ptr<RelayManager> relayMgr;              ///< On-demand pipeline coordinator

    // ─── Background TAR collection & cache ───────────────────────────
    std::mutex tarCacheMutex_;                                        ///< Guards TAR cache
    std::unordered_map<std::string, std::string> tarCache_;           ///< relay_id → TAR_BATCH_ALL JSON
    std::unordered_map<std::string, bool> tarFetchInProgress_;        ///< relay_id → in-progress flag
    std::condition_variable tarCacheCv_;                               ///< Notifies when TAR fetch completes
    std::vector<std::thread> tarBgThreads_;                            ///< Background TAR collection threads

    bool running = false;                                ///< Application running state

    /**
     * @brief Extract a string field value from a simple JSON object.
     *
     * @param json       Raw JSON string.
     * @param fieldName  Field name to look up (without quotes).
     * @return Extracted value string, or empty string on failure.
     */
    /**
     * @brief Collect all TAR data for a relay in the background.
     *
     * @details Reuses the same TAR 0..N loop as FETCH_ALL_TAR but runs
     * in a background thread.  Result is cached so future requests
     * return instantly.
     *
     * @param relayId  Relay identifier to collect TAR data from
     */
    void collectTarBackground(const std::string& relayId)
    {
        {
            std::lock_guard<std::mutex> lock(tarCacheMutex_);
            // Already cached or another thread is collecting — bail out
            if (tarCache_.count(relayId) || tarFetchInProgress_[relayId])
                return;
            tarFetchInProgress_[relayId] = true;   // atomic check-and-set under lock
        }

        std::cout << "[TAR-BG] Starting background TAR collection for relay " << relayId << "\n";

        // Retry loop — relay may not be ready immediately after startup.
        // Wait 3s between attempts, up to 5 retries.
        constexpr int MAX_RETRIES = 5;
        constexpr int RETRY_DELAY_SEC = 3;

        for (int attempt = 0; attempt <= MAX_RETRIES && app_running.load(); ++attempt)
        {
            if (attempt > 0)
            {
                std::cout << "[TAR-BG] Relay " << relayId << " returned 0 rows — retry "
                          << attempt << "/" << MAX_RETRIES << " in " << RETRY_DELAY_SEC << "s\n";
                for (int s = 0; s < RETRY_DELAY_SEC && app_running.load(); ++s)
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                if (!app_running.load()) break;
            }

            int count = 0;
            std::string batch;
            batch.reserve(128 * 1024);  // pre-allocate ~128KB
            batch = "TAR_BATCH_ALL:[";
            bool first = true;

            for (int i = 0; app_running.load(); ++i)
            {
                std::string response = relayMgr->handleUserCommand(
                    relayId, "TAR " + std::to_string(i));
                if (response.empty())
                    break;

                // Stop when relay signals no more TAR rows
                if (response.find("Invalid Target") != std::string::npos)
                    break;

                std::string escaped = escapeTarJson(response);
                std::string entry = "{\"idx\":" + std::to_string(i)
                                  + ",\"data\":\"" + escaped + "\"}";

                if (!first) batch += ",";
                first = false;
                batch += entry;

                ++count;
            }

            // Got 0 rows — relay not ready yet, retry
            if (count == 0)
                continue;

            batch += "]";

            {
                std::lock_guard<std::mutex> lock(tarCacheMutex_);
                tarCache_[relayId] = batch;
                tarFetchInProgress_[relayId] = false;
            }
            tarCacheCv_.notify_all();

            // Push final complete TAR data to all connected clients
            wsServer.broadcastText(batch);
            std::cout << "[TAR-BG] Background TAR collection complete for relay "
                      << relayId << " (" << count << " rows, pushed to "
                      << wsServer.clientCount() << " clients)\n";
            return;  // success — done
        }

        // All retries exhausted or app shutting down
        {
            std::lock_guard<std::mutex> lock(tarCacheMutex_);
            tarFetchInProgress_[relayId] = false;
        }
        tarCacheCv_.notify_all();
        std::cout << "[TAR-BG] Background TAR gave up for relay " << relayId
                  << " after " << MAX_RETRIES << " retries — FETCH_ALL_TAR will collect on demand\n";
    }

    static std::string extractJsonField(const std::string& json, const std::string& fieldName)
    {
        const std::string key = "\"" + fieldName + "\"";
        auto pos = json.find(key);
        if (pos == std::string::npos) return "";
        pos = json.find(':', pos + key.size());
        if (pos == std::string::npos) return "";
        pos = json.find('"', pos + 1);
        if (pos == std::string::npos) return "";
        auto end = json.find('"', pos + 1);
        if (end == std::string::npos) return "";
        return json.substr(pos + 1, end - pos - 1);
    }

    /**
     * @brief Starts all application components.
     *
     * @details Initialization sequence:
     * 1. Open shared SQLite database
     * 2. Export existing data to JSON
     * 3. Set up WS command & action handlers
     * 4. Start shared WebSocket server
     * 5. Start DB API server
     * 6. Create RelayManager
     * 7. Start SharedSerReader (JSON writer)
     * 8. Start ThreadManager (polling)
     * 9. No relay connections are made until user clicks a relay card
     *
     * @return true on success, false if any shared component fails
     */
    bool start()
    {
        if (running)
            return true;

        app_running = true;

        std::cout << "========================================\n";
        std::cout << "  Telnet-SML Multi-Relay Application\n";
        std::cout << "  (On-Demand Architecture v3.0)\n";
        std::cout << "========================================\n\n";

        // 1. Open shared database
        if (!serDb.open())
        {
            std::cerr << "Failed to open database: " << serDb.getLastError() << "\n";
            return false;
        }
        std::cout << "[DB] Database opened. Existing records: " << serDb.getRecordCount() << "\n";

        // 2. Export existing records to data.json
        {
            auto existing = serDb.getAllRecords();
            std::string json = recordsToJsonTable(existing);
            std::string err;
            if (writeJsonToFile("ui/data.json", json, &err))
                std::cout << "[Init] Exported " << existing.size() << " records to ui/data.json\n";
            else
                std::cerr << "[Init] data.json export failed: " << err << "\n";
        }

        // 3. Create RelayManager (no pipelines started yet — on-demand)
        relayMgr = std::make_unique<RelayManager>(
            serDb, wsServer, shmRing, app_running);
        std::cout << "[RelayMgr] Initialized with " << relayMgr->getConfigs().size()
                  << " relay configurations\n";

        // 4. Set up WS command handler — routes commands to the correct relay pipeline
        //    Browser sends: "relay_id:command" (e.g. "1:SER", "2:TAR 0", "2:EVE")
        //    Fallback: if no colon, try to route to first active relay
        wsServer.setCommandHandler([this](const std::string& cmd) -> std::string {
            auto colon = cmd.find(':');
            if (colon != std::string::npos)
            {
                std::string relayId = cmd.substr(0, colon);
                std::string realCmd = cmd.substr(colon + 1);
                std::cout << "[WS→Relay] Routing '" << realCmd << "' to relay " << relayId << " via Command FSM\n";
                return relayMgr->handleUserCommand(relayId, realCmd);
            }
            else
            {
                // No relay prefix — try first active relay
                auto ids = relayMgr->getActiveRelayIds();
                if (!ids.empty())
                {
                    std::cout << "[WS→Relay] Routing '" << cmd << "' to relay " << ids.front() << " via Command FSM\n";
                    return relayMgr->handleUserCommand(ids.front(), cmd);
                }
                return "";
            }
        });

        // 4b. Set up streaming command handler — FETCH_ALL_TAR loops TAR 0..N on the server
        wsServer.setStreamCommandHandler(
            [this](const std::string& cmd,
                   WebSocketSession::StreamingCallback sendFn,
                   std::atomic<bool>& abort) -> bool
        {
            // Parse relay prefix (e.g. "3:FETCH_ALL_TAR")
            auto colon = cmd.find(':');
            std::string relayId, realCmd;
            if (colon != std::string::npos)
            {
                relayId = cmd.substr(0, colon);
                realCmd = cmd.substr(colon + 1);
            }
            else
            {
                auto ids = relayMgr->getActiveRelayIds();
                if (ids.empty()) return false;
                relayId = ids.front();
                realCmd = cmd;
            }

            if (realCmd != "FETCH_ALL_TAR")
                return false;   // not a streaming command — fall through

            // Try to serve from background cache first
            {
                std::unique_lock<std::mutex> lock(tarCacheMutex_);
                // Wait for in-progress background fetch to complete (up to 10 min)
                tarCacheCv_.wait_for(lock, std::chrono::minutes(10), [&]() {
                    return tarCache_.count(relayId) > 0 || !tarFetchInProgress_[relayId];
                });

                auto it = tarCache_.find(relayId);
                if (it != tarCache_.end())
                {
                    std::cout << "[WS→Relay] Serving cached TAR_BATCH_ALL for relay " << relayId
                              << " (" << it->second.size() << " bytes)\n";
                    sendFn(it->second);
                    return true;
                }
            }

            // Fallback: collect directly (no cached data available)
            {
                std::lock_guard<std::mutex> lock(tarCacheMutex_);
                if (tarFetchInProgress_[relayId])
                    return false;   // background fetch still running — don't duplicate
                tarFetchInProgress_[relayId] = true;
            }
            std::cout << "[WS→Relay] FETCH_ALL_TAR batch-collecting for relay " << relayId << "\n";
            int count = 0;

            // Collect all TAR responses into a JSON array, then send as one batch
            std::string batch;
            batch.reserve(128 * 1024);  // pre-allocate ~128KB
            batch = "TAR_BATCH_ALL:[";
            bool first = true;

            for (int i = 0; !abort.load(); ++i)
            {
                std::string response = relayMgr->handleUserCommand(
                    relayId, "TAR " + std::to_string(i));
                if (response.empty())
                    break;  // no more rows — relay returned nothing

                // Stop when relay signals no more TAR rows
                if (response.find("Invalid Target") != std::string::npos)
                    break;

                std::string escaped = escapeTarJson(response);
                std::string entry = "{\"idx\":" + std::to_string(i) + ",\"data\":\"" + escaped + "\"}";

                if (!first) batch += ",";
                first = false;
                batch += entry;

                ++count;
            }

            batch += "]";

            // Cache the result (only non-empty) and clear in-progress flag
            {
                std::lock_guard<std::mutex> lock(tarCacheMutex_);
                if (count > 0)
                    tarCache_[relayId] = batch;
                tarFetchInProgress_[relayId] = false;
            }
            tarCacheCv_.notify_all();

            // Always send TAR_BATCH_ALL so the frontend promise resolves
            // (even if empty — prevents the UI from hanging on timeout)
            std::cout << "[WS→Relay] Sending TAR_BATCH_ALL (" << batch.size() << " bytes, " << count << " rows)\n";
            sendFn(batch);
            std::cout << "[WS→Relay] FETCH_ALL_TAR done — " << count << " rows batched\n";
            return true;
        });

        // 5. Set up action handler — relay lifecycle + time sync + password + getTarData
        wsServer.setActionHandler([this](const std::string& jsonMsg) -> std::string {
            std::string action = extractJsonField(jsonMsg, "action");

            // ── Relay lifecycle management ──
            if (action == "start_relay")
            {
                std::string relayId = extractJsonField(jsonMsg, "relay_id");
                std::cout << "[WS->Action] Starting relay " << relayId << "\n";
                bool ok = relayMgr->startRelay(relayId);
                if (ok)
                {
                    // Only spawn background TAR collection if not already cached or in progress
                    bool shouldCollect = false;
                    {
                        std::lock_guard<std::mutex> lock(tarCacheMutex_);
                        if (!tarCache_.count(relayId) && !tarFetchInProgress_[relayId])
                            shouldCollect = true;
                    }
                    if (shouldCollect)
                    {
                        tarBgThreads_.emplace_back([this, relayId]() {
                            collectTarBackground(relayId);
                        });
                    }
                }
                return ok
                    ? "{\"action\":\"start_relay\",\"relay_id\":\"" + relayId + "\",\"status\":\"success\"}"
                    : "{\"action\":\"start_relay\",\"relay_id\":\"" + relayId + "\",\"status\":\"failed\",\"error\":\"Unknown relay or start failed\"}";
            }

            if (action == "stop_relay")
            {
                std::string relayId = extractJsonField(jsonMsg, "relay_id");
                std::cout << "[WS->Action] Stopping relay " << relayId << "\n";
                // Clear cached TAR data for this relay
                {
                    std::lock_guard<std::mutex> lock(tarCacheMutex_);
                    tarCache_.erase(relayId);
                    tarFetchInProgress_.erase(relayId);
                }
                bool ok = relayMgr->stopRelay(relayId);
                return ok
                    ? "{\"action\":\"stop_relay\",\"relay_id\":\"" + relayId + "\",\"status\":\"success\"}"
                    : "{\"action\":\"stop_relay\",\"relay_id\":\"" + relayId + "\",\"status\":\"failed\",\"error\":\"Relay not active\"}";
            }

            if (action == "relay_status")
            {
                std::string result = "{\"action\":\"relay_status\",\"active\":[";
                auto ids = relayMgr->getActiveRelayIds();
                for (size_t i = 0; i < ids.size(); ++i)
                {
                    if (i > 0) result += ",";
                    result += "\"" + ids[i] + "\"";
                }
                result += "]}";
                return result;
            }

            // ── Time sync — read_time / sync_time / sntp_sync ──
            if (action == "read_time" || action == "sync_time" || action == "sntp_sync")
            {
                std::string relayId = extractJsonField(jsonMsg, "relay_id");
                if (relayId.empty())
                    return "{\"action\":\"" + action + "\",\"status\":\"failed\",\"error\":\"Missing relay_id\"}";

                auto* pipeline = relayMgr->getPipeline(relayId);
                if (!pipeline)
                    return "{\"action\":\"" + action + "\",\"status\":\"failed\",\"error\":\"Relay not active\"}";

                // Route commands through pipeline (thread-safe)
                auto sender = [pipeline](const std::string& cmd) {
                    return pipeline->handleUserCommand(cmd);
                };
                auto batchSender = [pipeline](const std::vector<std::string>& cmds) {
                    return pipeline->handleUserCommandBatch(cmds);
                };
                TimeSyncManager tsm(sender, batchSender);

                std::string sntpServer = extractJsonField(jsonMsg, "sntp_server");
                return tsm.handleAction(action, sntpServer, pipeline->config().password);
            }

            // ── Password change (requires relay_id to know which relay) ──
            // TODO: Route to per-relay PasswordManager when available
            if (action == "change_password")
            {
                std::cout << "[WS->Action] PasswordManager not yet per-relay\n";
                return "{\"action\":\"change_password\",\"status\":\"failed\",\"error\":\"Password change requires per-relay support (coming soon)\"}";
            }

            std::cout << "[WS->Action] Unknown action: " << action << "\n";
            return "{\"status\":\"failed\",\"error\":\"Unknown action\"}";
        });

        // 6. Start WebSocket server
        if (!wsServer.start())
        {
            std::cerr << "Failed to start WebSocket server\n";
            serDb.close();
            return false;
        }

        // 7. Generic database WebSocket API on port 8766 (disabled — not used by any front-end)
        // dbApiServer = std::make_unique<WSDBServer>(serDb.getDbHandle(), 8766);
        // if (!dbApiServer->start())
        //     std::cerr << "[WSDB] Warning: DB API server failed to start\n";

        // 8. Polling callback — queue SER to ALL active relays
        threadMgr.setPollingCallback([this]() {
            if (!app_running.load())
                return;

            auto ids = relayMgr->getActiveRelayIds();
            if (ids.empty())
                return;

            std::cout << "[Poller] Requesting SER from " << ids.size() << " active relay(s)...\n";
            for (const auto& id : ids)
                relayMgr->queueCommand(id, "SER");
        });

        // 9. Start support threads
        shmReader.start();
        threadMgr.startAll();

        // ─── Summary ─────────────────────────────────────────────
        std::cout << "\n========================================\n";
        std::cout << "  Multi-Relay On-Demand Architecture\n";
        std::cout << "  Configured relays: " << relayMgr->getConfigs().size() << "\n";
        for (const auto& cfg : relayMgr->getConfigs())
        {
            std::cout << "    [" << cfg.id << "] " << cfg.name
                      << " @ " << cfg.host << ":" << cfg.port
                      << " (" << cfg.substation << " / " << cfg.bay << ")\n";
        }
        std::cout << "\n  Shared Services:\n";
        std::cout << "    - WebSocket Server: ws://localhost:8765\n";
        // std::cout << "    - DB API Server:    ws://localhost:8766\n";
        std::cout << "    - SQLite Database:  ser_records.db\n";
        std::cout << "    - Poller:           2 min interval\n";
        std::cout << "    - JSON Writer:      ui/data.json\n";
        std::cout << "\n  Auto-starting all configured relays...\n";
        std::cout << "========================================\n";

        // 10. Auto-start all configured relays and begin TAR collection
        for (const auto& cfg : relayMgr->getConfigs())
        {
            std::cout << "[Auto-TAR] Auto-starting relay " << cfg.id
                      << " (" << cfg.name << ") and scheduling TAR collection\n";
            bool ok = relayMgr->startRelay(cfg.id);
            if (ok)
            {
                tarBgThreads_.emplace_back([this, id = cfg.id]() {
                    collectTarBackground(id);
                });
            }
            else
            {
                std::cerr << "[Auto-TAR] Failed to start relay " << cfg.id << "\n";
            }
        }

        running = true;
        return true;
    }

    /**
     * @brief Blocks until user requests exit.
     */
    void waitForExit()
    {
        std::cout << "\n[INFO] Press Enter to exit...\n";
        std::cin.get();
    }

    /**
     * @brief Stops all application components gracefully.
     *
     * @details Shutdown sequence:
     * 1. app_running = false
     * 2. Stop all relay pipelines (via RelayManager)
     * 3. Stop ThreadManager (polling)
     * 4. Stop SharedSerReader
     * 5. Stop DB API server
     * 6. Stop WebSocket server
     * 7. Close database
     */
    void stop()
    {
        if (!running)
            return;

        std::cout << "\n[Main] Shutting down...\n";
        app_running = false;

        // Stop all relay pipelines first
        if (relayMgr)
            relayMgr->stopAll();

        // Join background TAR collection threads
        for (auto& t : tarBgThreads_)
        {
            if (t.joinable())
                t.join();
        }
        tarBgThreads_.clear();

        threadMgr.stopAll();
        shmReader.stop();
        // if (dbApiServer) dbApiServer->stop();
        wsServer.stop();
        serDb.close();

        running = false;
        std::cout << "[Main] Application terminated.\n";
    }
};

// ============================================================================
// TelnetSmlApp Public Interface Implementation
// ============================================================================

/**
 * @brief Constructs a TelnetSmlApp instance.
 *
 * @details Creates the private implementation (PIMPL) object.
 * No connections are made until start() is called.
 */
TelnetSmlApp::TelnetSmlApp() : impl_(std::make_unique<Impl>())
{
}

/**
 * @brief Destroys the TelnetSmlApp instance.
 *
 * @details Ensures graceful shutdown by calling stop() if still running.
 */
TelnetSmlApp::~TelnetSmlApp()
{
    if (impl_)
        impl_->stop();
}

/**
 * @brief Starts the Telnet-SML application.
 *
 * @details Initializes and starts all components:
 * - Database connection
 * - WebSocket server
 * - Worker threads (Reception, Processing, SHM Reader)
 * - Polling thread
 *
 * @return true on success, false if initialization fails
 */
bool TelnetSmlApp::start()
{
    return impl_->start();
}

/**
 * @brief Waits for user to request application exit.
 *
 * @details Blocks the calling thread until Enter key is pressed.
 * Typically called from main() after start().
 */
void TelnetSmlApp::waitForExit()
{
    impl_->waitForExit();
}

/**
 * @brief Stops the Telnet-SML application.
 *
 * @details Gracefully shuts down all components in reverse order.
 * Safe to call multiple times.
 */
void TelnetSmlApp::stop()
{
    impl_->stop();
}

/**
 * @brief Checks if the application is currently running.
 *
 * @return true if running, false otherwise
 */
bool TelnetSmlApp::isRunning() const
{
    return impl_ && impl_->running;
}
