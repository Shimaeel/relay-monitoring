// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file telnet_fsm.hpp
 * @brief Boost.SML Finite State Machine for Telnet Communication
 * 
 * @details This header implements a complete finite state machine using Boost.SML
 * for managing Telnet communication with substation relay devices. The FSM handles:
 * - Connection establishment with retry logic
 * - Multi-level authentication
 * - System Event Record (SER) polling
 * - Error recovery and state transitions
 * 
 * ## State Machine Diagram
 * 
 * @dot
 * digraph TelnetFSM {
 *     rankdir=TB;
 *     node [shape=ellipse, style=filled];
 *     
 *     // States with colors
 *     Idle [fillcolor=lightgray, label="Idle\n(Initial)"];
 *     Connecting [fillcolor=lightyellow];
 *     Login_L1 [fillcolor=lightyellow, label="Login_L1\n(Authentication)"];
 *     WaitingRetry [fillcolor=orange, label="WaitingRetry\n(Delay)"];
 *     Operational [fillcolor=lightgreen];
 *     Polling [fillcolor=lightblue, label="Polling\n(SER Query)"];
 *     Done [fillcolor=lightgreen, label="Done\n(Success)"];
 *     Error [fillcolor=lightcoral, label="Error\n(Failed)"];
 *     
 *     // Transitions
 *     Idle -> Connecting [label="start_event"];
 *     Connecting -> Login_L1 [label="step[ConnectOk]"];
 *     Connecting -> Error [label="step[ConnectFail]"];
 *     Login_L1 -> Operational [label="step[LoginComplete]"];
 *     Login_L1 -> WaitingRetry [label="step[CanRetry]"];
 *     Login_L1 -> Error [label="step[MaxRetries]"];
 *     WaitingRetry -> Connecting [label="step"];
 *     Operational -> Polling [label="step"];
 *     Polling -> Done [label="step[SerComplete]"];
 *     Polling -> Error [label="step[SerFail]"];
 *     Done -> Done [label="step"];
 * }
 * @enddot
 * 
 * ## Component Architecture
 * 
 * @dot
 * digraph Components {
 *     rankdir=LR;
 *     node [shape=box, style=filled, fillcolor=lightblue];
 *     
 *     subgraph cluster_fsm {
 *         label="TelnetFSM";
 *         Events [label="Events\n(start, step, unhandled)"];
 *         States [label="States\n(7 states)"];
 *         Guards [label="Guards\n(condition checks)"];
 *         Actions [label="Actions\n(side effects)"];
 *     }
 *     
 *     subgraph cluster_deps {
 *         label="Dependencies";
 *         Client [label="TelnetClient"];
 *         Config [label="ConnectionConfig\nLoginConfig"];
 *         Retry [label="RetryState"];
 *         DB [label="SERDatabase"];
 *     }
 *     
 *     Actions -> Client [label="uses"];
 *     Actions -> Config [label="reads"];
 *     Actions -> Retry [label="modifies"];
 *     Actions -> DB [label="stores"];
 *     Guards -> Client [label="checks"];
 *     Guards -> Retry [label="checks"];
 * }
 * @enddot
 * 
 * ## Usage Example
 * 
 * @code{.cpp}
 * TelnetClient client;
 * ConnectionConfig conn{"192.168.0.2", 23, 2000ms};
 * LoginConfig creds{"user", "pass"};
 * RetryState retry{3, 0, 30s};
 * SERDatabase db("records.db");
 * 
 * sml::sm<TelnetFSM> fsm{client, conn, creds, retry, db};
 * fsm.process_event(start_event{});
 * 
 * while (!fsm.is("Done"_s) && !fsm.is("Error"_s)) {
 *     fsm.process_event(step_event{});
 *     std::this_thread::sleep_for(200ms);
 * }
 * @endcode
 * 
 * @see TelnetClient Network communication class
 * @see SERDatabase Database for storing records
 * @see https://boost-ext.github.io/sml/ Boost.SML documentation
 * 
 * @note Requires Boost.SML header-only library
 * @note All guards and actions are stateless functors
 * 
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

#include <boost/sml.hpp>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>

