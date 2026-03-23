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

// ================= CONSTRUCTOR =================
TelnetClient::TelnetClient()
    : socket_(io_), connected_(false), last_io_ok_(false), io_timeout_(std::chrono::milliseconds(5000))
{
}

// ================= CONNECT =================
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

// ================= GENERIC SEND =================
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

// ================= MULTI-PAGE SEND =================
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

// ================= IS CONNECTED =================
bool TelnetClient::isConnected() const
{
    return connected_;
}

// ================= RESPONSE ACCESS =================
const std::string& TelnetClient::getLastResponse() const
{
    return last_response_;
}

bool TelnetClient::getLastIoResult() const
{
    return last_io_ok_;
}

void TelnetClient::clearLastResponse()
{
    last_response_.clear();
}

// ================= TELNET COMMAND WRAPPERS =================
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

// ================= COMPLETION HELPERS =================
bool TelnetClient::isResponseComplete(const std::string& buffer) const
{
    if (buffer.find("SER Response Complete") != std::string::npos)
        return true;

    // For SER data, look for the actual command prompt at end of response
    // The relay shows "=>" as the prompt after SER data
    return endsWithPrompt(buffer);
}

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
