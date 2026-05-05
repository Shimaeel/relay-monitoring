// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file client.cpp
 * @brief Implementation of TelnetClient class
 * 
 * @details Provides TCP/Telnet communication implementation using Boost.Asio.
 * Handles connection establishment, command transmission, and response collection
 * with non-blocking I/O and idle timeout detection.
 * 
 * ## Implementation Details
 * 
 * ### Connection Strategy
 * Uses async_connect with steady_timer for timeout handling. The I/O context
 * runs until either connection succeeds or timeout fires.
 * 
 * ### Response Collection
 * Employs non-blocking socket reads with adaptive timeout:
 * - Standard commands: 50ms idle timeout
 * - SER command: 500ms idle timeout (more data expected)
 * 
 * ### Prompt Detection
 * Supports multiple prompt styles:
 * - Relay prompt: `=>`
 * - Unix prompts: `>`, `#`, `$`
 * - Question prompts: `?`
 * 
 * @see client.hpp Header file with class declaration
 * @see TelnetFSM State machine using this client
 */

#include "client.hpp"

#include <iostream>
#include <chrono>
#include <array>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#endif

/**
 * @brief Construct an idle client not yet bound to any endpoint.
 *
 * @details Initialises the embedded Boost.Asio socket against the internal
 * io_context, clears connection flags, and sets a default 5 s per-operation
 * I/O timeout. The client performs no network activity until connectCheck()
 * is invoked.
 */
TelnetClient::TelnetClient()
    : socket_(io_), connected_(false), last_io_ok_(false), io_timeout_(std::chrono::milliseconds(5000))
{
}

/**
 * @brief Resolve the host, establish a TCP session, and enable TCP keep-alive.
 *
 * @details Runs an async resolve + async_connect against a steady_timer so the
 * whole attempt honours @p timeout. On success the socket is left open with
 * TCP keep-alive tuned for long-running 24/7 sessions — probes start after
 * 30 s of idleness, repeat every 5 s, and give up after 3 misses (~45 s to
 * detect a silently dropped link). On failure, any half-opened socket is
 * closed and both @c connected_ and @c last_io_ok_ are cleared.
 *
 * @param host     DNS name or IP literal of the relay.
 * @param port     TCP port to connect to (Telnet default is 23).
 * @param timeout  Upper bound on resolve + connect combined.
 * @return true on established TCP session, false on any resolve/connect error.
 *
 * @note Keep-alive tuning is applied via SIO_KEEPALIVE_VALS on Windows and
 *       TCP_KEEPIDLE/INTVL/CNT on POSIX; failures are logged but non-fatal.
 */
bool TelnetClient::connectCheck(const std::string& host,
                                int port,
                                std::chrono::milliseconds timeout)
{
    try
    {
        io_.restart();
        tcp::resolver resolver(io_);
        boost::system::error_code ec;
        auto endpoints = resolver.resolve(host, std::to_string(port), ec);
        if (ec)
        {
            if (socket_.is_open())
                socket_.close();
            connected_ = false;
            last_io_ok_ = false;
            return false;
        }

        bool connected = false;
        asio::steady_timer timer(io_);
        timer.expires_after(timeout);

        if (socket_.is_open())
            socket_.close();

        timer.async_wait([this](const boost::system::error_code& t_ec) {
            if (!t_ec)
                socket_.close();
        });

        asio::async_connect(
            socket_,
            endpoints,
            [&](const boost::system::error_code& c_ec, const tcp::endpoint&) {
                if (!c_ec)
                    connected = true;
                ec = c_ec;
                timer.cancel();
            });

        io_.run();

        connected_ = connected;
        last_io_ok_ = connected;

        // Enable TCP keep-alive for long-running 24/7 connections.
        // Detects dead relay connections when network drops silently.
        // Default Windows keep-alive is ~2 hours — far too slow for 24/7.
        // Tuned: probe after 30s idle, retry every 5s, give up after 3 probes (~45s detection).
        if (connected)
        {
            boost::asio::socket_base::keep_alive keepAlive(true);
            boost::system::error_code ka_ec;
            socket_.set_option(keepAlive, ka_ec);
            if (ka_ec)
                std::cerr << "[TCP] Failed to set keep-alive: " << ka_ec.message() << "\n";

#ifdef _WIN32
            // Windows: use SIO_KEEPALIVE_VALS to set idle + interval
            struct tcp_keepalive ka_vals{};
            ka_vals.onoff = 1;
            ka_vals.keepalivetime = 30000;     // 30s before first probe
            ka_vals.keepaliveinterval = 5000;  // 5s between probes
            DWORD bytesReturned = 0;
            int result = WSAIoctl(
                socket_.native_handle(),
                SIO_KEEPALIVE_VALS,
                &ka_vals, sizeof(ka_vals),
                nullptr, 0,
                &bytesReturned, nullptr, nullptr);
            if (result == SOCKET_ERROR)
                std::cerr << "[TCP] Failed to set keep-alive timers: WSA error "
                          << WSAGetLastError() << "\n";
#else
            // Linux/macOS: set individual socket options
            int idle_sec = 30;   // seconds before first probe
            int intvl_sec = 5;   // seconds between probes
            int cnt = 3;         // number of probes before giving up
            auto fd = socket_.native_handle();
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle_sec,  sizeof(idle_sec));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl_sec, sizeof(intvl_sec));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,       sizeof(cnt));
#endif
        }

        return connected;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Connection failed: " << e.what() << std::endl;
        if (socket_.is_open())
            socket_.close();
        connected_ = false;
        last_io_ok_ = false;
        return false;
    }
}

