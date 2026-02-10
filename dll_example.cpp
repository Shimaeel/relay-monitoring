/**
 * @file dll_example.cpp
 * @brief Example Application Using Telnet-SML DLL
 * 
 * @details Demonstrates how to use the telnet_sml.dll library in an external
 * application. This example connects to a relay, retrieves SER data, and
 * displays it through the WebSocket server.
 * 
 * ## Building This Example
 * 
 * @code{.bash}
 * # First build the DLL
 * ./build_dll.ps1
 * 
 * # Then compile this example
 * g++ -std=c++17 ^
 *     -I third_party/sml/include ^
 *     -I C:\Libraries\boost_1_90_0 ^
 *     -I C:\sqlite ^
 *     dll_example.cpp ^
 *     -Llib -ltelnet_sml ^
 *     -lws2_32 -lmswsock ^
 *     -o dll_example.exe
 * 
 * # Copy DLL to executable location
 * copy lib\telnet_sml.dll .
 * 
 * # Run
 * ./dll_example.exe
 * @endcode
 * 
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#include <iostream>
#include <thread>
#include <chrono>

// Include headers from the DLL
#include "client.hpp"
#include "telnet_fsm.hpp"
#include "ser_database.hpp"
#include "ws_server.hpp"

// Import SML literals
using namespace sml;

/**
 * @brief Example application entry point
 * 
 * @details Demonstrates the complete workflow:
 * 1. Create TelnetClient for communication
 * 2. Configure connection parameters
 * 3. Initialize database and WebSocket server
 * 4. Run state machine to completion
 */
int main()
{
    std::cout << "========================================\n";
    std::cout << "  Telnet-SML DLL Example Application\n";
    std::cout << "========================================\n\n";

    // Create the client (from DLL)
    TelnetClient client;

    // Connection configuration
    ConnectionConfig conn{
        "192.168.0.2",                          // Relay IP address
        23,                                      // Telnet port
        std::chrono::milliseconds(2000)         // Connection timeout
    };

    // Authentication credentials
    LoginConfig creds{
        "acc",                                   // Username
        "OTTER"                                  // Password
    };

    // Retry configuration
    RetryState retry{
        3,                                       // Max retries
        0,                                       // Current attempt
        std::chrono::seconds(30)                // Delay between retries
    };

    // Initialize database (from DLL)
    std::cout << "[INFO] Opening database...\n";
    SERDatabase serDb("ser_records.db");
    if (!serDb.open())
    {
        std::cerr << "[ERROR] Failed to open database: " << serDb.getLastError() << "\n";
        return 1;
    }
    std::cout << "[INFO] Database opened. Existing records: " << serDb.getRecordCount() << "\n";

    // Start WebSocket server (from DLL)
    std::cout << "[INFO] Starting WebSocket server...\n";
    SERWebSocketServer wsServer(serDb, 8765);
    if (!wsServer.start())
    {
        std::cerr << "[ERROR] Failed to start WebSocket server\n";
        return 1;
    }

    // Create and run the state machine
    std::cout << "[INFO] Creating state machine...\n";
    sml::sm<TelnetFSM> fsm{client, conn, creds, retry, serDb};

    std::cout << "[INFO] Starting FSM...\n";
    fsm.process_event(start_event{});

    // Main loop
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

    // Display results
    std::cout << "\n[INFO] Database now contains " << serDb.getRecordCount() << " records\n";

    // Keep server running
    std::cout << "\n[INFO] WebSocket server running on ws://localhost:8765\n";
    std::cout << "[INFO] Press Enter to exit...\n";
    std::cin.get();

    // Cleanup
    wsServer.stop();
    serDb.close();

    std::cout << "[INFO] Application terminated.\n";
    return 0;
}
