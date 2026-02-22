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

/**
 * @class ReceptionWorker
 * @brief Non-blocking reception thread that manages relay communication.
 *
 * @details The ReceptionWorker runs as Thread 1 in the application architecture.
 * It handles all Telnet communication with the substation relay device and
 * immediately pushes responses to the SPSC ring buffer without blocking.
 *
 * ## Responsibilities
 * - Establish and maintain TCP connection to relay (port 23)
 * - Execute login sequence (Level 1 authentication)
 * - Process command queue and execute sequentially
 * - Push command/response pairs to RawDataRingBuffer
 *
 * ## Thread Safety
 * - Uses mutex-protected command queue
 * - Condition variable for efficient waiting
 * - Atomic stop flag for clean shutdown
 *
 * ## Data Flow
 * @dot
 * digraph Reception {
 *     rankdir=LR;
 *     Relay [shape=ellipse, label="Relay Device"];
 *     Client [label="TelnetClient"];
 *     Ring [label="RawDataRingBuffer\n(SPSC)"];
 *
 *     Relay -> Client [label="TCP:23"];
 *     Client -> Ring [label="push\n(non-blocking)"];
 * }
 * @enddot
 *
 * @note This class is internal to the implementation file.
 * @see ProcessingWorker Consumer of ring buffer data
 * @see RawDataRingBuffer SPSC queue for command/response pairs
 */
class ReceptionWorker
{
    std::atomic<bool> stop_flag_{false};       ///< Flag to signal worker thread termination
    std::thread worker_thread_;                 ///< Worker thread handle
    std::mutex queue_mutex_;                    ///< Mutex protecting command_queue_
    std::condition_variable queue_cv_;          ///< CV for command queue notification
    std::deque<std::string> command_queue_;     ///< Queue of commands pending execution

    TelnetClient& client_;                      ///< Reference to Telnet client for I/O
    ConnectionConfig& conn_;                    ///< Connection configuration (host, port, timeout)
    LoginConfig& creds_;                        ///< Login credentials (user, password)
    RawDataRingBuffer& rawBuffer_;              ///< SPSC ring buffer for output
    std::atomic<bool>& app_running_;            ///< Application running state flag

    bool connected_ = false;                    ///< Current TCP connection state
    bool logged_in_ = false;                    ///< Current login state

    /**
     * @brief Ensures connection and login to relay device.
     *
     * @details Executes the following sequence if not already connected:
     * 1. Establish TCP connection to relay
     * 2. Perform Level 1 login authentication
     *
     * Connection state is cached to avoid redundant operations.
     *
     * @return true if connected and logged in, false on failure
     * @note Resets connection state on login failure
     */
    bool ensureConnected()
    {
        if (connected_ && logged_in_)
            return true;
        
        if (!connected_)
        {
            std::cout << "[Reception] Connecting to " << conn_.host << ":" << conn_.port << "...\n";
            connected_ = client_.connectCheck(conn_.host, conn_.port, conn_.timeout);
            if (!connected_)
            {
                std::cout << "[Reception] Connection failed\n";
                return false;
            }
            std::cout << "[Reception] Connected\n";
        }
        
        if (!logged_in_)
        {
            std::cout << "[Reception] Logging in...\n";
            logged_in_ = client_.LoginLevel1Function(creds_.l1_user, creds_.l1_pass);
            if (!logged_in_)
            {
                std::cout << "[Reception] Login failed\n";
                connected_ = false;
                return false;
            }
            std::cout << "[Reception] Logged in\n";
        }

        return true;
    }

    static constexpr int MAX_RETRIES = 3;           ///< Max retry attempts per command
    static constexpr int RETRY_DELAY_SEC = 5;        ///< Delay between retries (seconds)

