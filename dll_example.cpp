// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

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
#include "telnet_sml_app.hpp"

/**
 * @brief Example application entry point
 * 
 * @details Demonstrates the DLL facade workflow:
 * 1. Create TelnetSmlApp
 * 2. Start the application
 * 3. Wait for exit signal
 */
int main()
{
    TelnetSmlApp app;
    if (!app.start())
        return 1;

    app.waitForExit();
    app.stop();
    return 0;
}
