#include "client.hpp"

#include <iostream>
#include <chrono>
#include <array>

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
    if (!connected_)
    {
        last_io_ok_ = false;
        return false;
    }

    try
    {
        outBuffer.clear();
        last_response_.clear();

        std::string fullCmd = cmd + "\r\n";
        asio::write(socket_, asio::buffer(fullCmd));

        auto start = std::chrono::steady_clock::now();

        while (true)
        {
            std::array<char, 512> data{};
            boost::system::error_code ec;
            std::size_t bytes = socket_.read_some(asio::buffer(data), ec);

            if (ec)
            {
                last_io_ok_ = false;
                return false;
            }

            outBuffer.append(data.data(), bytes);
            last_response_ = outBuffer;

            if (isResponseComplete(outBuffer))
            {
                last_io_ok_ = true;
                return true;
            }

            if (std::chrono::steady_clock::now() - start > io_timeout_)
            {
                last_io_ok_ = false;
                return false;
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "SendCmdReceiveData error: "
                  << e.what() << std::endl;
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
    std::string buffer;
    if (!SendCmdReceiveData(username, buffer))
        return false;

    return SendCmdReceiveData(password, buffer);
}

// ================= COMPLETION HELPERS =================
bool TelnetClient::isResponseComplete(const std::string& buffer) const
{
    if (buffer.find("SER Response Complete") != std::string::npos)
        return true;

    return endsWithPrompt(buffer);
}

bool TelnetClient::endsWithPrompt(const std::string& buffer)
{
    auto pos = buffer.find_last_not_of(" \r\n\t");
    if (pos == std::string::npos)
        return false;

    const char last = buffer[pos];
    return last == '>' || last == '#' || last == '$' || last == ':';
}
