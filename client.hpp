#pragma once

#include <boost/asio.hpp>
#include <string>
#include <functional>

namespace asio = boost::asio;
using boost::asio::ip::tcp;

// ================= FSM EVENT TYPES =================
// Typed events instead of bool (STEP-10)
enum class FsmEvent
{
    Login1Ok,
    Login1Fail,
    Login2Ok,
    Login2Fail,
    SerOk,
    SerFail
};

class TelnetClient
{
public:
    TelnetClient();

    // ================= CORE OPERATIONS =================
    bool connect(const std::string& host, int port);
    void startReceiver();
    bool isConnected() const;

    // ================= GENERIC FSM API =================
    // Used by Boost.SML actions
    bool SendCmdReceiveData(const std::string& cmd,
                            std::string& outBuffer);

    // ================= RESPONSE ACCESS =================
    const std::string& getLastResponse() const;

    // ================= FSM CALLBACK (STEP-10) =================
    // TelnetClient -> FSM notification (typed)
    void setEventCallback(std::function<void(FsmEvent)> cb);

private:
    // ================= INTERNAL HELPERS =================
    void receiveMessages();
    void saveSERToFile(const std::string& ser_data);

private:
    asio::io_context io_;
    tcp::socket socket_;
    bool connected_;

    std::string ser_buffer_;
    bool capturing_ser_;

    // ================= FSM CALLBACK STORAGE =================
    std::function<void(FsmEvent)> eventCallback_;
};
