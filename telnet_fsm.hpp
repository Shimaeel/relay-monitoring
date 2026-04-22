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

// ================= COMMAND FSM EVENTS =================

/**
 * @brief Event: User requested SER data fetch.
 * @details Fired when user clicks SER/Refresh button in the browser.
 */
struct cmd_ser_event {};

/**
 * @brief Event: User requested FILE DIR EVENTS command.
 * @details Fired to fetch COMTRADE file list from relay.
 */
struct cmd_file_dir_events_event {};

/**
 * @brief Event: User requested TAR (Target) command.
 * @details Fired when user clicks TAR button. Carries argument string.
 */
struct cmd_tar_event { std::string args; };

/**
 * @brief Event: User requested Ctrl+C (break/interrupt).
 * @details Sends ASCII 0x03 to the relay device.
 */
struct cmd_ctrlc_event {};

/**
 * @brief Event: User requested Ctrl+D (end-of-transmission).
 * @details Sends ASCII 0x04 to the relay device.
 */
struct cmd_ctrld_event {};

/**
 * @brief Event: User requested a SET (Settings) command.
 * @details Fired when user submits a setting change. Carries argument string.
 */
struct cmd_set_event { std::string args; };

// ================= COMMAND RESPONSE HOLDER =================

/**
 * @brief Reason a command failed (for smart retry logic).
 *
 * @details Classified by CommandFSM actions after SendCmdReceiveData().
 * The caller uses this to decide whether to retry directly (TIMEOUT),
 * reconnect first (CONN_LOST), or give up immediately (NONE on success).
 */
enum class CmdFailReason
{
    NONE,       ///< No failure — command succeeded
    TIMEOUT,    ///< Still connected but relay didn't respond in time
    CONN_LOST   ///< Connection dropped during command execution
};

/**
 * @brief Shared holder for the last command FSM response.
 *
 * @details Written by RelayCommandFSM actions, read by the calling
 * PipelineReceptionWorker method (handleUserCommand / executeCommand).
 */
struct CmdResponseHolder
{
    std::string response;          ///< Raw relay response text
    bool success = false;          ///< true if command succeeded
    CmdFailReason failReason = CmdFailReason::NONE;  ///< Failure classification
};

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
    std::string l2_pass;    ///< Level 2 password (2AC) — needed for PAS / SET / TRI
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

/**
 * @brief FSM Action: Log Error state entry and reset retry counter for recovery.
 *
 * @details Called on entry to Error state.  Logs the failure and resets
 * the retry counter so that a subsequent disconnect_event can trigger
 * a fresh reconnect cycle (24/7 auto-recovery).
 */
struct ErrorRecoveryAction
{
    template <class Event>
    void operator()(const Event&, RetryState& retry) const
    {
        retry.reset();
        std::cout << "[FSM] Error state entered \xe2\x80\x94 retries reset for auto-recovery\n";
    }
};


// ================= COMMAND FSM ACTIONS =================

/**
 * @brief Command Action: Send SER command to relay.
 * @details Stores response in CmdResponseHolder for caller to read.
 */
struct CmdSerAction
{
    void operator()(const cmd_ser_event&, TelnetClient& client, CmdResponseHolder& resp) const
    {
        client.clearLastResponse();
        std::string response;
        bool ok = client.SendCmdReceiveData("SER", response);
        resp.success = ok && !response.empty();
        resp.response = std::move(response);
        resp.failReason = resp.success ? CmdFailReason::NONE
                        : !client.isConnected() ? CmdFailReason::CONN_LOST
                        : CmdFailReason::TIMEOUT;
        std::cout << "[CmdFSM] SER " << (resp.success ? "OK" : "FAIL") << "\n";
    }
};

/**
 * @brief Command Action: Send FILE DIR EVENTS command to relay.
 * @details Stores response in CmdResponseHolder for caller to read.
 */
struct CmdFileDirEventsAction
{
    void operator()(const cmd_file_dir_events_event&, TelnetClient& client, CmdResponseHolder& resp) const
    {
        client.clearLastResponse();
        std::string response;
        // Use multi-page collection to handle pagination
        bool ok = client.SendCmdMultiPage("FILE DIR EVENTS", response);
        resp.success = ok && !response.empty();
        resp.response = std::move(response);
        resp.failReason = resp.success ? CmdFailReason::NONE
                        : !client.isConnected() ? CmdFailReason::CONN_LOST
                        : CmdFailReason::TIMEOUT;
        std::cout << "[CmdFSM] FILE DIR EVENTS " << (resp.success ? "OK" : "FAIL") << "\n";
    }
};

/**
 * @brief Command Action: Send TAR command to relay.
 * @details Sends "TAR <args>" and stores response.
 */
struct CmdTarAction
{
    void operator()(const cmd_tar_event& e, TelnetClient& client, CmdResponseHolder& resp) const
    {
        client.clearLastResponse();
        std::string cmd = "TAR";
        if (!e.args.empty()) cmd += " " + e.args;
        std::string response;
        bool ok = client.SendCmdReceiveData(cmd, response);
        resp.success = ok && !response.empty();
        resp.response = std::move(response);
        resp.failReason = resp.success ? CmdFailReason::NONE
                        : !client.isConnected() ? CmdFailReason::CONN_LOST
                        : CmdFailReason::TIMEOUT;
        std::cout << "[CmdFSM] TAR " << (resp.success ? "OK" : "FAIL") << "\n";
    }
};

