#pragma once

#include <boost/sml.hpp>
#include <iostream>
#include "client.hpp"

namespace sml = boost::sml;

// ================= EVENTS =================
struct start_event {};

struct connect_ok {};
struct connect_fail {};

struct login1_ok {};
struct login1_fail {};

struct login2_ok {};
struct login2_fail {};

struct ser_ok {};
struct ser_fail {};


// ================= ACTION FUNCTIONS =================
struct ConnectAction {
    void operator()(const start_event&, TelnetClient& client) const {
        std::cout << "[ACTION] Start connection\n";

        std::string dummy;
        client.SendCmdReceiveData("", dummy);
    }
};

struct Login1Action {
    void operator()(const login1_ok&, TelnetClient& client) const {
        std::cout << "[ACTION] Login Level 1 command sent\n";

        std::string buffer;
        client.SendCmdReceiveData("LOGIN_LEVEL_1", buffer);
    }
};

struct Login2Action {
    void operator()(const login2_ok&, TelnetClient& client) const {
        std::cout << "[ACTION] Login Level 2 command sent\n";

        std::string buffer;
        client.SendCmdReceiveData("LOGIN_LEVEL_2", buffer);
    }
};

struct SendSERAction {
    void operator()(const ser_ok&, TelnetClient& client) const {
        std::cout << "[ACTION] Sending SER GET_ALL\n";
        std::string buffer;
        client.SendCmdReceiveData("SER GET_ALL", buffer);
    }
};



// ================= GUARD FUNCTIONS =================
struct AlwaysTrueGuard {
    template <typename Event>
    bool operator()(const Event&) const {
        return true;
    }
};

// 🔴 STEP-6 GUARD (ADDED)
struct Login1ResponseOkGuard {
    bool operator()(const login1_ok&, TelnetClient& client) const {

        // 🔹 TEMP SIMPLE CHECK (example strings)
        // Replace keywords based on your relay output
        const std::string& response = client.getLastResponse();

        if (response.find("Login successful") != std::string::npos ||
            response.find(">") != std::string::npos)
        {
            std::cout << "[GUARD] LoginLevel1 SUCCESS\n";
            return true;
        }

        std::cout << "[GUARD] LoginLevel1 FAILED\n";
        return false;
    }
};


// ================= ENTRY FUNCTIONS =================
struct OnConnectingEntry {
    void operator()() const {
        std::cout << "[ENTRY] Connecting\n";
    }
};

struct OnLogin1Entry {
    void operator()() const {
        std::cout << "[ENTRY] LoginLevel1\n";
    }
};


// ================= EXIT FUNCTIONS =================
struct OnConnectingExit {
    void operator()() const {
        std::cout << "[EXIT] Connecting\n";
    }
};

struct OnLogin1Exit {
    void operator()() const {
        std::cout << "[EXIT] LoginLevel1\n";
    }
};


// ================= SAFETY HANDLERS =================
auto on_unhandled = [](auto) {
    std::cout << "[UNHANDLED EVENT]\n";
};

auto on_unexpected = [](auto) {
    std::cout << "[UNEXPECTED EVENT]\n";
};


// ================= FSM =================
struct TelnetFSM {
    auto operator()() const {
        using namespace sml;

        return make_transition_table(
            // Entry / Exit
            "Connecting"_s + on_entry<_> / OnConnectingEntry{},
            "Connecting"_s + on_exit<_>  / OnConnectingExit{},

            "LoginLevel1"_s + on_entry<_> / OnLogin1Entry{},
            "LoginLevel1"_s + on_exit<_>  / OnLogin1Exit{},

            // Transitions
            *"Idle"_s + event<start_event> / ConnectAction{} = "Connecting"_s,

            "Connecting"_s + event<connect_ok> [ AlwaysTrueGuard{} ] = "LoginLevel1"_s,
            "Connecting"_s + event<connect_fail> = "Error"_s,

            // 🔴 STEP-6 GUARD WIRED HERE
            "LoginLevel1"_s
                + event<login1_ok>
                [ Login1ResponseOkGuard{} ]
                / Login1Action{}
                = "LoginLevel2"_s,

            "LoginLevel1"_s + event<login1_fail> = "Error"_s,

            "LoginLevel2"_s + event<login2_ok> / Login2Action{} = "SendSER"_s,
            "LoginLevel2"_s + event<login2_fail> = "Error"_s,

            "SendSER"_s + event<ser_ok> = "Success"_s,
            "SendSER"_s + event<ser_fail> = "Error"_s,

            // Safety
            state<_> + unexpected_event<_> / on_unexpected
        );
    }
};
