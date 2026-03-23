// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file client.hpp
 * @brief Telnet Client for Substation Communication
 * 
 * @details This header defines the TelnetClient class which provides TCP-based
 * Telnet communication with electrical substation relay devices. The client
 * handles connection management, command/response exchange, and prompt detection.
 * 
 * ## Class Architecture
 * 
 * @dot
 * digraph TelnetClientClass {
 *     rankdir=TB;
 *     node [shape=record, style=filled, fillcolor=lightyellow];
 *     
 *     TelnetClient [label="{TelnetClient|
 *         - io_context\l
 *         - socket_\l  
 *         - connected_\l
 *         - last_io_ok_\l
 *         - last_response_\l
 *         - io_timeout_\l
 *         |
 *         + connectCheck()\l
 *         + isConnected()\l
 *         + SendCmdReceiveData()\l
 *         + LoginLevel1Function()\l
 *         + getLastResponse()\l
 *         + getLastIoResult()\l
 *         + clearLastResponse()\l
 *         - isResponseComplete()\l
 *         - endsWithPrompt()\l
 *     }"];
 *     
 *     asio [label="boost::asio\nio_context", shape=ellipse, fillcolor=lightblue];
 *     tcp [label="tcp::socket", shape=ellipse, fillcolor=lightblue];
 *     
 *     TelnetClient -> asio [label="uses"];
 *     TelnetClient -> tcp [label="owns"];
 * }
 * @enddot
 * 
 * ## Communication Sequence
 * 
 * @msc
 * Client,Socket,Relay;
 * Client->Socket [label="connectCheck()"];
 * Socket->Relay [label="TCP SYN"];
 * Relay->Socket [label="TCP ACK"];
 * Socket->Client [label="connected"];
 * Client->Socket [label="SendCmdReceiveData(cmd)"];
 * Socket->Relay [label="cmd + CRLF"];
 * Relay->Socket [label="response data"];
 * Socket->Client [label="buffer"];
 * Client note Client [label="Check prompt"];
 * @endmsc
 * 
 * ## Usage Example
 * 
 * @code{.cpp}
 * TelnetClient client;
 * 
 * // Connect with timeout
 * if (client.connectCheck("192.168.0.2", 23, std::chrono::milliseconds(2000))) {
 *     // Login
 *     client.LoginLevel1Function("user", "password");
 *     
 *     // Send command and get response
 *     std::string buffer;
 *     client.SendCmdReceiveData("SER", buffer);
 *     
 *     // Process response
 *     std::cout << buffer << std::endl;
 * }
 * @endcode
 * 
 * @see TelnetFSM State machine that orchestrates client operations
 * @see SERRecord Structure for parsed SER data
 * 
 * @note Uses non-blocking I/O for response collection with idle timeout detection
 * @note Thread-safe for single-client usage; not designed for concurrent access
 * 
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

#include "dll_export.hpp"
#include <boost/asio.hpp>
#include <string>
#include <chrono>

namespace asio = boost::asio;
using boost::asio::ip::tcp;

/**
 * @class TelnetClient
 * @brief TCP/Telnet client for substation relay communication
 * 
 * @details Provides a complete Telnet client implementation using Boost.Asio
 * for asynchronous networking. Key features:
 * 
 * - **Connection Management**: Timeout-based connection with automatic cleanup
 * - **Command/Response**: Send commands and receive responses with prompt detection
 * - **State Tracking**: Maintains connection state and last I/O result for FSM guards
 * - **Non-blocking I/O**: Uses idle timeout detection for SER data collection
 * 
 * ## Internal State Diagram
 * 
 * @dot
 * digraph ClientState {
 *     rankdir=LR;
 *     node [shape=circle];
 *     
 *     Disconnected [style=filled, fillcolor=lightcoral];
 *     Connected [style=filled, fillcolor=lightgreen];
 *     
 *     Disconnected -> Connected [label="connectCheck()\nsuccess"];
 *     Connected -> Disconnected [label="error/timeout"];
 *     Connected -> Connected [label="SendCmd()/Login()"];
 * }
 * @enddot
 * 
 * @invariant socket_ is valid throughout object lifetime
 * @invariant connected_ reflects actual socket connection state
 */
class TELNET_SML_API TelnetClient
{
public:
    /**
     * @brief Construct a new TelnetClient object
     * 
     * @details Initializes the Boost.Asio I/O context and TCP socket.
     * Sets default I/O timeout to 5000ms. Socket starts in disconnected state.
     * 
     * @post connected_ == false
     * @post last_io_ok_ == false
     * @post io_timeout_ == 5000ms
     */
    TelnetClient();

    // ================= CORE OPERATIONS =================
    
    /**
     * @brief Connect to a Telnet server with timeout
     * 
     * @details Establishes TCP connection to the specified host and port.
     * Uses asynchronous connect with timer for timeout handling.
     * 
     * Connection Process:
     * 1. Resolve hostname to IP addresses
     * 2. Start async connect with timeout timer
     * 3. Run I/O context until connection or timeout
     * 4. Update connection state
     * 
     * @param host Target hostname or IP address
     * @param port Target port number (typically 23 for Telnet)
     * @param timeout Maximum time to wait for connection
     * 
     * @return true Connection established successfully
     * @return false Connection failed (timeout, refused, or error)
     * 
     * @pre Socket may be in any state (will be closed and reopened)
     * @post connected_ reflects connection result
     * @post last_io_ok_ reflects connection result
     * 
     * @throws None (catches all exceptions internally)
     * 
     * @see isConnected() Check connection state after connect
     */
    bool connectCheck(const std::string& host,
                      int port,
                      std::chrono::milliseconds timeout);
    