#include "client.hpp"
#include "ser_record.hpp"
#include "ser_database.hpp"
#include "asn_tlv_codec.hpp"
#include "shared_memory/shared_ring_buffer.hpp"

namespace sml = boost::sml;

// ================= EVENTS =================

/**
 * @brief Event to start the FSM from Idle state
 * 
 * @details Triggers the initial transition from Idle to Connecting.
 * Should be sent once after FSM construction.
 */
struct start_event {};

/**
 * @brief Event to advance the FSM through states
 * 
 * @details Main event for state machine progression. Guards determine
 * which transition to take based on client and retry state.
 * Send periodically in the main loop.
 */
struct step_event {};

/**
 * @brief Event for unhandled situations
 * 
 * @details Sent when step_event was not handled by current state.
 * Triggers safety transition to Error state.
 */
struct unhandled_event {};

/**
 * @brief Event to signal connection loss from Operational state
 *
 * @details Triggers reconnection cycle when the active Telnet session
 * drops (command failure, timeout, etc.).  Used by RelayConnectionFSM
 * inside PipelineReceptionWorker.
 */
struct disconnect_event {};

// ================= CONFIG =================

/**
 * @brief Connection configuration parameters
 * 
 * @details Holds network connection settings passed to TelnetClient::connectCheck().
 * Configured once at startup and passed to FSM as dependency.
 */
struct ConnectionConfig
{
    std::string host;                      ///< Target hostname or IP address
    int port;                              ///< Target port (typically 23 for Telnet)
    std::chrono::milliseconds timeout;     ///< Connection timeout duration
};

/**
 * @brief Authentication credentials configuration
 * 
 * @details Holds Level 1 username and password for relay authentication.
 * Used by Login1Action for authentication sequence.
 */
struct LoginConfig
{
    std::string l1_user;    ///< Level 1 username
    std::string l1_pass;    ///< Level 1 password
};

/**
 * @brief Retry state management
 * 
 * @details Tracks retry attempts for authentication failures.
 * Modified by RetryWaitAction and ResetRetryAction.
 * 
 * ## Retry Flow
 * 
 * @msc
 * Login,Guard,Action,Timer;
 * Login->Guard [label="CanRetry?"];
 * Guard->Action [label="yes"];
 * Action->Timer [label="wait delay"];
 * Timer->Login [label="retry"];
 * @endmsc
 */
struct RetryState
{
    int max_retries = 3;                              ///< Maximum retry attempts allowed
    int current_attempt = 0;                           ///< Current attempt counter
    std::chrono::seconds retry_delay{30};             ///< Delay between retry attempts
    
    /**
     * @brief Check if more retry attempts are available
     * @return true if current_attempt < max_retries
     */
    bool canRetry() const { return current_attempt < max_retries; }
    
    /**
     * @brief Reset retry counter to zero
     * @post current_attempt == 0
     */
    void reset() { current_attempt = 0; }
    
    /**
     * @brief Increment retry counter
     * @post current_attempt increased by 1
     */
    void increment() { ++current_attempt; }
};

// ================= ACTION FUNCTIONS =================

/**
 * @brief FSM Action: Establish connection to relay
 * 
 * @details Called on entry to "Connecting" state.
 * Clears previous response and attempts TCP connection.
 * 
 * @tparam Event The triggering event type
 * @see ConnectOkGuard Guard for checking connection success
 */
struct ConnectAction
{
    /**
     * @brief Execute connection attempt
     * 
     * @param event The triggering event (unused)
     * @param client TelnetClient to use for connection
     * @param config Connection parameters (host, port, timeout)
     */
    template <class Event>
    void operator()(const Event&, TelnetClient& client, const ConnectionConfig& config) const
    {
        // std::cout << "[ACTION] Connect\n";
        client.clearLastResponse();
        client.connectCheck(config.host, config.port, config.timeout);
    }
};

/**
 * @brief FSM Action: Perform Level 1 authentication
 * 
 * @details Called on entry to "Login_L1" state.
 * Sends username and password using TelnetClient::LoginLevel1Function().
 * 
 * @tparam Event The triggering event type
 * @see Login1CompleteGuard Guard for checking login success
 */
