#include <iostream>
#include <thread>
#include <chrono>

#include "client.hpp"
#include "telnet_fsm.hpp"

int main()
{
    using namespace sml;

    std::cout << "=== Telnet FSM Test ===\n";

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

    for (int i = 0; i < 10; ++i)
    {
        bool handled = fsm.process_event(step_event{});
        if (!handled)
            fsm.process_event(unhandled_event{});

        if (fsm.is("Error"_s))
            break;

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "=== FSM DONE ===\n";
    return 0;
}
