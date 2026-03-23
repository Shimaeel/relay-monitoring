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

// ================= BATCH SEND =================
bool TelnetClient::SendBatchCmdsReceiveAll(
    const std::vector<std::string>& cmds,
    std::vector<std::string>& responses,
    std::chrono::milliseconds timeout)
{
    responses.clear();
    if (cmds.empty()) return true;

    if (!connected_ || !socket_.is_open())
    {
        last_io_ok_ = false;
        return false;
    }

    try
    {
        // 1. Build a single mega-command: "TAR 0\r\nTAR 1\r\n...TAR N\r\n"
        std::string mega;
        mega.reserve(cmds.size() * 12);
        for (const auto& c : cmds)
        {
            mega += c;
            mega += "\r\n";
        }

        // 2. Single TCP write — relay queues all commands in its input buffer
        asio::write(socket_, asio::buffer(mega));

        // 3. Read the entire response stream, splitting on "=>" prompt
        auto start = std::chrono::steady_clock::now();
        auto lastDataTime = start;

        std::string allData;
        allData.reserve(256 * 1024);  // pre-allocate ~256KB

        const int expected = static_cast<int>(cmds.size());
        int promptsSeen = 0;

        // Idle timeout: 500ms — longer than single-command because relay is
        // processing many commands back-to-back, gaps between responses vary.
        const auto idleTimeout = std::chrono::milliseconds(500);

        socket_.non_blocking(true);

        while (promptsSeen < expected)
        {
            std::array<char, 8192> data{};
            boost::system::error_code ec;
            std::size_t bytes = socket_.read_some(asio::buffer(data), ec);

            if (ec == boost::asio::error::would_block)
            {
                auto now = std::chrono::steady_clock::now();

                // Idle timeout — if we have data and nothing new for a while, done
                if (!allData.empty() && (now - lastDataTime) > idleTimeout)
                    break;

                // Overall timeout
                if ((now - start) > timeout)
                    break;

                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            if (ec)
            {
                socket_.non_blocking(false);
                last_io_ok_ = false;
                return false;
            }

            if (bytes > 0)
            {
                lastDataTime = std::chrono::steady_clock::now();
                allData.append(data.data(), bytes);

                // Count prompt markers in the total buffer so far
                promptsSeen = 0;
                size_t pos = 0;
                while ((pos = allData.find("=>", pos)) != std::string::npos)
                {
                    ++promptsSeen;
                    pos += 2;
                }
            }

            // Overall timeout check
            if ((std::chrono::steady_clock::now() - start) > timeout)
                break;
        }

        socket_.non_blocking(false);

        // 4. Split allData by "=>" prompt markers into individual responses
        //    Each response ends with "=>" (possibly followed by whitespace).
        //    Format: "<response text>\r\n=>\r\n<next response>..."
        size_t searchFrom = 0;
        for (int i = 0; i < expected; ++i)
        {
            size_t promptPos = allData.find("=>", searchFrom);
            if (promptPos == std::string::npos)
            {
                // Fewer responses than expected — push remaining as last chunk
                if (searchFrom < allData.size())
                    responses.push_back(allData.substr(searchFrom));
                break;
            }

            // Extract text from searchFrom to promptPos (excluding the "=>" itself)
            responses.push_back(allData.substr(searchFrom, promptPos - searchFrom));

            // Skip past "=>" and any trailing whitespace
            searchFrom = promptPos + 2;
            while (searchFrom < allData.size() &&
                   (allData[searchFrom] == ' ' || allData[searchFrom] == '\r' ||
                    allData[searchFrom] == '\n' || allData[searchFrom] == '\t'))
            {
                ++searchFrom;
            }
        }

        last_io_ok_ = !responses.empty();
        last_response_ = allData;
        return last_io_ok_;
    }
    catch (const std::exception& e)
    {
        socket_.non_blocking(false);
        std::cerr << "SendBatchCmdsReceiveAll error: " << e.what() << std::endl;
        last_io_ok_ = false;
        return false;
    }
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