struct Login1Action
{
    /**
     * @brief Execute Level 1 login sequence
     * 
     * @param event The triggering event (unused)
     * @param client TelnetClient for sending credentials
     * @param config Login parameters (username, password)
     */
    template <class Event>
    void operator()(const Event&, TelnetClient& client, const LoginConfig& config) const
    {
        // std::cout << "[ACTION] Login Level 1\n";
        client.clearLastResponse();
        client.LoginLevel1Function(config.l1_user, config.l1_pass);
    }
};

/**
 * @brief FSM Action: Poll for System Event Records
 * 
 * @details Called on entry to "Polling" state.
 * Sends SER command, parses response, and stores records in database.
 * 
 * ## Data Flow
 * 
 * @dot
 * digraph PollFlow {
 *     rankdir=LR;
 *     node [shape=box];
 *     
 *     SER [label="Send SER"];
 *     Parse [label="Parse Response"];
 *     Store [label="Store in DB"];
 *     
 *     SER -> Parse -> Store;
 * }
 * @enddot
 * 
 * @tparam Event The triggering event type
 * @see parseSERResponse Function for parsing SER response
 * @see SerCompleteGuard Guard for checking poll success
 */
struct PollSerAction
{
    /**
     * @brief Execute SER polling and storage
     * 
     * @param event The triggering event (unused)
     * @param client TelnetClient for sending SER command
     * @param db Database for storing parsed records
     */
    template <class Event>
    void operator()(const Event&, TelnetClient& client, SERDatabase& db, SharedRingBuffer& ring) const
    {
        client.clearLastResponse();
        std::string buffer;
        client.SendCmdReceiveData("SER", buffer);

        // Parse and store SER records in database
        auto records = parseSERResponse(buffer);
        
        if (!records.empty())
        {
            int inserted = db.insertRecords(records);
            std::cout << "[SER] Parsed " << records.size() << " records, stored " << inserted << " in database\n";
            std::cout << "Records size before TLV encode: "
          << records.size() << std::endl;

            auto payload = asn_tlv::encodeSerRecordsToTlv(records);
            if (!payload.empty())
            {
                ring.write(payload.data(), payload.size());
            }
        }
        else
        {
            std::cout << "[SER] No records parsed - check response format!\n";
        }
    }
};

/**
 * @brief FSM Action: Wait before retry attempt
 * 
 * @details Called on entry to "WaitingRetry" state.
 * Increments retry counter and sleeps for configured delay.
 * 
 * @tparam Event The triggering event type
 */
struct RetryWaitAction
{
    /**
     * @brief Execute retry wait
     * 
     * @param event The triggering event (unused)
     * @param retry Retry state to increment and read delay from
     */
    template <class Event>
    void operator()(const Event&, RetryState& retry) const
    {
        retry.increment();
        std::cout << "[RETRY] Attempt " << retry.current_attempt << "/" << retry.max_retries 
                  << " - Waiting " << retry.retry_delay.count() << "s before retry...\n";
        std::this_thread::sleep_for(retry.retry_delay);
    }
};

/**
 * @brief FSM Action: Reset retry counter
 * 
 * @details Called on entry to "Operational" state.
 * Resets retry counter after successful login.
 * 
 * @tparam Event The triggering event type
 */
struct ResetRetryAction
{
    /**
     * @brief Execute retry counter reset
     * 
     * @param event The triggering event (unused)
     * @param retry Retry state to reset
     */
    template <class Event>
    void operator()(const Event&, RetryState& retry) const
    {
        retry.reset();
    }
};

/**
 * @brief FSM Action: Log connection-wait state entry (no counter increment)
 *
 * @details Called on entry to ConnectWait state in RelayConnectionFSM.
 * Connection failures are always retried without limit, so no retry
 * counter is modified here.
 */
struct ConnectWaitLogAction
{
    template <class Event>
    void operator()(const Event&) const
    {
        std::cout << "[FSM] Connection failed \xe2\x80\x94 waiting before retry\n";
    }
};

