// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file server.cpp
 * @brief Basic Telnet Server for Substation Monitoring
 * 
 * @details This file implements a standalone telnet server for substation monitoring
 * and control. It provides:
 * - TCP socket server using Boost.Asio
 * - Command-line interface via telnet protocol
 * - Substation monitoring commands (status, voltage, current, temperature, etc.)
 * - System Event Records (SER) retrieval
 * - Multi-client support (sequential, one at a time)
 * 
 * Server Features:
 * - Port: 9999 (non-privileged port)
 * - Protocol: Telnet (plain text over TCP)
 * - Commands: status, power, voltage, current, temperature, alarm, SER, reset, help, exit
 * - Welcome banner and interactive prompt
 * - Error handling and graceful disconnection
 * 
 * Command Interface:
 * ```
 * > status
 * STATUS: All systems operational
 * Voltage: 380V - Normal
 * Current: 45A - Normal
 * Temperature: 35°C - Normal
 * 
 * > SER
 * Total Records: 5
 * Record 1: SER-001 | 2026-01-14 09:15:23 | ACTIVE | Voltage threshold exceeded
 * ...
 * ```
 * 
 * Architecture:
 * - Synchronous blocking I/O model
 * - One client at a time (sequential processing)
 * - Accept loop: Accept → Handle → Close → Accept
 * - No threading or async operations
 * 
 * Use Cases:
 * - Testing telnet client implementations
 * - Substation monitoring system prototyping
 * - Educational/demonstration purposes
 * - Standalone server without state machine integration
 * 
 * @example
 * // Compile:
 * // g++ -std=c++17 server.cpp -o server -lboost_system
 * 
 * // Run:
 * ./server
 * // === Telnet Substation Server ===
 * // Server started on port 9999
 * // Waiting for client connection...
 * 
 * // Connect:
 * // telnet localhost 9999
 * // > help
 * // AVAILABLE COMMANDS:
 * //   status      - Get system status
 * //   power       - Get power level
 * //   ...
 * 
 * @note Requires Boost.Asio library
 * @note No authentication or encryption
 * @note Handles one client at a time
 * @note For integrated version with state machine, see server_integrated.cpp
 * 
 * @author State Machine Team
 * @version 1.0.0
 * @date 2026-01-16
 */

#include <iostream>
#include <boost/asio.hpp>
#include <string>
#include <thread>
#include <vector>

using boost::asio::ip::tcp;

/**
 * @brief Basic Telnet Server for Substation Monitoring
 * 
 * @details Implements a simple telnet server that:
 * - Accepts TCP connections on a specified port
 * - Presents an interactive command prompt
 * - Processes substation monitoring commands
 * - Returns formatted responses
 * - Handles disconnection gracefully
 * 
 * The server operates in synchronous mode:
 * - Blocks on accept() waiting for connections
 * - Handles one client completely before accepting next
 * - Uses blocking I/O for command/response
 * - No concurrent client support
 * 
 * Server Lifecycle:
 * 1. Constructor: Initialize acceptor on port
 * 2. start(): Enter accept loop
 * 3. handleClient(): Process commands for connected client
 * 4. processCommand(): Generate responses
 * 5. Client disconnects, return to step 2
 */
class TelnetServer
{
private:
    boost::asio::io_context io_;   /**< Boost.Asio I/O context for socket operations */
    tcp::acceptor acceptor_;       /**< TCP acceptor for incoming connections */
    bool running_;                 /**< Server running flag (false to stop) */

public:
    /**
     * @brief Construct telnet server on specified port
     * 
     * @details Constructor:
     * - Initializes Boost.Asio acceptor on specified port
     * - Binds to all network interfaces (0.0.0.0)
     * - Uses IPv4 protocol
     * - Sets running flag to true
     * - Does not start accepting connections yet
     * 
     * Port Selection:
     * - Typically use 9999 (non-privileged port)
     * - Port 23 requires admin/root privileges
     * - Port must not be in use by another service
     * 
     * @param port TCP port number to listen on (1-65535)
     * @throws boost::system::system_error if port binding fails
     * 
     * @example
     * try {
     *     TelnetServer server(9999);
     *     // Server created, ready to start
     * } catch (const boost::system::system_error& e) {
     *     std::cerr << "Failed to bind port: " << e.what() << std::endl;
     * }
     */
    TelnetServer(int port)
        : acceptor_(io_, tcp::endpoint(tcp::v4(), port)), running_(true)
    {
    }

