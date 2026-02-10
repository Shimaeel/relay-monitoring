/**
 * @file main.cpp
 * @brief Main entry point for Telnet-SML Substation Monitoring System
 * 
 * @details This application provides automated monitoring of electrical substation
 * equipment via Telnet protocol, using a Boost.SML finite state machine for 
 * robust connection management and System Event Record (SER) data collection.
 * 
 * ## System Architecture
 * 
 * @dot
 * digraph Architecture {
 *     rankdir=TB;
 *     node [shape=box, style=filled, fillcolor=lightblue];
 *     
 *     subgraph cluster_client {
 *         label="Client Application";
 *         main [label="main.cpp\n(Entry Point)"];
 *         fsm [label="TelnetFSM\n(State Machine)"];
 *         client [label="TelnetClient\n(TCP/Telnet)"];
 *     }
 *     
 *     subgraph cluster_storage {
 *         label="Data Layer";
 *         db [label="SERDatabase\n(SQLite)"];
 *         json [label="data.json\n(UI Export)"];
 *     }
 *     
 *     subgraph cluster_ui {
 *         label="User Interface";
 *         ws [label="WebSocket Server"];
 *         html [label="index.html\n(Web UI)"];
 *     }
 *     
 *     subgraph cluster_external {
 *         label="External";
 *         relay [label="Substation Relay\n(Telnet Server)"];
 *     }
 *     
 *     main -> fsm [label="controls"];
 *     fsm -> client [label="actions"];
 *     client -> relay [label="Telnet TCP:23"];
 *     fsm -> db [label="stores SER"];
 *     db -> json [label="exports"];
 *     db -> ws [label="real-time"];
 *     ws -> html [label="WebSocket"];
 * }
 * @enddot
 * 
 * ## Application Workflow
 * 
 * @msc
 * main,FSM,Client,Relay,Database,WebSocket;
 * main->FSM [label="create"];
 * main->Database [label="open()"];
 * main->WebSocket [label="start()"];
 * main->FSM [label="start_event"];
 * FSM->Client [label="connect()"];
 * Client->Relay [label="TCP connect"];
 * Relay->Client [label="connected"];
 * FSM->Client [label="login()"];
 * Client->Relay [label="credentials"];
 * Relay->Client [label="prompt"];
 * FSM->Client [label="SER command"];
 * Client->Relay [label="SER"];
 * Relay->Client [label="SER records"];
 * FSM->Database [label="insertRecords()"];
 * Client<-WebSocket [label="getData"];
 * WebSocket->Database [label="getAllRecords()"];  
 * @endmsc
 * 
 * ## Key Features
 * - Automatic connection and authentication to substation relays
 * - Retry logic with configurable attempts and delays
 * - SQLite persistence for System Event Records
 * - Real-time WebSocket server for UI updates
 * - Clean state machine architecture using Boost.SML
 * 
 * @see TelnetFSM State machine implementation
 * @see TelnetClient Telnet communication handler
 * @see SERDatabase SQLite database handler
 * @see SERWebSocketServer WebSocket server for UI
 * 
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 * @copyright MIT License
 */

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>

#include "client.hpp"
#include "telnet_fsm.hpp"
#include "ser_database.hpp"
#include "ws_server.hpp"
#include "thread_manager.hpp"

/**
 * @brief Main entry point for the Telnet-SML application
 * 
 * @details Initializes and orchestrates all system components:
 * 1. Creates TelnetClient for network communication
 * 2. Configures connection and authentication parameters
 * 3. Opens SQLite database for SER record storage
 * 4. Starts WebSocket server for real-time UI updates
 * 5. Initializes and runs the FSM to manage the connection workflow
 * 6. Waits for user input before graceful shutdown
 * 
 * ## Configuration Parameters
 * 
 * | Parameter | Default | Description |
 * |-----------|---------|-------------|
 * | Host | 192.168.0.2 | Relay IP address |
 * | Port | 23 | Telnet port |
 * | Timeout | 2000ms | Connection timeout |
 * | Max Retries | 3 | Authentication retry attempts |
 * | Retry Delay | 30s | Delay between retry attempts |
 * | WS Port | 8765 | WebSocket server port |
 * 
 * @return int Exit code (0 = success, 1 = database/server error)
 * 
 * @pre Network connectivity to the relay device
 * @pre Valid credentials configured
 * @post SER records stored in SQLite database
 * @post WebSocket server available for UI connections
 */