/**
 * @brief FSM Action: Increment login retry counter
 *
 * @details Called on entry to LoginRetryWait state after explicit
 * login rejection.  Increments the retry counter so that
 * CanRetryGuard / MaxRetriesReachedGuard can limit attempts.
 */
struct LoginRetryIncrAction
{
    template <class Event>
    void operator()(const Event&, RetryState& retry) const
    {
        retry.increment();
        std::cout << "[FSM] Login retry " << retry.current_attempt
                  << "/" << retry.max_retries << "\n";
    }
};

// ================= GUARD FUNCTIONS =================

/**
 * @brief Check if buffer contains a command prompt
 * 
 * @details Helper function for guards. Searches last 30 characters
 * for prompt characters: >, #, $, :, ?
 * 
 * @param buffer Response buffer to check
 * @return true if prompt character found
 */
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

/**
 * @brief FSM Guard: Check if connection succeeded
 * 
 * @details Returns true if last I/O operation succeeded (connection established).
 * Used for transition from Connecting to Login_L1.
 */
struct ConnectOkGuard
{
    /**
     * @brief Evaluate connection success
     * 
     * @param event The step_event triggering guard evaluation
     * @param client TelnetClient to check state of
     * @return true if connection succeeded
     */
    bool operator()(const step_event&, const TelnetClient& client) const
    {
        bool result = client.getLastIoResult();
        // std::cout << "[DEBUG] ConnectOkGuard: " << (result ? "SUCCESS" : "FAIL") << "\n";
        return result;
    }
};

/**
 * @brief FSM Guard: Check if connection failed
 * 
 * @details Returns true if last I/O operation failed.
 * Used for transition from Connecting to Error.
 */
struct ConnectFailGuard
{
    /**
     * @brief Evaluate connection failure
     * 
     * @param event The step_event triggering guard evaluation
     * @param client TelnetClient to check state of
     * @return true if connection failed
     */
    bool operator()(const step_event&, const TelnetClient& client) const
    {
        return !client.getLastIoResult();
    }
};

/**
 * @brief FSM Guard: Check if Level 1 login completed successfully
 * 
 * @details Returns true if login succeeded:
 * - Last I/O operation succeeded
 * - Response contains a command prompt
 */
struct Login1CompleteGuard
{
    /**
     * @brief Evaluate login completion
     * 
     * @param event The step_event triggering guard evaluation
     * @param client TelnetClient to check state of
     * @return true if login succeeded and prompt received
     */
    bool operator()(const step_event&, const TelnetClient& client) const
    {
        const std::string& response = client.getLastResponse();
        // std::cout << "[DEBUG] Login1CompleteGuard - IO Result: " << client.getLastIoResult() 
        //           << ", Response: [" << response << "], HasPrompt: " << hasPrompt(response) << "\n";
        return client.getLastIoResult() && hasPrompt(response);
    }
};

/**
 * @brief FSM Guard: Check if Level 1 login failed
 * 
 * @details Inverse of Login1CompleteGuard.
 */
struct Login1FailGuard
{
    /**
     * @brief Evaluate login failure
     * 
     * @param event The step_event triggering guard evaluation
     * @param client TelnetClient to check state of
     * @return true if login did not complete successfully
     */
    bool operator()(const step_event&, const TelnetClient& client) const
    {
        return !Login1CompleteGuard{}(step_event{}, client);
    }
};

/**
 * @brief FSM Guard: Check if login was explicitly rejected
 * 
 * @details Searches response for error indicators:
 * "invalid", "Invalid", "INVALID", "error", "Error", "denied"
 */