    /**
     * @brief Check if client is currently connected
     * 
     * @return true Socket is connected
     * @return false Socket is disconnected
     */
    bool isConnected() const;

    // ================= GENERIC FSM API =================
    
    /**
     * @brief Send command and receive response data
     * 
     * @details Generic command/response method used by Boost.SML actions.
     * Sends command with CRLF termination and collects response using
     * non-blocking I/O with idle timeout detection.
     * 
     * ## Response Collection Algorithm
     * 
     * ```
     * 1. Send command + "\r\n"
     * 2. Set socket to non-blocking mode
     * 3. Loop:
     *    a. Try read from socket
     *    b. If would_block: check idle timeout
     *    c. If data received: update idle timer, append to buffer
     *    d. Check for response completion (prompt or marker)
     *    e. Check overall timeout
     * 4. Return buffer contents
     * ```
     * 
     * @param[in] cmd Command string to send (without CRLF)
     * @param[out] outBuffer Buffer to receive response data
     * 
     * @return true Command sent and response received successfully
     * @return false Not connected, write error, or timeout with no data
     * 
     * @pre isConnected() == true
     * @post outBuffer contains response data
     * @post last_response_ updated with response
     * @post last_io_ok_ reflects operation result
     * 
     * @note SER command uses longer idle timeout (500ms vs 200ms)
     */
    bool SendCmdReceiveData(const std::string& cmd,
                            std::string& outBuffer);

    /**
     * @brief Send command and collect multi-page response
     *
     * @details Sends the initial command, then detects "Press RETURN to continue"
     * prompts in the relay output and automatically sends carriage returns to
     * advance through all pages. Accumulates the full response.
     *
     * @param[in] cmd   Command string to send (without CRLF)
     * @param[out] outBuffer Accumulated response across all pages
     * @param maxPages  Maximum number of continuation pages (safety limit)
     *
     * @return true  Full multi-page response collected
     * @return false Connection error or timeout
     */
    bool SendCmdMultiPage(const std::string& cmd,
                          std::string& outBuffer,
                          int maxPages = 20);

    // ================= TELNET COMMAND WRAPPERS =================
    
    /**
     * @brief Perform Level 1 login authentication
     * 
     * @details Sends username and password as sequential commands.
     * Used for basic authentication to substation relay devices.
     * 
     * Login Sequence:
     * ```
     * Client: username\r\n
     * Server: Password: (or prompt)
     * Client: password\r\n
     * Server: command prompt (e.g., ">")
     * ```
     * 
     * @param username Level 1 username
     * @param password Level 1 password
     * 
     * @return true Both username and password accepted
     * @return false Authentication failed at any step
     * 
     * @see Login1CompleteGuard FSM guard for checking login success
     */
    bool LoginLevel1Function(const std::string& username,
                             const std::string& password);

    // ================= RESPONSE ACCESS =================
    
    /**
     * @brief Get the last response received
     * 
     * @return const std::string& Reference to last response buffer
     */
    const std::string& getLastResponse() const;
    
    /**
     * @brief Get the result of the last I/O operation
     * 
     * @details Used by FSM guards to check operation success.
     * 
     * @return true Last operation succeeded
     * @return false Last operation failed
     */
    bool getLastIoResult() const;
    
    /**
     * @brief Clear the last response buffer
     * 
     * @details Called before each new command to reset state.
     * 
     * @post last_response_.empty() == true
     */
    void clearLastResponse();

private:
    // ================= INTERNAL HELPERS =================
    
    /**
     * @brief Check if response is complete
     * 
     * @details Detects response completion by:
     * - Finding "SER Response Complete" marker
     * - Detecting command prompt at buffer end
     * 
     * @param buffer Response buffer to check
     * @return true Response is complete
     * @return false Still waiting for more data
     */
    bool isResponseComplete(const std::string& buffer) const;
    
    /**
     * @brief Check if buffer ends with a command prompt
     * 
     * @details Searches last 50 characters for prompt patterns:
     * - "=>" (relay prompt)
     * - "> ", "# ", "$ " (Unix-style prompts)
     * - "? " (question prompts)
     * 
     * @param buffer Response buffer to check
     * @return true Prompt detected at end
     * @return false No prompt found
     */
    static bool endsWithPrompt(const std::string& buffer);

private:
    asio::io_context io_;                    ///< Boost.Asio I/O context for async operations
    tcp::socket socket_;                      ///< TCP socket for Telnet communication
    bool connected_;                          ///< Current connection state
    bool last_io_ok_;                          ///< Result of last I/O operation (for FSM guards)
    std::string last_response_;               ///< Buffer containing last received response
    std::chrono::milliseconds io_timeout_;    ///< Overall I/O timeout for commands
};