/**
 * @brief Send a command and collect the relay's response until idle or prompt.
 *
 * @details Appends CR/LF to @p cmd, writes it to the socket, then loops in
 * non-blocking mode accumulating bytes into @p outBuffer. Completion is
 * detected either by an explicit marker (isResponseComplete()) or by the
 * stream going idle for longer than the adaptive idle timeout:
 * - 500 ms for @c SER (large multi-page dumps)
 * - 80 ms for every other command
 * The overall wall-clock is bounded by @c io_timeout_ (5 s default) so a
 * runaway relay cannot hang the caller indefinitely.
 *
 * @param[in]  cmd        Command text to send (no CR/LF; added internally).
 *                        Pass an empty string to send a bare RETURN.
 * @param[out] outBuffer  Receives all bytes observed until completion.
 * @return true if data was collected without a socket error, false otherwise.
 *         On timeout the call returns true iff any data was buffered.
 *
 * @note The socket is flipped back to blocking mode on every exit path so
 *       subsequent writes behave normally.
 */
bool TelnetClient::SendCmdReceiveData(const std::string& cmd,
                                      std::string& outBuffer)
{
    if (!connected_ || !socket_.is_open())
    {
        // std::cout << "[DEBUG] SendCmdReceiveData: Not connected\n";
        last_io_ok_ = false;
        return false;
    }

    try
    {
        // std::cout << "[DEBUG] SendCmdReceiveData: Sending '" << cmd << "'\n";
        outBuffer.clear();
        last_response_.clear();

        std::string fullCmd = cmd + "\r\n";
        asio::write(socket_, asio::buffer(fullCmd));

        auto start = std::chrono::steady_clock::now();
        auto lastDataTime = start;
        int readCount = 0;
        
        // For SER command, use longer idle timeout to collect all data
        bool isSERCmd = (cmd == "SER" || cmd == "ser");
        auto idleTimeout = isSERCmd ? std::chrono::milliseconds(500) : std::chrono::milliseconds(80);

        // Set socket to non-blocking for idle detection
        socket_.non_blocking(true);

        while (true)
        {
            std::array<char, 4096> data{};  // Larger buffer
            boost::system::error_code ec;
            std::size_t bytes = socket_.read_some(asio::buffer(data), ec);

            if (ec == boost::asio::error::would_block)
            {
                // No data available, check if we should stop
                auto now = std::chrono::steady_clock::now();
                
                // If we have data and haven't received anything for a while, we're done
                if (!outBuffer.empty() && (now - lastDataTime) > idleTimeout)
                {
                    socket_.non_blocking(false);
                    last_io_ok_ = true;
                    return true;
                }
                
                // Check overall timeout
                if (now - start > io_timeout_)
                {
                    socket_.non_blocking(false);
                    last_io_ok_ = !outBuffer.empty();  // Success if we got some data
                    return last_io_ok_;
                }
                
                // Small sleep to avoid busy loop
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            
            if (ec)
            {
                socket_.non_blocking(false);
                // std::cout << "[DEBUG] Read error: " << ec.message() << "\n";
                last_io_ok_ = false;
                return false;
            }

            if (bytes > 0)
            {
                readCount++;
                lastDataTime = std::chrono::steady_clock::now();
                outBuffer.append(data.data(), bytes);
                last_response_ = outBuffer;
            }

            // Check for explicit completion markers
            if (isResponseComplete(outBuffer))
            {
                socket_.non_blocking(false);
                last_io_ok_ = true;
                return true;
            }

            if (std::chrono::steady_clock::now() - start > io_timeout_)
            {
                socket_.non_blocking(false);
                last_io_ok_ = !outBuffer.empty();
                return last_io_ok_;
            }
        }
    }
    catch (const std::exception& e)
    {
        socket_.non_blocking(false);
        std::cerr << "SendCmdReceiveData error: "
                  << e.what() << std::endl;
        last_io_ok_ = false;
        return false;
    }
}

/**
 * @brief Issue a command and drive it through any paged "Press RETURN" prompts.
 *
 * @details Runs SendCmdReceiveData() once for the initial command, then keeps
 * sending bare RETURN while the tail of the last page contains
 * "Press RETURN to continue" and the final relay prompt (`=>`) has not yet
 * appeared. Stops early if either condition flips or @p maxPages is reached,
 * guarding against infinite paging on misbehaving firmware.
 *
 * @param[in]  cmd        Command text to send.
 * @param[out] outBuffer  Concatenated output of every collected page.
 * @param[in]  maxPages   Hard ceiling on how many RETURNs to send.
 * @return true if any output was collected, false if the first page failed.
 */
bool TelnetClient::SendCmdMultiPage(const std::string& cmd,
                                    std::string& outBuffer,
                                    int maxPages)
{
    outBuffer.clear();

    // Send initial command and collect first page
    std::string page;
    if (!SendCmdReceiveData(cmd, page))
        return false;
    outBuffer += page;

    // Keep sending RETURN while the response pauses at "Press RETURN to continue"
    for (int i = 0; i < maxPages; ++i)
    {
        // Check if the accumulated buffer ends with the relay prompt =>
        if (endsWithPrompt(outBuffer))
            break;

        // Check if the last page contained the continuation prompt
        if (page.find("Press RETURN to continue") == std::string::npos)
            break;

        // Send bare RETURN to advance to the next page
        page.clear();
        if (!SendCmdReceiveData(std::string(""), page))
            break;
        outBuffer += page;
    }

    last_response_ = outBuffer;
    last_io_ok_ = !outBuffer.empty();
    return last_io_ok_;
}

/**
 * @brief Report whether the last connectCheck() succeeded and was not closed.
 * @return true while the TCP session is believed to be open.
 * @note The flag reflects application-level state; a silently dropped link
 *       only clears it after the next failed I/O or keep-alive miss.
 */
bool TelnetClient::isConnected() const
{
    return connected_;
}

/**
 * @brief Return the buffer captured by the most recent Send* call.
 *
 * @details Useful for callers that issued a command without supplying their
 * own output buffer, or that need to re-parse a previously-collected page.
 *
 * @return Reference to the cached response (empty if none yet).
 */
const std::string& TelnetClient::getLastResponse() const
{
    return last_response_;
}

/**
 * @brief Indicate whether the most recent I/O attempt completed successfully.
 * @return true if the last Send... or connectCheck call ended without a socket error.
 */
bool TelnetClient::getLastIoResult() const
{
    return last_io_ok_;
}

/**
 * @brief Discard the cached response buffer.
 *
 * @details Call before issuing a fresh command when you intend to rely on
 * getLastResponse() afterwards — ensures stale bytes from a prior command
 * do not bleed into the next inspection.
 */
void TelnetClient::clearLastResponse()
{
    last_response_.clear();
}

// ================= TELNET COMMAND WRAPPERS =================

/**
 * @brief Perform a Level-1 (ACC) login against the SEL relay.
 *
 * @details Sends the username line, waits for the prompt, then sends the
 * password. Does not parse the banner — success/failure is inferred by the
 * caller from a follow-up command or by inspecting the response through
 * getLastResponse().
 *
 * @param username Level-1 account name (case-sensitive on most firmware).
 * @param password Level-1 account password.
 * @return true if both lines were transmitted and a response was read;
 *         false if the transport failed at any step.
 */
bool TelnetClient::LoginLevel1Function(const std::string& username,
                                       const std::string& password)
{
    // std::cout << "[DEBUG] Sending username: " << username << "\n";
    std::string buffer;
    if (!SendCmdReceiveData(username, buffer))
    {
        // std::cout << "[DEBUG] Username send failed\n";
        return false;
    }
    // std::cout << "[DEBUG] Username response: [" << buffer << "]\n";

    // std::cout << "[DEBUG] Sending password\n";
    bool result = SendCmdReceiveData(password, buffer);
    // std::cout << "[DEBUG] Password response: [" << buffer << "], Result: " << result << "\n";
    return result;
}

/**
 * @brief Elevate an existing Level-1 session to Level-2 (2AC) access.
 *
 * @details Issues the @c 2ac command, supplies @p l2_password at the ensuing
 * "Password:" prompt, then inspects the reply. Explicit failure strings
 * ("Invalid", "invalid", "Denied") short-circuit to false; otherwise success
 * is confirmed by the presence of the elevated prompt `=>>` (some firmware
 * downgrades to `=>` after elevation, which is also accepted).
 *
 * @param l2_password Level-2 password.
 * @return true on successful elevation, false on bad credentials or transport
 *         error. Requires the client to already be at Level 1.
 */
bool TelnetClient::LoginLevel2Function(const std::string& l2_password)
{
    std::string buffer;
    // SEL elevate command. Case-insensitive on the relay; lowercase
    // matches the documented default ("2ac" + "TAIL").
    if (!SendCmdReceiveData("2ac", buffer))
        return false;

    // Relay replies with "Password:" — send the L2 password next.
    if (!SendCmdReceiveData(l2_password, buffer))
        return false;

    // Successful L2 login ends at "=>>" prompt.  SEL reports common
    // failures inline (e.g. "Invalid Password").  Detect those.
    if (buffer.find("Invalid") != std::string::npos
        || buffer.find("invalid") != std::string::npos
        || buffer.find("Denied")  != std::string::npos)
        return false;

    return buffer.find("=>>") != std::string::npos
        || buffer.find("=>")  != std::string::npos;  // some firmware shows single prompt
}

/**
 * @brief Demote the session from Level-2 back to Level-1.
 *
 * @details Sends the @c acc command, which unconditionally drops privileges
 * on SEL firmware. The method does not validate the reply — it is the
 * caller's responsibility to issue a follow-up command if confirmation of
 * the new access level is required.
 *
 * @return true if the command was sent and a response was read.
 */
bool TelnetClient::LogoutLevel2Function()
{
    std::string buffer;
    return SendCmdReceiveData("acc", buffer);
}

// ================= COMPLETION HELPERS =================

/**
 * @brief Decide whether the accumulated response can be considered complete.
 *
 * @details Recognises either the explicit "SER Response Complete" sentinel
 * or a trailing relay prompt (delegated to endsWithPrompt()). The former
 * catches SER dumps that may contain a trailing `=>` mid-stream; the latter
 * handles ordinary commands that terminate at the session prompt.
 *
 * @param buffer Bytes received so far for the current command.
 * @return true if the response appears terminated and can be returned to
 *         the caller.
 */
bool TelnetClient::isResponseComplete(const std::string& buffer) const
{
    if (buffer.find("SER Response Complete") != std::string::npos)
        return true;

    // For SER data, look for the actual command prompt at end of response
    // The relay shows "=>" as the prompt after SER data
    return endsWithPrompt(buffer);
}

/**
 * @brief Heuristically detect a shell/relay prompt at the tail of @p buffer.
 *
 * @details Scans the last 50 bytes for the SEL relay prompt `=>` followed
 * only by whitespace, and for Unix-style prompts (`>`, `#`, `$`) followed by
 * a space or newline. A `?` prompt is also recognised for interactive
 * questions. Bare `:` is deliberately rejected because it commonly appears
 * inside timestamps and would otherwise trigger false completions.
 *
 * @param buffer Accumulated response bytes.
 * @return true if the buffer appears to end at an interactive prompt.
 *
 * @note static because it performs no member access; kept on the class for
 *       grouping purposes.
 */
bool TelnetClient::endsWithPrompt(const std::string& buffer)
{
    // Check last 50 characters for actual command prompt
    // Looking for patterns like "\n=>" or "Level 1\n=>"
    size_t len = buffer.length();
    if (len < 3) return false;
    
    // Look at last 50 characters
    size_t start = (len > 50) ? len - 50 : 0;
    std::string tail = buffer.substr(start);
    
    // Look for "=>" prompt which indicates end of relay response
    // Must be at end of a line or end of buffer
    size_t promptPos = tail.rfind("=>");
    if (promptPos != std::string::npos)
    {
        // Check if it's at the end (possibly followed by whitespace)
        size_t afterPrompt = promptPos + 2;
        while (afterPrompt < tail.length())
        {
            char c = tail[afterPrompt];
            if (c != ' ' && c != '\r' && c != '\n' && c != '\t')
                break;
            afterPrompt++;
        }
        if (afterPrompt >= tail.length())
            return true;
    }
    
    // Also check for other common prompts at end
    // But NOT just ":" which appears in timestamps
    if (tail.length() >= 2)
    {
        std::string lastTwo = tail.substr(tail.length() - 2);
        if (lastTwo == "> " || lastTwo == ">\r" || lastTwo == ">\n")
            return true;
        if (lastTwo == "# " || lastTwo == "#\r" || lastTwo == "#\n")
            return true;
        if (lastTwo == "$ " || lastTwo == "$\r" || lastTwo == "$\n")
            return true;
    }
    
    // Check for "? " prompt (for questions)
    if (tail.length() >= 2)
    {
        std::string lastTwo = tail.substr(tail.length() - 2);
        if (lastTwo == "? ")
            return true;
    }
    
    return false;
}