    /**
     * @brief Start accepting client connections
     * 
     * @details Main server loop:
     * 1. Print startup banner with port information
     * 2. Enter infinite accept loop
     * 3. For each connection:
     *    a. Block on accept() waiting for client
     *    b. Log client's IP address
     *    c. Send welcome banner
     *    d. Call handleClient() to process commands
     *    e. Close connection
     * 4. Repeat until running_ becomes false
     * 
     * Startup Banner Displays:
     * - Server name and version
     * - Port number
     * - Connection status message
     * 
     * Blocking Behavior:
     * - accept() blocks until client connects
     * - handleClient() blocks until client disconnects
     * - Only one client handled at a time
     * 
     * Exception Handling:
     * - Catches and logs all exceptions
     * - Continues accepting after client errors
     * - Server keeps running unless stop() called
     * 
     * @note Blocks indefinitely until stop() is called
     * @note Handles clients sequentially, not concurrently
     * @note Exceptions from handleClient() are logged but don't stop server
     * 
     * @example
     * TelnetServer server(9999);
     * server.start();
     * // === Telnet Substation Server ===
     * // Server started on port 9999
     * // Waiting for client connection...
     * // 
     * // Client connected: 127.0.0.1
     * // ... client interaction ...
     * // Client disconnected.
     * // 
     * // Waiting for client connection...
     */
    void start()
    {
        std::cout << "=== Telnet Substation Server ===" << std::endl;
        std::cout << "Server started on port " << acceptor_.local_endpoint().port() << std::endl;
        std::cout << "Waiting for client connection...\n" << std::endl;

        while (running_)
        {
            try
            {
                tcp::socket socket(io_);
                acceptor_.accept(socket);

                std::cout << "Client connected: " << socket.remote_endpoint().address() << std::endl;

                // Send welcome message
                std::string welcome = "\r\n========================================\r\n";
                welcome += "Welcome to Substation Telnet Server\r\n";
                welcome += "========================================\r\n";
                welcome += "Type 'help' for available commands\r\n";
                welcome += "Type 'exit' to disconnect\r\n";
                welcome += "========================================\r\n\r\n";
                boost::asio::write(socket, boost::asio::buffer(welcome));

                handleClient(socket);
            }
            catch (const std::exception& e)
            {
                std::cerr << "Error: " << e.what() << std::endl;
            }
        }
    }

    /**
     * @brief Stop accepting new connections
     * 
     * @details Sets the running_ flag to false, which causes the accept
     * loop in start() to exit. Current client connection (if any) will
     * continue until completion.
     * 
     * Shutdown Behavior:
     * - Accept loop exits after current accept() completes
     * - Active client connection not interrupted
     * - No forced disconnection
     * 
     * @note Does not forcibly close existing connections
     * @note Accept loop may not exit immediately if blocking on accept()
     * @note Typically called from signal handler or another thread
     * 
     * @example
     * // From signal handler:
     * TelnetServer* g_server = nullptr;
     * void signal_handler(int sig) {
     *     if (g_server) {
     *         g_server->stop();
     *     }
     * }
     * 
     * // Or from another thread:
     * TelnetServer server(9999);
     * std::thread server_thread([&]() { server.start(); });
     * // ... later ...
     * server.stop();
     * server_thread.join();
     */
    void stop()
    {
        running_ = false;
    }

private:
    /**
     * @brief Handle commands for a connected client
     * 
     * @details Client session loop:
     * 1. Send prompt ("> ") to client
     * 2. Read command until newline
     * 3. Strip whitespace and carriage returns
     * 4. Log command to console
     * 5. Check for exit commands ("exit" or "quit")
     * 6. Process command and get response
     * 7. Send response with \r\n terminator
     * 8. Repeat until disconnect or exit
     * 
     * Command Processing:
     * - Uses read_until() to read complete lines
     * - Strips \r characters from Windows telnet clients
     * - Empty commands are ignored (continue loop)
     * - Exit commands send goodbye and break loop
     * 
     * Disconnect Handling:
     * - "exit" or "quit" commands trigger clean disconnect
     * - Socket errors caught and logged
     * - Socket explicitly closed before return
     * - Disconnect message logged to console
     * 
     * Response Format:
     * - Each response terminated with \r\n
     * - Compatible with telnet protocol
     * - Windows-style line endings for compatibility
     * 
     * @param socket Reference to client's TCP socket
     * 
     * @note Blocks on read_until() waiting for commands
     * @note Exceptions caught and logged, connection closed on error
     * @note Socket automatically closed at end of function
     * 
     * @example
     * // Internal call flow:
     * tcp::socket socket(io_);
     * acceptor_.accept(socket);
     * std::cout << "Client connected: " << socket.remote_endpoint().address() << std::endl;
     * handleClient(socket);
     * 
     * // Client interaction:
     * // > status
     * // Client command: status
     * // STATUS: All systems operational...
     * // > voltage
     * // Client command: voltage
     * // VOLTAGE READING:...
     * // > exit
     * // Client command: exit
     * // Goodbye!
     * // Client disconnected.
     */
    void handleClient(tcp::socket& socket)
    {
        try
        {
            while (socket.is_open())
            {
                // Send prompt
                std::string prompt = "> ";
                boost::asio::write(socket, boost::asio::buffer(prompt));

                // Receive command
                boost::asio::streambuf buffer;
                boost::asio::read_until(socket, buffer, '\n');

                std::istream is(&buffer);
                std::string command;
                std::getline(is, command);

                if (command.empty())
                {
                    continue;
                }

                // Remove carriage return if present
                if (!command.empty() && command.back() == '\r')
                {
                    command.pop_back();
                }

                std::cout << "Client command: " << command << std::endl;

                // Check for exit command
                if (command == "exit" || command == "quit")
                {
                    std::string bye = "Goodbye!\r\n";
                    boost::asio::write(socket, boost::asio::buffer(bye));
                    break;
                }

                // Process command and send response
                std::string response = processCommand(command);
                response += "\r\n";
                boost::asio::write(socket, boost::asio::buffer(response));
            }

            socket.close();
            std::cout << "Client disconnected.\n" << std::endl;
        }
        catch (const std::exception& e)
        {
            std::cerr << "Client handler error: " << e.what() << std::endl;
        }
    }