struct InvalidLoginGuard
{
    /**
     * @brief Evaluate for invalid credentials response
     * 
     * @param event The step_event triggering guard evaluation
     * @param client TelnetClient to check response of
     * @return true if response indicates invalid login
     */
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

/**
 * @brief FSM Guard: Check if retry is possible after invalid login
 * 
 * @details Returns true if:
 * - Login was explicitly rejected (InvalidLoginGuard)
 * - More retry attempts are available
 */
struct CanRetryGuard
{
    /**
     * @brief Evaluate if retry should be attempted
     * 
     * @param event The step_event triggering guard evaluation
     * @param client TelnetClient to check response of
     * @param retry RetryState to check attempt count
     * @return true if should retry authentication
     */
    bool operator()(const step_event&, const TelnetClient& client, const RetryState& retry) const
    {
        return InvalidLoginGuard{}(step_event{}, client) && retry.canRetry();
    }
};

/**
 * @brief FSM Guard: Check if max retries reached after invalid login
 * 
 * @details Returns true if:
 * - Login was explicitly rejected (InvalidLoginGuard)
 * - No more retry attempts available
 */
struct MaxRetriesReachedGuard
{
    /**
     * @brief Evaluate if max retries exceeded
     * 
     * @param event The step_event triggering guard evaluation
     * @param client TelnetClient to check response of
     * @param retry RetryState to check attempt count
     * @return true if should transition to Error state
     */
    bool operator()(const step_event&, const TelnetClient& client, const RetryState& retry) const
    {
        return InvalidLoginGuard{}(step_event{}, client) && !retry.canRetry();
    }
};

/**
 * @brief FSM Guard: Check if SER polling completed successfully
 * 
 * @details Returns true if:
 * - Last I/O operation succeeded
 * - Response contains completion marker or prompt
 */
struct SerCompleteGuard
{
    /**
     * @brief Evaluate SER polling completion
     * 
     * @param event The step_event triggering guard evaluation
     * @param client TelnetClient to check state of
     * @return true if SER data received successfully
     */
    bool operator()(const step_event&, const TelnetClient& client) const
    {
        const std::string& response = client.getLastResponse();
        return client.getLastIoResult() &&
               (response.find("SER Response Complete") != std::string::npos || hasPrompt(response));
    }
};

/**
 * @brief FSM Guard: Check if SER polling failed
 * 
 * @details Inverse of SerCompleteGuard.
 */
struct SerFailGuard
{
    /**
     * @brief Evaluate SER polling failure
     * 
     * @param event The step_event triggering guard evaluation
     * @param client TelnetClient to check state of
     * @return true if SER polling did not complete successfully
     */
    bool operator()(const step_event&, const TelnetClient& client) const
    {
        return !SerCompleteGuard{}(step_event{}, client);
    }
};

// ================= SAFETY HANDLERS =================

/**
 * @brief Lambda handler for unhandled events
 * @details Logs warning when step_event is not handled by current state
 */
inline auto on_unhandled = [](const unhandled_event&) {
    std::cout << "[UNHANDLED EVENT]\n";
};

/**
 * @brief Lambda handler for unexpected events
 * @details Logs warning for any unexpected event type
 */
inline auto on_unexpected = [](const auto&) {
    std::cout << "[UNEXPECTED EVENT]\n";
};

// ================= FSM =================

/**
 * @brief Boost.SML State Machine for Telnet Communication
 * 
 * @details Defines the complete state machine transition table for managing
 * Telnet communication workflow. Uses Boost.SML DSL for declarative state
 * machine definition.
 * 
 * ## States
 * 
 * | State | Description |
 * |-------|-------------|
 * | Idle | Initial state, waiting for start event |
 * | Connecting | Establishing TCP connection |
 * | Login_L1 | Performing Level 1 authentication |
 * | WaitingRetry | Waiting before retry attempt |
 * | Operational | Successfully authenticated |
 * | Polling | Querying SER data |
 * | Done | Successfully completed |
 * | Error | Terminal error state |
 * 
 * ## Transition Table
 * 
 * @dot
 * digraph Transitions {
 *     rankdir=LR;
 *     node [shape=box, style=filled, fillcolor=lightyellow];
 *     
 *     table [shape=none, label=<
 *         <TABLE BORDER="1" CELLBORDER="1" CELLSPACING="0">
 *             <TR><TD><B>Source</B></TD><TD><B>Event</B></TD><TD><B>Guard</B></TD><TD><B>Target</B></TD></TR>
 *             <TR><TD>Idle</TD><TD>start</TD><TD>-</TD><TD>Connecting</TD></TR>
 *             <TR><TD>Connecting</TD><TD>step</TD><TD>ConnectOk</TD><TD>Login_L1</TD></TR>
 *             <TR><TD>Connecting</TD><TD>step</TD><TD>ConnectFail</TD><TD>Error</TD></TR>
 *             <TR><TD>Login_L1</TD><TD>step</TD><TD>LoginComplete</TD><TD>Operational</TD></TR>
 *             <TR><TD>Login_L1</TD><TD>step</TD><TD>CanRetry</TD><TD>WaitingRetry</TD></TR>
 *             <TR><TD>Login_L1</TD><TD>step</TD><TD>MaxRetries</TD><TD>Error</TD></TR>
 *             <TR><TD>WaitingRetry</TD><TD>step</TD><TD>-</TD><TD>Connecting</TD></TR>
 *             <TR><TD>Operational</TD><TD>step</TD><TD>-</TD><TD>Polling</TD></TR>
 *             <TR><TD>Polling</TD><TD>step</TD><TD>SerComplete</TD><TD>Done</TD></TR>
 *             <TR><TD>Polling</TD><TD>step</TD><TD>SerFail</TD><TD>Error</TD></TR>
 *         </TABLE>
 *     >];
 * }
 * @enddot
 * 
 * ## Dependencies (injected via FSM constructor)
 * - TelnetClient& client - Network communication
 * - ConnectionConfig& conn - Connection parameters
 * - LoginConfig& creds - Authentication credentials
 * - RetryState& retry - Retry state tracking
 * - SERDatabase& db - Record storage
 * - SharedRingBuffer& ring - ASN payload transfer
 */
struct TelnetFSM
{
    /**
     * @brief Define FSM transition table
     * 
     * @details Uses Boost.SML DSL to define:
     * - Entry actions for states
     * - Event-driven transitions with guards
     * - Safety handlers for unexpected events
     * 
     * @return auto The complete transition table
     */
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

