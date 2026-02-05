#pragma once

#include <boost/asio.hpp>
#include <string>

namespace asio = boost::asio;
using boost::asio::ip::tcp;

class TelnetClient
{
public:
    TelnetClient();

    // Core operations
    bool connect(const std::string& host, int port);
    void startReceiver();
    bool sendCommand(const std::string& command);
    bool isConnected() const;

private:
    // Internal helpers
    void receiveMessages();
    void saveSERToFile(const std::string& ser_data);

private:
    asio::io_context io_;
    tcp::socket socket_;
    bool connected_;

    std::string ser_buffer_;
    bool capturing_ser_;
};