    /**
     * @brief Executes a command with automatic retry on failure.
     *
     * @details This method:
     * 1. Attempts to connect + login (with retries)
     * 2. Sends command to relay device
     * 3. On failure: resets connection, waits, retries (up to MAX_RETRIES)
     * 4. Pushes command/response pair to RawDataRingBuffer (non-blocking)
     *
     * ## Retry Flow
     * ```
     * Attempt 1 -> connect -> login -> send cmd
     *   fail? -> reset connection -> wait 5s
     * Attempt 2 -> connect -> login -> send cmd
     *   fail? -> reset connection -> wait 5s
     * Attempt 3 -> connect -> login -> send cmd
     *   fail? -> give up (next poller cycle will retry)
     * ```
     *
     * @param cmd The command string to execute (e.g., "SER")
     *
     * @note Push is non-blocking; if buffer is full, oldest entry is overwritten
     * @note Connection is reset on command failure for auto-recovery
     */
    void executeCommand(const std::string& cmd)
    {
        std::cout << "[Reception] executeCommand called for: " << cmd << "\n";

        for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt)
        {
            if (stop_flag_.load())
                return;

            // --- Connection + Login ---
            if (!ensureConnected())
            {
                std::cout << "[Reception] Connection failed (attempt " << attempt
                          << "/" << MAX_RETRIES << ")\n";
                connected_ = false;
                logged_in_ = false;

                if (attempt < MAX_RETRIES)
                {
                    std::cout << "[Reception] Retrying in " << RETRY_DELAY_SEC << "s...\n";
                    // Interruptible sleep — wake up early if app is stopping
                    for (int s = 0; s < RETRY_DELAY_SEC && !stop_flag_.load(); ++s)
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                continue;
            }

            // --- Send Command ---
            std::string response;
            std::cout << "[Reception] Sending command to relay (attempt " << attempt
                      << "/" << MAX_RETRIES << ")...\n";
            bool ok = client_.SendCmdReceiveData(cmd, response);

            std::cout << "[Reception] SendCmdReceiveData returned: " << (ok ? "OK" : "FAIL")
                      << ", response size: " << response.size() << "\n";

            if (ok && !response.empty())
            {
                // Success — push to ring buffer
                bool pushed = rawBuffer_.push({cmd, response});
                std::cout << "[Reception] Pushed to ring buffer: " << (pushed ? "OK" : "OVERWRITE")
                          << ", " << response.size() << " bytes for '" << cmd << "'\n";
                std::cout << "[Reception] Ring buffer size now: " << rawBuffer_.size() << "\n";
                return;  // Done — exit retry loop
            }

            // --- Command failed ---
            std::cout << "[Reception] Command '" << cmd << "' failed or empty response (attempt "
                      << attempt << "/" << MAX_RETRIES << ")\n";
            connected_ = false;
            logged_in_ = false;

            if (attempt < MAX_RETRIES)
            {
                std::cout << "[Reception] Retrying in " << RETRY_DELAY_SEC << "s...\n";
                for (int s = 0; s < RETRY_DELAY_SEC && !stop_flag_.load(); ++s)
                    std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        std::cout << "[Reception] All " << MAX_RETRIES << " attempts failed for '" << cmd
                  << "', will retry on next poll cycle\n";
    }

    /**
     * @brief Main worker thread loop.
     *
     * @details Continuously processes commands from the queue:
     * 1. Wait on condition variable for new commands
     * 2. Dequeue and execute each command
     * 3. Exit when stop_flag_ is set
     *
     * @note Thread-safe via mutex and condition variable
     */
    void runLoop()
    {
        std::cout << "[Reception] Worker thread started\n";
        while (!stop_flag_.load())
        {
            std::string cmd;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                std::cout << "[Reception] Waiting for command in queue...\n";
                queue_cv_.wait(lock, [&]() {
                    return stop_flag_.load() || !command_queue_.empty();
                });
                
                if (stop_flag_.load())
                    break;
                
                if (command_queue_.empty())
                    continue;
                
                cmd = std::move(command_queue_.front());
                command_queue_.pop_front();
                std::cout << "[Reception] Got command from queue: " << cmd << "\n";
            }
            
            executeCommand(cmd);
        }
        std::cout << "[Reception] Worker thread exiting\n";
    }

public:
    /**
     * @brief Constructs a ReceptionWorker instance.
     *
     * @param client Reference to TelnetClient for relay communication
     * @param conn Connection configuration (host, port, timeout)
     * @param creds Login credentials for Level 1 authentication
     * @param rawBuffer SPSC ring buffer for command/response output
     * @param app_running Application state flag for coordinated shutdown
     */
    ReceptionWorker(TelnetClient& client,
                    ConnectionConfig& conn,
                    LoginConfig& creds,
                    RawDataRingBuffer& rawBuffer,
                    std::atomic<bool>& app_running)
        : client_(client)
        , conn_(conn)
        , creds_(creds)
        , rawBuffer_(rawBuffer)
        , app_running_(app_running)
    {
    }

    /**
     * @brief Starts the worker thread.
     *
     * @details Resets stop flag and spawns worker thread running runLoop().
     */
    void start()
    {
        stop_flag_ = false;
        worker_thread_ = std::thread([this]() { runLoop(); });
    }

    void stop()
    {
        stop_flag_ = true;
        queue_cv_.notify_all();
        rawBuffer_.notifyAll();
        if (worker_thread_.joinable())
            worker_thread_.join();
    }

    /**
     * @brief Queues a command for asynchronous execution.
     *
     * @details Adds command to the queue and notifies worker thread.
     * This is a non-blocking operation; the command will be executed
     * asynchronously by the worker thread.
     *
     * @param cmd Command string to queue (e.g., "SER", "ACC")
     *
     * @note Thread-safe; can be called from any thread
     */
    void queueCommand(const std::string& cmd)
    {
        std::cout << "[Reception] queueCommand called: " << cmd << "\n";
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            command_queue_.push_back(cmd);
            std::cout << "[Reception] Command queued, queue size: " << command_queue_.size() << "\n";
        }
        queue_cv_.notify_one();
    }

