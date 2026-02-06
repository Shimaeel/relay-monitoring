#pragma once

#include <boost/asio.hpp>
#include <string>
#include <chrono>

namespace asio = boost::asio;
using boost::asio::ip::tcp;

class TelnetClient
{
public:
    TelnetClient();

    // ================= CORE OPERATIONS =================
    bool connectCheck(const std::string& host,
                      int port,
                      std::chrono::milliseconds timeout);
    bool isConnected() const;

    // ================= GENERIC FSM API =================
    // Used by Boost.SML actions
    bool SendCmdReceiveData(const std::string& cmd,
                            std::string& outBuffer);

    // ================= TELNET COMMAND WRAPPERS =================
    bool LoginLevel1Function(const std::string& username,
                             const std::string& password);

    // ================= RESPONSE ACCESS =================
    const std::string& getLastResponse() const;
    bool getLastIoResult() const;
    void clearLastResponse();

private:
    // ================= INTERNAL HELPERS =================
    bool isResponseComplete(const std::string& buffer) const;
    static bool endsWithPrompt(const std::string& buffer);

private:
    asio::io_context io_;
    tcp::socket socket_;
    bool connected_;
    bool last_io_ok_;
    std::string last_response_;
    std::chrono::milliseconds io_timeout_;
};
