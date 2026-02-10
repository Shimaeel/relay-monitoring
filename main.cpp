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
#include "telnet_sml_app.hpp"

/**
 * @brief Main entry point for the Telnet-SML application
 * 
 * @details Delegates all application logic to the Telnet-SML DLL.
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
    TelnetSmlApp app;
    if (!app.start())
        return 1;

    app.waitForExit();
    app.stop();
    return 0;
}