    /**
     * @brief Resets connection state for reconnection.
     *
     * @details Clears connected and logged_in flags, forcing
     * re-establishment of connection on next command.
     */
    void resetConnection()
    {
        connected_ = false;
        logged_in_ = false;
    }
};

/**
 * @class ProcessingWorker
 * @brief FSM/Processing thread that processes relay data and persists to storage.
 *
 * @details The ProcessingWorker runs as Thread 2 in the application architecture.
 * It continuously reads from the SPSC ring buffer, processes SER data, and
 * coordinates the sequential pipeline for data persistence and distribution.
 *
 * ## Responsibilities
 * - Read command/response pairs from RawDataRingBuffer
 * - Parse SER response text into structured records
 * - Store validated records in SQLite database
 * - Broadcast new records via WebSocket (only after DB success)
 * - Write to shared memory for JSON file output
 *
 * ## Data Flow (Sequential Pipeline)
 * @code
 * SPSC Ring Buffer
 *       │
 *       ▼
 * Parse SER Response
 *       │
 *       ▼
 * SQLite (insertRecords)
 *       │
 *       ▼ (on success only)
 * WebSocket Broadcast
 *       │
 *       ▼
 * SharedRingBuffer (JSON writer)
 * @endcode
 *
 * ## Thread Safety
 * - Blocking wait on SPSC ring buffer
 * - Atomic stop flag for clean shutdown
 * - Database and WebSocket operations are thread-safe
 *
 * @dot
 * digraph Processing {
 *     rankdir=TB;
 *     Ring [label="SPSC Ring Buffer"];
 *     Parse [label="parseSERResponse()"];
 *     DB [label="SQLite\n(Persistent Storage)"];
 *     WS [label="WebSocket Server"];
 *     SHM [label="SharedRingBuffer"];
 *
 *     Ring -> Parse;
 *     Parse -> DB;
 *     DB -> WS [label="on success"];
 *     WS -> SHM;
 * }
 * @enddot
 *
 * @note This class is internal to the implementation file.
 * @see ReceptionWorker Producer of ring buffer data
 * @see SERDatabase SQLite persistence layer
 * @see SERWebSocketServer WebSocket broadcast server
 */
class ProcessingWorker
{
    std::atomic<bool> stop_flag_{false};       ///< Flag to signal worker thread termination
    std::thread worker_thread_;                 ///< Worker thread handle

    RawDataRingBuffer& rawBuffer_;              ///< SPSC ring buffer input
    SERDatabase& db_;                           ///< SQLite database for persistence
    SERWebSocketServer& wsServer_;              ///< WebSocket server for broadcasting
    SharedRingBuffer& shmRing_;                 ///< Shared memory ring for JSON output
    std::atomic<bool>& app_running_;            ///< Application running state flag