int main()
{
    using namespace sml;

    std::cout << "========================================\n";
    std::cout << "  Telnet-SML Multi-threaded Application\n";
    std::cout << "========================================\n\n";

    // Shared stop flag for graceful shutdown
    std::atomic<bool> app_running{true};
    std::mutex fsm_mutex;  // Protect FSM access from multiple threads

    TelnetClient client;

    ConnectionConfig conn{
        "192.168.0.2",
        23,
        std::chrono::milliseconds(2000)
    };

    LoginConfig creds{
        "acc",
        "OTTER"
    };

    // Retry configuration: 3 attempts, 30 second delay
    RetryState retry{3, 0, std::chrono::seconds(30)};

    // Initialize SQLite database for SER records
    SERDatabase serDb("ser_records.db");
    if (!serDb.open())
    {
        std::cerr << "Failed to open database: " << serDb.getLastError() << "\n";
        return 1;
    }
    std::cout << "[DB] Database opened. Existing records: " << serDb.getRecordCount() << "\n";

    // =====================================================
    // Thread 1: WebSocket Server (handles UI connections)
    // =====================================================
    SERWebSocketServer wsServer(serDb, 8765);
    if (!wsServer.start())
    {
        std::cerr << "Failed to start WebSocket server\n";
        return 1;
    }

    // =====================================================
    // Thread 2 & 3: Thread Manager (DB Writer + SER Poller)
    // =====================================================
    ThreadManager threadMgr(serDb, std::chrono::seconds(120));  // Poll every 2 minutes

    // Create FSM
    sml::sm<TelnetFSM> fsm{ client, conn, creds, retry, serDb };

    // Set up polling callback (Thread 2: Periodic SER polling)
    threadMgr.setPollingCallback([&]() {
        std::lock_guard<std::mutex> lock(fsm_mutex);
        
        if (!app_running.load()) return;
        
        std::cout << "[Poller] Re-running FSM for SER update...\n";
        
        // Reset retry state for new poll cycle
        retry.reset();
        
        // Reinitialize FSM for new cycle
        fsm.process_event(start_event{});
        
        for (int i = 0; i < 10 && app_running.load(); ++i)
        {
            bool handled = fsm.process_event(step_event{});
            if (!handled)
                fsm.process_event(unhandled_event{});

            if (fsm.is("Error"_s) || fsm.is("Done"_s))
                break;

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        
        std::cout << "[Poller] Poll cycle complete\n";
    });

    // Start background threads (DB Writer + Poller)
    threadMgr.startAll();

    // =====================================================
    // Main Thread: Initial FSM execution
    // =====================================================
    std::cout << "\n[Main] Starting initial SER retrieval...\n";
    
    {
        std::lock_guard<std::mutex> lock(fsm_mutex);
        fsm.process_event(start_event{});

        for (int i = 0; i < 10; ++i)
        {
            bool handled = fsm.process_event(step_event{});
            if (!handled)
                fsm.process_event(unhandled_event{});

            if (fsm.is("Error"_s))
            {
                std::cout << "[ERROR] FSM entered Error state!\n";
                break;
            }

            if (fsm.is("Done"_s))
            {
                std::cout << "[INFO] SER data retrieved successfully.\n";
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    // =====================================================
    // Main Thread: Wait for user input
    // =====================================================
    std::cout << "\n========================================\n";
    std::cout << "  Active Threads:\n";
    std::cout << "  - Main Thread: User input handler\n";
    std::cout << "  - Thread 1: WebSocket Server (port 8765)\n";
    std::cout << "  - Thread 2: SER Poller (2 min interval)\n";
    std::cout << "  - Thread 3: Database Writer (async)\n";
    std::cout << "========================================\n";
    std::cout << "\n[INFO] Press Enter to exit...\n";
    std::cin.get();

    // =====================================================
    // Graceful Shutdown
    // =====================================================
    std::cout << "\n[Main] Shutting down...\n";
    app_running = false;
    
    threadMgr.stopAll();
    wsServer.stop();
    
    std::cout << "[Main] Application terminated.\n";
    return 0;
}
