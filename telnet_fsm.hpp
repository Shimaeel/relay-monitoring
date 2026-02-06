#pragma once

#include <boost/sml.hpp>
#include <iostream>
#include <string>
#include <chrono>

#include "client.hpp"

namespace sml = boost::sml;

// ================= EVENTS =================
struct start_event {};
struct step_event {};
struct unhandled_event {};

// ================= CONFIG =================
struct ConnectionConfig
{
    std::string host;
    int port;
    std::chrono::milliseconds timeout;
};

struct LoginConfig
{
    std::string l1_user;
    std::string l1_pass;
};

// ================= ACTION FUNCTIONS =================
struct ConnectAction
{
    template <class Event>
    void operator()(const Event&, TelnetClient& client, const ConnectionConfig& config) const
    {
        std::cout << "[ACTION] Connect\n";
        client.clearLastResponse();
        client.connectCheck(config.host, config.port, config.timeout);
    }
};

struct Login1Action
{
    template <class Event>
    void operator()(const Event&, TelnetClient& client, const LoginConfig& config) const
    {
        std::cout << "[ACTION] Login Level 1\n";
        client.clearLastResponse();
        client.LoginLevel1Function(config.l1_user, config.l1_pass);
    }
};

struct PollSerAction
{
    template <class Event>
    void operator()(const Event&, TelnetClient& client) const
    {
        std::cout << "[ACTION] SER GET_ALL\n";
        client.clearLastResponse();
        std::string buffer;
        client.SendCmdReceiveData("SER GET_ALL", buffer);
    }
};

// ================= GUARD FUNCTIONS =================
inline bool hasPrompt(const std::string& buffer)
{
    auto pos = buffer.find_last_not_of(" \r\n\t");
    if (pos == std::string::npos)
        return false;
    const char last = buffer[pos];
    return last == '>' || last == '#' || last == '$' || last == ':';
}

struct ConnectOkGuard
{
    bool operator()(const step_event&, const TelnetClient& client) const
    {
        return client.getLastIoResult();
    }
};

struct ConnectFailGuard
{
    bool operator()(const step_event&, const TelnetClient& client) const
    {
        return !client.getLastIoResult();
    }
};

struct Login1CompleteGuard
{
    bool operator()(const step_event&, const TelnetClient& client) const
    {
        const std::string& response = client.getLastResponse();
        return client.getLastIoResult() &&
               (response.find("Level 1") != std::string::npos || hasPrompt(response));
    }
};

struct Login1FailGuard
{
    bool operator()(const step_event&, const TelnetClient& client) const
    {
        return !Login1CompleteGuard{}(step_event{}, client);
    }
};

struct SerCompleteGuard
{
    bool operator()(const step_event&, const TelnetClient& client) const
    {
        const std::string& response = client.getLastResponse();
        return client.getLastIoResult() &&
               (response.find("SER Response Complete") != std::string::npos || hasPrompt(response));
    }
};

struct SerFailGuard
{
    bool operator()(const step_event&, const TelnetClient& client) const
    {
        return !SerCompleteGuard{}(step_event{}, client);
    }
};

// ================= SAFETY HANDLERS =================
inline auto on_unhandled = [](const unhandled_event&) {
    std::cout << "[UNHANDLED EVENT]\n";
};

inline auto on_unexpected = [](const auto&) {
    std::cout << "[UNEXPECTED EVENT]\n";
};

// ================= FSM =================
struct TelnetFSM
{
    auto operator()() const
    {
        using namespace sml;

        return make_transition_table(
            // Entry / Exit
            "Connecting"_s + on_entry<_> / ConnectAction{},
            "Login_L1"_s + on_entry<_> / Login1Action{},
            "Polling"_s + on_entry<_> / PollSerAction{},

            // Transitions
            *"Idle"_s + event<start_event> = "Connecting"_s,

            "Connecting"_s + event<step_event> [ ConnectOkGuard{} ] = "Login_L1"_s,
            "Connecting"_s + event<step_event> [ ConnectFailGuard{} ] = "Error"_s,

            "Login_L1"_s + event<step_event> [ Login1CompleteGuard{} ] = "Operational"_s,
            "Login_L1"_s + event<step_event> [ Login1FailGuard{} ] = "Error"_s,

            "Operational"_s + event<step_event> = "Polling"_s,

            "Polling"_s + event<step_event> [ SerCompleteGuard{} ] = "Operational"_s,
            "Polling"_s + event<step_event> [ SerFailGuard{} ] = "Error"_s,

            // Safety
            state<_> + event<unhandled_event> / on_unhandled = "Error"_s,
            state<_> + unexpected_event<_> / on_unexpected = "Error"_s
        );
    }
};