    /**
     * @brief Main worker thread loop.
     *
     * @details Continuously processes data from ring buffer:
     * 1. Wait for data (blocking)
     * 2. Parse SER response into records
     * 3. Insert records into SQLite
     * 4. Broadcast via WebSocket (on success)
     * 5. Write to shared memory for JSON output
     */
    void runLoop()
    {
        std::cout << "[Processing] Worker thread started\n";
        while (!stop_flag_.load() && app_running_.load())
        {
            std::cout << "[Processing] Waiting for data from ring buffer...\n";
            auto msg = rawBuffer_.waitPop(stop_flag_);
            if (!msg)
            {
                std::cout << "[Processing] waitPop returned empty (stop signal?)\n";
                continue;
            }
            
            std::cout << "[Processing] Received message: cmd='" << msg->command 
                      << "', response size=" << msg->response.size() << " bytes\n";
            
            // Only process SER command responses through the SER pipeline
            if (msg->command != "SER")
            {
                // Non-SER command (e.g. FIL DIR) — broadcast raw text response to all WS clients
                std::cout << "[Processing] Non-SER command '" << msg->command 
                          << "', broadcasting text response (" << msg->response.size() << " bytes)\n";
                wsServer_.broadcastText(msg->response);
                continue;
            }
            
            // Parse SER response
            std::cout << "[Processing] Parsing SER response...\n";
            auto records = parseSERResponse(msg->response);
            if (records.empty())
            {
                std::cout << "[Processing] No records parsed from response. First 200 chars:\n";
                std::cout << msg->response.substr(0, 200) << "\n";
                continue;
            }
            
            std::cout << "[Processing] Parsed " << records.size() << " SER records\n";
            
            // Step 1: SQLite (Persistent Storage)
            int inserted = db_.insertRecords(records);
            std::cout << "[Processing] SQLite: Stored " << inserted << " new records\n";
            
            // Step 2: WebSocket Server (After DB Write) - always broadcast so UI stays current
            // Even when inserted==0 (all duplicates), connected browsers need the data
            // (e.g. browser connected after app restart with existing DB)
            wsServer_.broadcast(records);
            std::cout << "[Processing] WebSocket: Broadcast " << records.size() << " records to clients\n";
            
            // Write to shared memory for JSON file writer (only on new data)
            if (inserted > 0)
            {
                auto payload = asn_tlv::encodeSerRecordsToTlv(records);
                if (!payload.empty())
                {
                    shmRing_.write(payload.data(), payload.size());
                }
            }
        }
        std::cout << "[Processing] Worker thread exiting\n";
    }

public:
    /**
     * @brief Constructs a ProcessingWorker instance.
     *
     * @param rawBuffer SPSC ring buffer for command/response input
     * @param db SQLite database for record persistence
     * @param wsServer WebSocket server for broadcasting to clients
     * @param shmRing Shared ring buffer for JSON file writer output
     * @param app_running Application state flag for coordinated shutdown
     */
    ProcessingWorker(RawDataRingBuffer& rawBuffer,
                     SERDatabase& db,
                     SERWebSocketServer& wsServer,
                     SharedRingBuffer& shmRing,
                     std::atomic<bool>& app_running)
        : rawBuffer_(rawBuffer)
        , db_(db)
        , wsServer_(wsServer)
        , shmRing_(shmRing)
        , app_running_(app_running)
    {
    }

    /**
     * @brief Starts the worker thread.
     *
     * @details Resets stop flag and spawns worker thread running runLoop().
     */
    void start()
    {
        stop_flag_ = false;
        worker_thread_ = std::thread([this]() { runLoop(); });
    }

    /**
     * @brief Stops the worker thread gracefully.
     *
     * @details Sets stop flag, notifies ring buffer, and joins worker thread.
     */
    void stop()
    {
        stop_flag_ = true;
        rawBuffer_.notifyAll();
        if (worker_thread_.joinable())
            worker_thread_.join();
    }
};

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
 * @details This class contains the actual implementation of the Telnet-SML
 * application, following the PIMPL idiom for ABI stability and compilation
 * firewall.
 *
 * ## Architecture
 * The implementation manages the complete multi-threaded pipeline:
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
 *       ├──▶ SQLite (Persistent Storage)
 *       │
 *       ├──▶ WebSocket Server (After DB Write)
 *       │
 *       └──▶ SharedRingBuffer → JSON File
 * @endcode
 *
 * ## Component Ownership
 * | Component | Type | Purpose |
 * |-----------|------|----------|
 * | client | TelnetClient | Relay communication |
 * | serDb | SERDatabase | SQLite persistence |
 * | wsServer | SERWebSocketServer | WebSocket broadcast |
 * | rawBuffer | RawDataRingBuffer | SPSC queue (100 slots) |
 * | shmRing | SharedRingBuffer | JSON output (500KB) |
 * | threadMgr | ThreadManager | 2-min polling |
 *
 * @see TelnetSmlApp Public interface
 */