    /**
     * @brief Process telnet command and generate response
     * 
     * @details Command dispatcher that maps commands to responses:
     * 
     * Command Catalog:
     * - **status**: Overall system status (voltage, current, temperature)
     * - **power**: Power level and supply status
     * - **voltage**: Three-phase voltage readings (A, B, C)
     * - **current**: Three-phase current readings (A, B, C)
     * - **temperature**: Temperature reading with cooling status
     * - **alarm**: Active alarm status
     * - **reset**: Trigger system reset operation
     * - **SER**: Get System Event Records (5 hardcoded records)
     * - **help**: Display command list with descriptions
     * - **Unknown**: Error message with help suggestion
     * 
     * SER Command:
     * - Case-insensitive ("SER", "ser", "SER GET_ALL", "ser get_all")
     * - Returns 5 mock event records
     * - Format: Record N: ID | timestamp | status | description
     * - Ends with "SER Response Complete" marker
     * 
     * Response Characteristics:
     * - All responses use \r\n line endings
     * - Structured format for easy parsing
     * - Consistent formatting across commands
     * - No trailing \r\n (added by caller)
     * 
     * Mock Data:
     * - All responses return hardcoded values
     * - No actual hardware monitoring
     * - Suitable for testing and development
     * 
     * @param command Command string from client (case-sensitive except SER)
     * @return Response string with \r\n line separators (no trailing \r\n)
     * 
     * @note Case-sensitive for most commands (lowercase required)
     * @note SER command is case-insensitive
     * @note Unknown commands return helpful error message
     * @note Caller must append final \r\n before sending
     * 
     * @example
     * // Example 1: Status command
     * std::string response = processCommand("status");
     * // Returns:
     * // "STATUS: All systems operational\r\n
     * //  Voltage: 380V - Normal\r\n
     * //  Current: 45A - Normal\r\n
     * //  Temperature: 35°C - Normal"
     * 
     * @example
     * // Example 2: Voltage command
     * std::string response = processCommand("voltage");
     * // Returns:
     * // "VOLTAGE READING:\r\n
     * //  Phase A: 380V\r\n
     * //  Phase B: 380V\r\n
     * //  Phase C: 380V"
     * 
     * @example
     * // Example 3: SER command
     * std::string response = processCommand("SER");
     * // Returns:
     * // "Total Records: 5\r\n
     * //  Record 1: SER-001 | 2026-01-14 09:15:23 | ACTIVE | Voltage threshold exceeded\r\n
     * //  Record 2: SER-002 | 2026-01-14 09:20:45 | ACTIVE | Temperature anomaly detected\r\n
     * //  Record 3: SER-003 | 2026-01-14 09:25:12 | INACTIVE | System reset completed\r\n
     * //  Record 4: SER-004 | 2026-01-14 09:30:08 | ACTIVE | Backup power activated\r\n
     * //  Record 5: SER-005 | 2026-01-14 09:35:55 | ACTIVE | Current spike on Phase A\r\n
     * //  SER Response Complete"
     * 
     * @example
     * // Example 4: Unknown command
     * std::string response = processCommand("invalid");
     * // Returns:
     * // "Unknown command: 'invalid'\r\n
     * //  Type 'help' for available commands"
     */
    std::string processCommand(const std::string& command)
    {
        if (command == "status")
        {
            return "STATUS: All systems operational\r\n"
                "Voltage: 380V - Normal\r\n"
                "Current: 45A - Normal\r\n"
                "Temperature: 35�C - Normal";
        }
        else if (command == "power")
        {
            return "POWER STATUS: Power Level - 98%\r\n"
                "Supply: Active\r\n"
                "Backup: Standby";
        }
        else if (command == "voltage")
        {
            return "VOLTAGE READING:\r\n"
                "Phase A: 380V\r\n"
                "Phase B: 380V\r\n"
                "Phase C: 380V";
        }
        else if (command == "current")
        {
            return "CURRENT READING:\r\n"
                "Phase A: 45A\r\n"
                "Phase B: 43A\r\n"
                "Phase C: 44A";
        }
        else if (command == "temperature")
        {
            return "TEMPERATURE: 35�C - Normal\r\nCooling system active";
        }
        else if (command == "alarm")
        {
            return "ALARM STATUS: No active alarms";
        }
        else if (command == "reset")
        {
            return "System reset command received\r\nReset in progress...";
        }
        else if (command == "SER" || command == "SER GET_ALL" || command == "ser" || command == "ser get_all")
        {
            return "Total Records: 5\r\n"
                "Record 1: SER-001 | 2026-01-14 09:15:23 | ACTIVE | Voltage threshold exceeded\r\n"
                "Record 2: SER-002 | 2026-01-14 09:20:45 | ACTIVE | Temperature anomaly detected\r\n"
                "Record 3: SER-003 | 2026-01-14 09:25:12 | INACTIVE | System reset completed\r\n"
                "Record 4: SER-004 | 2026-01-14 09:30:08 | ACTIVE | Backup power activated\r\n"
                "Record 5: SER-005 | 2026-01-14 09:35:55 | ACTIVE | Current spike on Phase A\r\n"
                "SER Response Complete";
        }
        else if (command == "help")
        {
            return "AVAILABLE COMMANDS:\r\n"
                "  status      - Get system status\r\n"
                "  power       - Get power level\r\n"
                "  voltage     - Get voltage readings\r\n"
                "  current     - Get current readings\r\n"
                "  temperature - Get temperature\r\n"
                "  alarm       - Get alarm status\r\n"
                "  SER         - Get System Event Records\r\n"
                "  reset       - Reset system\r\n"
                "  help        - Show this help\r\n"
                "  exit        - Disconnect";
        }
        else
        {
            return "Unknown command: '" + command + "'\r\nType 'help' for available commands";
        }
    }
};