            // After successful poll, go to Done state (poll only once)
            "Polling"_s + event<step_event> [ SerCompleteGuard{} ] = "Done"_s,
            "Polling"_s + event<step_event> [ SerFailGuard{} ] = "Error"_s,

            // Done state - stay here (no more polling)
            "Done"_s + event<step_event> = "Done"_s,

            // Safety
            state<_> + event<unhandled_event> / on_unhandled = "Error"_s,
            state<_> + unexpected_event<_> / on_unexpected = "Error"_s
        );
    }
};

// ================= RELAY CONNECTION FSM =================

/**
 * @brief Boost.SML State Machine for per-relay connection lifecycle.
 *
 * @details Used by PipelineReceptionWorker (relay_pipeline.hpp).
 * Manages connect \xe2\x86\x92 login \xe2\x86\x92 operational cycle with automatic
 * reconnection on disconnect and separate retry handling for
 * connection failures (unlimited) vs. login failures (limited).
 *
 * ## State Diagram
 *
 * @code
 *                     \xe2\x94\x8c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 disconnect_event \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x90
 *    start_event      \xe2\x96\xbc                               \xe2\x94\x82
 *   *Idle \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x96\xba Connecting                          \xe2\x94\x82
 *                     \xe2\x94\x82                               \xe2\x94\x82
 *              \xe2\x94\x8c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xb4\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x90                        \xe2\x94\x82
 *         [ConnectOk]    [ConnectFail]                 \xe2\x94\x82
 *              \xe2\x94\x82              \xe2\x94\x82                        \xe2\x94\x82
 *              \xe2\x96\xbc              \xe2\x96\xbc                        \xe2\x94\x82
 *          Login_L1    ConnectWait \xe2\x94\x80step\xe2\x94\x80\xe2\x96\xba Connecting  \xe2\x94\x82
 *              \xe2\x94\x82                                      \xe2\x94\x82
 *     \xe2\x94\x8c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xac\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x90              \xe2\x94\x82
 * [Complete] [CanRetry] [MaxRetries] [Fail]       \xe2\x94\x82
 *     \xe2\x94\x82        \xe2\x94\x82          \xe2\x94\x82          \xe2\x94\x82          \xe2\x94\x82
 *     \xe2\x96\xbc        \xe2\x96\xbc          \xe2\x96\xbc          \xe2\x96\xbc          \xe2\x94\x82
 * Operational  LoginRetryWait  Error  ConnectWait  \xe2\x94\x82
 *     \xe2\x94\x82  \xe2\x96\xb2       \xe2\x94\x82                              \xe2\x94\x82
 *     \xe2\x94\x82  \xe2\x94\x94\xe2\x94\x80step  \xe2\x94\x94\xe2\x94\x80step\xe2\x94\x80\xe2\x96\xba Connecting              \xe2\x94\x82
 *     \xe2\x94\x82                                          \xe2\x94\x82
 *     \xe2\x94\x94\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x98
 * @endcode
 *
 * ## States
 *
 * | State           | Description                                  |
 * |-----------------|----------------------------------------------|
 * | Idle            | Initial — waiting for start_event            |
 * | Connecting      | TCP connect attempt (ConnectAction on entry)  |
 * | ConnectWait     | Pause before connect retry (no counter incr) |
 * | Login_L1        | Level-1 authentication (Login1Action on entry)|
 * | LoginRetryWait  | Pause before login retry (counter increments) |
 * | Operational     | Ready for command execution                   |
 * | Error           | Terminal — login retries exhausted             |
 *
 * ## Differences from TelnetFSM
 *
 * | Aspect           | TelnetFSM        | RelayConnectionFSM       |
 * |------------------|------------------|--------------------------|
 * | ConnectFail      | \xe2\x86\x92 Error           | \xe2\x86\x92 ConnectWait (unlimited)  |
 * | Polling/Done     | Present          | Removed (pipeline handles)|
 * | Operational      | \xe2\x86\x92 Polling         | Self-loop (ready)         |
 * | Disconnect       | N/A              | \xe2\x86\x92 Connecting (reconnect)  |
 * | Retry wait       | Blocking (30 s)  | Non-blocking (worker loop)|
 *
 * ## Dependencies (injected via sml::sm constructor)
 * - TelnetClient&       — network communication
 * - ConnectionConfig&   — host, port, timeout
 * - LoginConfig&        — username, password
 * - RetryState&         — login retry counter
 *
 * @see PipelineReceptionWorker  Uses this FSM for connection lifecycle
 * @see TelnetFSM                Original single-shot FSM (retained)
 */