class TelnetSmlApp::Impl
{
public:
    std::atomic<bool> app_running{true};                 ///< Global application running flag
    TelnetClient client;                                 ///< Telnet client for relay I/O

    /// @brief Connection configuration for relay
    ConnectionConfig conn{
        "192.168.0.2",                                   ///< Relay IP address
        23,                                              ///< Telnet port
        std::chrono::milliseconds(2000)                  ///< Connection timeout
    };

    /// @brief Login credentials for Level 1 authentication
    LoginConfig creds{
        "acc",                                           ///< Username
        "OTTER"                                          ///< Password
    };

    RetryState retry{3, 0, std::chrono::seconds(30)};    ///< Retry configuration
    SERDatabase serDb{"C:/Users/admin/Desktop/telnet/ser_records.db"};                 ///< SQLite database (persistent storage)
    SERWebSocketServer wsServer{serDb, 8765};            ///< WebSocket server (port 8765)
    std::unique_ptr<WSDBServer> dbApiServer;                ///< Generic DB WebSocket API (port 8766)
    ThreadManager threadMgr{serDb, std::chrono::seconds(120)}; ///< Poller (2 min interval)

    // Architecture components
    RawDataRingBuffer rawBuffer{100};                    ///< SPSC ring buffer (100 slots)
    SharedRingBuffer shmRing{"TelnetSmlShmRing", 500U * 1024U}; ///< Shared ring buffer (500KB)

    std::unique_ptr<ReceptionWorker> receptionWorker;    ///< Thread 1: Reception worker
    std::unique_ptr<ProcessingWorker> processingWorker;  ///< Thread 2: Processing worker

    RelayService relayService{client};                   ///< Thread-safe relay command service
    TimeSyncManager timeSyncMgr{relayService};           ///< DATE synchronization orchestrator
    PasswordManager passwordMgr{relayService};           ///< Password change orchestrator

    SharedSerReader shmReader{shmRing, "ui/data.json"}; ///< Thread 5: JSON file writer

    bool running = false;                                ///< Application running state

    /**
     * @brief Seed database with test SER records when empty (relay unavailable).
     *
     * @details Inserts realistic dummy records so the UI pipeline can be
     * verified end-to-end without a live relay connection.  Records are
     * only inserted when the database is empty (getRecordCount() == 0).
     *
     * @return Number of test records inserted (0 if DB already had data)
     */
    // int seedTestData()
    // {
    //     if (serDb.getRecordCount() > 0)
    //         return 0;  // DB already has data, skip seeding

    //     std::cout << "[Seed] Database empty — injecting test SER records...\n";

    //     std::vector<SERRecord> testRecords = {
    //         {"1",  "02/14/22 12:47:19.970", "AssertedD",   "Power loss Phase-A"},
    //         {"2",  "02/14/22 12:47:20.100", "AssertedD",   "Power loss Phase-B"},
    //         {"3",  "02/14/22 12:47:20.250", "AssertedD",   "Power loss Phase-C"},
    //         {"4",  "02/14/22 12:48:05.430", "Deasserted", "Power loss Phase-A"},
    //         {"5",  "02/14/22 12:48:05.580", "Deasserted", "Power loss Phase-B"},
    //         {"6",  "02/14/22 12:48:05.710", "Deasserted", "Power loss Phase-C"},
    //         {"7",  "03/10/22 09:15:32.000", "Asserted",   "Overcurrent Trip Relay 1"},
    //         {"8",  "03/10/22 09:15:32.150", "Asserted",   "Breaker Failure"},
    //         {"9",  "03/10/22 09:16:01.800", "Deasserted", "Overcurrent Trip Relay 1"},
    //         {"10", "03/10/22 09:16:02.000", "Deasserted", "Breaker Failure"},
    //         {"11", "05/22/23 14:30:00.500", "Asserted",   "Communication Failure"},
    //         {"12", "05/22/23 14:35:12.200", "Deasserted", "Communication Failure"},
    //         {"13", "08/01/23 08:00:00.000", "Asserted",   "Undervoltage Phase-A"},
    //         {"14", "08/01/23 08:00:00.100", "Asserted",   "Undervoltage Phase-B"},
    //         {"15", "08/01/23 08:00:15.300", "Deasserted", "Undervoltage Phase-A"},
    //         {"16", "08/01/23 08:00:15.450", "Deasserted", "Undervoltage Phase-B"},
    //         {"17", "11/15/24 22:10:44.600", "Asserted",   "Earth Fault Relay 2"},
    //         {"18", "11/15/24 22:11:00.000", "Deasserted", "Earth Fault Relay 2"},
    //         {"19", "01/05/25 06:30:10.800", "Asserted",   "Over Temperature Alarm"},
    //         {"20", "01/05/25 06:45:30.200", "Deasserted", "Over Temperature Alarm"}
    //     };

