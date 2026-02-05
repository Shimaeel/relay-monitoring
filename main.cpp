#include <iostream>
#include <thread>
#include <chrono>

#include "client.hpp"
#include "telnet_fsm.hpp"

int main()
{
    std::cout << "=== Telnet FSM STEP-10 Test ===\n";

    TelnetClient client;

    if (!client.connect("192.168.0.2", 23))
        return 1;

    client.startReceiver();

    sml::sm<TelnetFSM> fsm{ client };

    // ================= TYPED FSM EVENT BINDING =================
    client.setEventCallback([&fsm](FsmEvent ev) {
        switch (ev)
        {
        case FsmEvent::Login1Ok:
            std::cout << "[MAIN] Login1 OK\n";
            fsm.process_event(login1_ok{});
            break;

        case FsmEvent::Login1Fail:
            std::cout << "[MAIN] Login1 FAIL\n";
            fsm.process_event(login1_fail{});
            break;

        case FsmEvent::Login2Ok:
            std::cout << "[MAIN] Login2 OK\n";
            fsm.process_event(login2_ok{});
            break;

        case FsmEvent::Login2Fail:
            std::cout << "[MAIN] Login2 FAIL\n";
            fsm.process_event(login2_fail{});
            break;

        case FsmEvent::SerOk:
            fsm.process_event(ser_ok{});
            break;
        }
    });

    fsm.process_event(start_event{});

    std::this_thread::sleep_for(std::chrono::seconds(10));

    std::cout << "=== STEP-10 DONE ===\n";
    return 0;
}