/**
 * @brief Main entry point for telnet server application
 * 
 * @details Program execution flow:
 * 1. Create TelnetServer instance on port 9999
 * 2. Call start() to begin accepting connections
 * 3. Run indefinitely until process terminated
 * 4. Cleanup automatically via destructor
 * 
 * Port Selection:
 * - Uses port 9999 (non-privileged)
 * - Does not require administrator/root privileges
 * - Standard telnet port 23 would require elevated privileges
 * 
 * Error Handling:
 * - Catches all exceptions during server creation/operation
 * - Logs error message to stderr
 * - Returns non-zero exit code on failure
 * 
 * Normal Operation:
 * - Server runs until process terminated (Ctrl+C)
 * - Handles clients sequentially
 * - Returns 0 only if server exits cleanly (rare)
 * 
 * Termination:
 * - Ctrl+C (SIGINT) terminates process
 * - SIGTERM also terminates process
 * - No graceful shutdown handler (immediate exit)
 * - Active connections dropped on termination
 * 
 * @return 0 on successful exit (typically never reached)
 * @return Non-zero if server fails to start or encounters error
 * 
 * @example
 * // Compile:
 * // g++ -std=c++17 server.cpp -o server -lboost_system
 * 
 * // Run:
 * ./server
 * // === Telnet Substation Server ===
 * // Server started on port 9999
 * // Waiting for client connection...
 * 
 * // Connect from another terminal:
 * // telnet localhost 9999
 * // 
 * // ========================================
 * // Welcome to Substation Telnet Server
 * // ========================================
 * // Type 'help' for available commands
 * // Type 'exit' to disconnect
 * // ========================================
 * // 
 * // > help
 * // AVAILABLE COMMANDS:
 * //   status      - Get system status
 * //   power       - Get power level
 * //   voltage     - Get voltage readings
 * //   current     - Get current readings
 * //   temperature - Get temperature
 * //   alarm       - Get alarm status
 * //   SER         - Get System Event Records
 * //   reset       - Reset system
 * //   help        - Show this help
 * //   exit        - Disconnect
 * // > exit
 * // Goodbye!
 */
int main()
{
    try
    {
        TelnetServer server(9999); // Using port 9999 (no admin rights needed)
        server.start();
    }
    catch (const std::exception& e)
    {
        std::cerr << "Server error: " << e.what() << std::endl;
    }

    return 0;
}