    //     int inserted = serDb.insertRecords(testRecords);
    //     std::cout << "[Seed] Inserted " << inserted << " test records into database\n";
    //     return inserted;
    // }

    /**
     * @brief Starts all application components.
     *
     * @details Initialization sequence:
     * 1. Open SQLite database
     * 2. Start WebSocket server
     * 3. Create and start ReceptionWorker (Thread 1)
     * 4. Create and start ProcessingWorker (Thread 2)
     * 5. Configure polling callback
     * 6. Start SharedSerReader (Thread 5)
     * 7. Start ThreadManager (Thread 4)
     * 8. Queue initial SER command
     *
     * @return true on success, false if any component fails to start
     */
    bool start()
    {
        if (running)
            return true;

        app_running = true;

        std::cout << "========================================\n";
        std::cout << "  Telnet-SML Multi-threaded Application\n";
        std::cout << "  (Decoupled Architecture v2.0)\n";
        std::cout << "========================================\n\n";

        if (!serDb.open())
        {
            std::cerr << "Failed to open database: " << serDb.getLastError() << "\n";
            return false;
        }
        std::cout << "[DB] Database opened. Existing records: " << serDb.getRecordCount() << "\n";

        // Seed test data disabled: live relay data only

        // Export data.json from existing DB records
        {
            auto existing = serDb.getAllRecords();
            std::string json = recordsToJsonTable(existing);
            std::string err;
            if (writeJsonToFile("ui/data.json", json, &err))
                std::cout << "[Init] Exported " << existing.size() << " records to ui/data.json\n";
            else
                std::cerr << "[Init] data.json export failed: " << err << "\n";
        }

        // Set up command handler: queue UI commands (FIL DIR etc.) to ReceptionWorker
        // Response will be routed by ProcessingWorker via broadcastText
        wsServer.setCommandHandler([this](const std::string& cmd) -> std::string {
            if (receptionWorker) {
                std::cout << "[WS→Queue] Queuing UI command: " << cmd << "\n";
                receptionWorker->queueCommand(cmd);
            }
            return "";  // Response comes async via ProcessingWorker
        });

        // Set up action handler: JSON actions dispatched to the appropriate manager
        // Handler receives the full JSON message so managers can extract extra fields
        wsServer.setActionHandler([this](const std::string& jsonMsg) -> std::string {
            // Extract "action" field from JSON
            std::string action;
            {
                const std::string key = "\"action\"";
                auto pos = jsonMsg.find(key);
                if (pos != std::string::npos) {
                    pos = jsonMsg.find(':', pos + key.size());
                    if (pos != std::string::npos) {
                        pos = jsonMsg.find('"', pos + 1);
                        if (pos != std::string::npos) {
                            auto end = jsonMsg.find('"', pos + 1);
                            if (end != std::string::npos)
                                action = jsonMsg.substr(pos + 1, end - pos - 1);
                        }
                    }
                }
            }

            // Route to TimeSyncManager
            if (action == "read_time" || action == "sync_time")
            {
                std::cout << "[WS->Action] Routing to TimeSyncManager: " << action << "\n";
                return timeSyncMgr.handleAction(action);
            }

            // Route to PasswordManager (password values are NOT logged)
            if (action == "change_password")
            {
                std::cout << "[WS->Action] Routing to PasswordManager\n";
                return passwordMgr.handleAction(jsonMsg);
            }

            std::cout << "[WS->Action] Unknown action: " << action << "\n";
            return "{\"status\":\"failed\",\"error\":\"Unknown action\"}";
        });

        if (!wsServer.start())
        {
            std::cerr << "Failed to start WebSocket server\n";
            serDb.close();
            return false;
        }

        // Generic database WebSocket API on port 8766
        dbApiServer = std::make_unique<WSDBServer>(serDb.getDbHandle(), 8766);
        if (!dbApiServer->start())
            std::cerr << "[WSDB] Warning: DB API server failed to start\n";

        // Architecture: TelnetClient Thread -> SPSC Ring Buffer -> Processing Thread
        
        // 1. TelnetClient Thread (C++) - handles Telnet I/O, pushes to ring buffer
        receptionWorker = std::make_unique<ReceptionWorker>(
            client, conn, creds, rawBuffer, app_running);
        receptionWorker->start();
        
        // 2. FSM / Processing Thread - reads from ring buffer, writes to DB, then WebSocket
        processingWorker = std::make_unique<ProcessingWorker>(
            rawBuffer, serDb, wsServer, shmRing, app_running);
        processingWorker->start();
        
        // Polling callback requests SER data from TelnetClient Thread
        threadMgr.setPollingCallback([this]() {
            if (!app_running.load())
                return;
            std::cout << "[Poller] Requesting SER data...\n";
            receptionWorker->queueCommand("SER");
        });

        shmReader.start();
        threadMgr.startAll();

        // Initial data request to start the pipeline
        std::cout << "\n[Main] Starting initial SER retrieval...\n";
        receptionWorker->queueCommand("SER");

        std::cout << "\n========================================\n";
        std::cout << "  Active Threads (Architecture):\n";
        std::cout << "  - TelnetClient Thread: Relay communication\n";
        std::cout << "  - FSM/Processing Thread: Parse + DB + WebSocket\n";
        std::cout << "  - WebSocket Server Thread: port 8765\n";
        std::cout << "  - DB API Server Thread:    port 8766\n";
        std::cout << "  - Poller Thread: 2 min interval\n";
        std::cout << "  - JSON Writer Thread: SharedRingBuffer consumer\n";
        std::cout << "========================================\n";
        std::cout << "\n[Architecture] C++ Process Data Flow:\n";
        std::cout << "  Relay Device\n";
        std::cout << "       |\n";
        std::cout << "       v\n";
        std::cout << "  TelnetClient Thread (C++)\n";
        std::cout << "       |\n";
        std::cout << "       v\n";
        std::cout << "  C++ SPSC Ring Buffer\n";
        std::cout << "       |\n";
        std::cout << "       v\n";
        std::cout << "  FSM / Processing Thread\n";
        std::cout << "       |\n";
        std::cout << "       v\n";
        std::cout << "  SQLite (Persistent Storage)\n";
        std::cout << "       |\n";
        std::cout << "       v\n";
        std::cout << "  WebSocket Server (After DB Write)\n";
        std::cout << "========================================\n";
        std::cout << "\n[Architecture] Browser Data Flow:\n";
        std::cout << "  WebSocket Client\n";
        std::cout << "       |\n";
        std::cout << "       v\n";
        std::cout << "  JS Worker (Optional)\n";
        std::cout << "       |\n";
        std::cout << "       v\n";
        std::cout << "  Main JS Thread (DOM Access)\n";
        std::cout << "       |\n";
        std::cout << "       v\n";
        std::cout << "  Tabulator / JSON Export\n";
        std::cout << "========================================\n";

        running = true;
        return true;
    }

    /**
     * @brief Blocks until user requests exit.
     *
     * @details Waits for Enter key press to signal shutdown.
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
     * 1. Set app_running flag to false
     * 2. Stop ThreadManager (polling)
     * 3. Stop ReceptionWorker
     * 4. Stop ProcessingWorker
     * 5. Stop SharedSerReader
     * 6. Stop WebSocket server
     * 7. Close database
     */
    void stop()
    {
        if (!running)
            return;

        std::cout << "\n[Main] Shutting down...\n";
        app_running = false;

        threadMgr.stopAll();
        
        if (receptionWorker)
            receptionWorker->stop();
        if (processingWorker)
            processingWorker->stop();
        
        shmReader.stop();
        if (dbApiServer) dbApiServer->stop();
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
