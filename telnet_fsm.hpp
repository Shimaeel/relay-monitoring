#pragma once

#include <boost/sml.hpp>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>

#include "client.hpp"
#include "ser_record.hpp"
#include "ser_database.hpp"

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

struct RetryState
{
    int max_retries = 3;
    int current_attempt = 0;
    std::chrono::seconds retry_delay{30};
    
    bool canRetry() const { return current_attempt < max_retries; }
    void reset() { current_attempt = 0; }
    void increment() { ++current_attempt; }
};

// ================= ACTION FUNCTIONS =================
struct ConnectAction
{
    template <class Event>
    void operator()(const Event&, TelnetClient& client, const ConnectionConfig& config) const
    {
        // std::cout << "[ACTION] Connect\n";
        client.clearLastResponse();
        client.connectCheck(config.host, config.port, config.timeout);
    }
};

struct Login1Action
{
    template <class Event>
    void operator()(const Event&, TelnetClient& client, const LoginConfig& config) const
    {
        // std::cout << "[ACTION] Login Level 1\n";
        client.clearLastResponse();
        client.LoginLevel1Function(config.l1_user, config.l1_pass);
    }
};

struct PollSerAction
{
    template <class Event>
    void operator()(const Event&, TelnetClient& client, SERDatabase& db) const
    {
        // std::cout << "[ACTION] Sending SER command\n";
        client.clearLastResponse();
        std::string buffer;
        client.SendCmdReceiveData("SER", buffer);

        // Parse and store SER records in database
        auto records = parseSERResponse(buffer);
        if (!records.empty())
        {
            int inserted = db.insertRecords(records);
            std::cout << "[DB] Stored " << inserted << " SER records\n";
        }
    }
};

struct RetryWaitAction
{
    template <class Event>
    void operator()(const Event&, RetryState& retry) const
    {
        retry.increment();
        std::cout << "[RETRY] Attempt " << retry.current_attempt << "/" << retry.max_retries 
                  << " - Waiting " << retry.retry_delay.count() << "s before retry...\n";
        std::this_thread::sleep_for(retry.retry_delay);
    }
};

struct ResetRetryAction
{
    template <class Event>
    void operator()(const Event&, RetryState& retry) const
    {
        retry.reset();
    }
};

// ================= GUARD FUNCTIONS =================
inline bool hasPrompt(const std::string& buffer)
{
    // Check last 30 characters for prompt, ignoring telnet control codes
    size_t len = buffer.length();
    // std::cout << "[DEBUG hasPrompt] Buffer length: " << len << "\n";
    if (len == 0) return false;
    
    // Look at last 30 characters for a prompt character
    size_t start = (len > 30) ? len - 30 : 0;
    // std::cout << "[DEBUG hasPrompt] Checking from pos " << start << " to " << len << "\n";
    for(size_t i = len; i > start; --i) {
        char c = buffer[i-1];
        if (c == '>' || c == '#' || c == '$' || c == ':' || c == '?') {
            // std::cout << "[DEBUG hasPrompt] Found '" << c <<  "' at pos " << (i-1) << "\n";
            return true;
        }
    }
    // std::cout << "[DEBUG hasPrompt] No prompt found\n";
    return false;
}

struct ConnectOkGuard
{
    bool operator()(const step_event&, const TelnetClient& client) const
    {
        bool result = client.getLastIoResult();
        // std::cout << "[DEBUG] ConnectOkGuard: " << (result ? "SUCCESS" : "FAIL") << "\n";
        return result;
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
        // std::cout << "[DEBUG] Login1CompleteGuard - IO Result: " << client.getLastIoResult() 
        //           << ", Response: [" << response << "], HasPrompt: " << hasPrompt(response) << "\n";
        return client.getLastIoResult() && hasPrompt(response);
    }
};

struct Login1FailGuard
{
    bool operator()(const step_event&, const TelnetClient& client) const
    {
        return !Login1CompleteGuard{}(step_event{}, client);
    }
};

struct InvalidLoginGuard
{
    bool operator()(const step_event&, const TelnetClient& client) const
    {
        const std::string& response = client.getLastResponse();
        return response.find("invalid") != std::string::npos ||
               response.find("Invalid") != std::string::npos ||
               response.find("INVALID") != std::string::npos ||
               response.find("error") != std::string::npos ||
               response.find("Error") != std::string::npos ||
               response.find("denied") != std::string::npos;
    }
};

struct CanRetryGuard
{
    bool operator()(const step_event&, const TelnetClient& client, const RetryState& retry) const
    {
        return InvalidLoginGuard{}(step_event{}, client) && retry.canRetry();
    }
};

struct MaxRetriesReachedGuard
{
    bool operator()(const step_event&, const TelnetClient& client, const RetryState& retry) const
    {
        return InvalidLoginGuard{}(step_event{}, client) && !retry.canRetry();
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
            "WaitingRetry"_s + on_entry<_> / RetryWaitAction{},
            "Operational"_s + on_entry<_> / ResetRetryAction{},

            // Transitions
            *"Idle"_s + event<start_event> = "Connecting"_s,

            "Connecting"_s + event<step_event> [ ConnectOkGuard{} ] = "Login_L1"_s,
            "Connecting"_s + event<step_event> [ ConnectFailGuard{} ] = "Error"_s,

            // Login with retry logic
            "Login_L1"_s + event<step_event> [ Login1CompleteGuard{} ] = "Operational"_s,
            "Login_L1"_s + event<step_event> [ CanRetryGuard{} ] = "WaitingRetry"_s,
            "Login_L1"_s + event<step_event> [ MaxRetriesReachedGuard{} ] = "Error"_s,

            // After waiting, retry connection
            "WaitingRetry"_s + event<step_event> = "Connecting"_s,

            "Operational"_s + event<step_event> = "Polling"_s,

            "Polling"_s + event<step_event> [ SerCompleteGuard{} ] = "Operational"_s,
            "Polling"_s + event<step_event> [ SerFailGuard{} ] = "Error"_s,

            // Safety
            state<_> + event<unhandled_event> / on_unhandled = "Error"_s,
            state<_> + unexpected_event<_> / on_unexpected = "Error"_s
        );
    }
};