/**
 * @brief Command Action: Send Ctrl+C (0x03) to relay.
 * @details Used to interrupt a running command on the relay device.
 */
struct CmdCtrlCAction
{
    void operator()(const cmd_ctrlc_event&, TelnetClient& client, CmdResponseHolder& resp) const
    {
        client.clearLastResponse();
        std::string response;
        bool ok = client.SendCmdReceiveData(std::string(1, '\x03'), response);
        resp.success = ok;
        resp.response = std::move(response);
        resp.failReason = resp.success ? CmdFailReason::NONE
                        : !client.isConnected() ? CmdFailReason::CONN_LOST
                        : CmdFailReason::TIMEOUT;
        std::cout << "[CmdFSM] CTRL+C " << (resp.success ? "OK" : "FAIL") << "\n";
    }
};

/**
 * @brief Command Action: Send Ctrl+D (0x04) to relay.
 * @details Used to signal end-of-transmission / logout on the relay device.
 */
struct CmdCtrlDAction
{
    void operator()(const cmd_ctrld_event&, TelnetClient& client, CmdResponseHolder& resp) const
    {
        client.clearLastResponse();
        std::string response;
        bool ok = client.SendCmdReceiveData(std::string(1, '\x04'), response);
        resp.success = ok;
        resp.response = std::move(response);
        resp.failReason = resp.success ? CmdFailReason::NONE
                        : !client.isConnected() ? CmdFailReason::CONN_LOST
                        : CmdFailReason::TIMEOUT;
        std::cout << "[CmdFSM] CTRL+D " << (resp.success ? "OK" : "FAIL") << "\n";
    }
};

/**
 * @brief Command Action: Send SET (Settings) command to relay.
 * @details Sends "SET <args>" and stores response.
 */
struct CmdSetAction
{
    void operator()(const cmd_set_event& e, TelnetClient& client, CmdResponseHolder& resp) const
    {
        client.clearLastResponse();
        std::string cmd = "SET";
        if (!e.args.empty()) cmd += " " + e.args;
        std::string response;
        // Use multi-page collection to handle "Press RETURN to continue"
        bool ok = client.SendCmdMultiPage(cmd, response);
        resp.success = ok && !response.empty();
        resp.response = std::move(response);
        resp.failReason = resp.success ? CmdFailReason::NONE
                        : !client.isConnected() ? CmdFailReason::CONN_LOST
                        : CmdFailReason::TIMEOUT;
        std::cout << "[CmdFSM] SET " << (resp.success ? "OK" : "FAIL") << "\n";
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
            "Error"_s          + on_entry<_> / ErrorRecoveryAction{},

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

            // Error recovery: disconnect_event triggers fresh reconnection cycle (24/7)
            "Error"_s + event<disconnect_event>  = "Connecting"_s,

            // Safety
            state<_> + event<unhandled_event> / on_unhandled = "Error"_s,
            state<_> + unexpected_event<_> / on_unexpected = "Error"_s
        );
    }
};

// ================= RELAY COMMAND FSM =================

/**
 * @brief Boost.SML State Machine for user-initiated relay commands.
 *
 * @details Separate from RelayConnectionFSM.  This FSM handles commands
 * (SER, TAR, EVE, CTRL+C, CTRL+D, SET) that are triggered by user
 * button clicks in the browser UI.  Each command event fires an action
 * that sends the command via TelnetClient and stores the result in
 * CmdResponseHolder.
 *
 * The FSM sits in Idle and returns to Idle after every command.
 * Connection readiness is checked by the worker BEFORE firing the event.
 *
 * ## State Diagram
 *
 * @code
 *                   cmd_ser_event / CmdSerAction
 *                   cmd_tar_event / CmdTarAction
 *                   cmd_eve_event / CmdEveAction
 *                   cmd_ctrlc_event / CmdCtrlCAction
 *   ┌──────────┐    cmd_ctrld_event / CmdCtrlDAction   ┌──────────┐
 *   │          │    cmd_set_event / CmdSetAction        │          │
 *   │  *Idle   ├───────────────────────────────────────►│   Idle   │
 *   │          │                                        │          │
 *   └──────────┘                                        └──────────┘
 * @endcode
 *
 * ## Dependencies (injected via sml::sm constructor)
 * - TelnetClient&       — sends commands to the relay
 * - CmdResponseHolder&  — stores the result for the caller
 *
 * @see RelayConnectionFSM   Manages connection lifecycle (FSM #1)
 * @see PipelineReceptionWorker  Orchestrates both FSMs
 */
struct RelayCommandFSM
{
    auto operator()() const
    {
        using namespace sml;

        return make_transition_table(
            // SER command
            *"Idle"_s + event<cmd_ser_event>   / CmdSerAction{}   = "Idle"_s,

            // TAR command
            "Idle"_s + event<cmd_tar_event>   / CmdTarAction{}   = "Idle"_s,

            // Ctrl+C (break)
            "Idle"_s + event<cmd_ctrlc_event> / CmdCtrlCAction{} = "Idle"_s,

            // Ctrl+D (end-of-transmission)
            "Idle"_s + event<cmd_ctrld_event> / CmdCtrlDAction{} = "Idle"_s,

            // SET (settings) command
            "Idle"_s + event<cmd_set_event>   / CmdSetAction{}   = "Idle"_s,

            // Safety — unknown events return to Idle
            state<_> + unexpected_event<_> / on_unexpected = "Idle"_s
        );
    }
};
