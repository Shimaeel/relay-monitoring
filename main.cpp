#include <iostream>
#include <thread>
#include <chrono>

#include "client.hpp"
#include "telnet_fsm.hpp"
#include "ser_database.hpp"
#include "ws_server.hpp"

int main()
{
    using namespace sml;



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

    // Start WebSocket server for UI
    SERWebSocketServer wsServer(serDb, 8765);
    if (!wsServer.start())
    {
        std::cerr << "Failed to start WebSocket server\n";
        return 1;
    }

    sml::sm<TelnetFSM> fsm{ client, conn, creds, retry, serDb };

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

        // Stop loop after SER polling is complete
        if (fsm.is("Done"_s))
        {
            std::cout << "[INFO] SER data retrieved successfully.\n";
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Keep server running for UI access
    std::cout << "[INFO] Press Enter to exit...\n";
    std::cin.get();

    wsServer.stop();
    return 0;
}
