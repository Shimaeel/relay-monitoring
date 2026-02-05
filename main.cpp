#include <iostream>
#include <thread>
#include <chrono>

#include "client.hpp"

int main()
{
    std::cout << "=== TelnetClient Standalone Test ===\n";

    TelnetClient client;

    // 1️⃣ Connect
    if (!client.connect("192.168.0.2", 23))
    {
        std::cerr << "Connection failed\n";
        return 1;
    }

    // 2️⃣ Start receiver thread
    client.startReceiver();

    // 3️⃣ Send SER command
    if (!client.sendCommand("SER GET_ALL"))
    {
        std::cerr << "Failed to send SER command\n";
        return 1;
    }

    // 4️⃣ Wait to receive full response
    std::this_thread::sleep_for(std::chrono::seconds(5));

    std::cout << "=== Test finished ===\n";
    return 0;
}
