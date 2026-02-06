#include <iostream>
#include <thread>
#include <chrono>

#include "client.hpp"
#include "telnet_fsm.hpp"

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

    sml::sm<TelnetFSM> fsm{ client, conn, creds };

    fsm.process_event(start_event{});

    std::string lastPrinted;
    for (int i = 0; i < 10; ++i)
    {
        // std::cout << "\n--- Step " << i << " ---\n";
        // std::cout << "Last IO Result: " << (client.getLastIoResult() ? "OK" : "FAIL") << "\n";
        // std::cout << "Last Response: [" << client.getLastResponse() << "]\n";
        const std::string& response = client.getLastResponse();
        if (response != lastPrinted) {
            std::cout << response;
            lastPrinted = response;
        }
        
        bool handled = fsm.process_event(step_event{});
        if (!handled)
            fsm.process_event(unhandled_event{});

        if (fsm.is("Error"_s))
        {
            // std::cout << "FSM entered Error state!\n";
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // std::cout << "=== FSM DONE ===\n";
    return 0;
}