struct RelayConnectionFSM
{
    auto operator()() const
    {
        using namespace sml;

        return make_transition_table(
            // Entry actions
            "Connecting"_s     + on_entry<_> / ConnectAction{},
            "Login_L1"_s       + on_entry<_> / Login1Action{},
            "ConnectWait"_s    + on_entry<_> / ConnectWaitLogAction{},
            "LoginRetryWait"_s + on_entry<_> / LoginRetryIncrAction{},
            "Operational"_s    + on_entry<_> / ResetRetryAction{},

            // Idle -> start
            *"Idle"_s + event<start_event> = "Connecting"_s,

            // Connection
            "Connecting"_s + event<step_event> [ ConnectOkGuard{} ]   = "Login_L1"_s,
            "Connecting"_s + event<step_event> [ ConnectFailGuard{} ] = "ConnectWait"_s,

            "ConnectWait"_s + event<step_event> = "Connecting"_s,

            // Authentication (evaluated in declaration order)
            "Login_L1"_s + event<step_event> [ Login1CompleteGuard{} ]      = "Operational"_s,
            "Login_L1"_s + event<step_event> [ CanRetryGuard{} ]            = "LoginRetryWait"_s,
            "Login_L1"_s + event<step_event> [ MaxRetriesReachedGuard{} ]   = "Error"_s,
            "Login_L1"_s + event<step_event> [ Login1FailGuard{} ]          = "ConnectWait"_s,

            "LoginRetryWait"_s + event<step_event> = "Connecting"_s,

            // Operational (self-loop; disconnect triggers reconnect)
            "Operational"_s + event<step_event>       = "Operational"_s,
            "Operational"_s + event<disconnect_event>  = "Connecting"_s,

            // Safety
            state<_> + event<unhandled_event> / on_unhandled = "Error"_s,
            state<_> + unexpected_event<_> / on_unexpected = "Error"_s
        );
    }
};
