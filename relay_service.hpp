// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file relay_service.hpp
 * @brief Thread-safe relay communication service for generic command operations.
 *
 * @details Provides a thin service layer around TelnetClient for
 * sending commands, checking ACK responses, and Level 2 elevation.
 * Used by PasswordManager for password change operations.
 *
 * @see asn_tlv_codec.hpp      Shared BER utilities
 * @see password_manager.hpp   Uses this service for relay communication
 *
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

#include "dll_export.hpp"
#include "client.hpp"

#include <algorithm>
#include <mutex>
#include <string>

// ============================================================================
//  RelayService — thread-safe relay command service via TelnetClient
// ============================================================================

/**
 * @class RelayService
 * @brief Thread-safe wrapper around TelnetClient for relay command operations.
 *
 * @details All public methods lock the internal mutex so that
 * concurrent callers share the same underlying TelnetClient safely
 * (only one caller at a time sends Telnet I/O).
 *
 * ## Usage
 * @code
 * RelayService relay(client);
 * auto result = relay.sendRelayCommand("SER");
 * if (result.success)
 *     std::cout << "Response: " << result.response << "\n";
 * @endcode
 */
class TELNET_SML_API RelayService
{
public:
    /**
     * @brief Construct a service bound to an externally-owned TelnetClient.
     *
     * @details The service does not take ownership of @p client; the caller
     * must guarantee the client outlives every RelayService referencing it.
     * The internal mutex is initialised unlocked so the first command is
     * served without contention.
     *
     * @param client TelnetClient used for every I/O operation. Must already
     *               be connected (or connectable) before sendRelayCommand()
     *               is called.
     */
    explicit RelayService(TelnetClient& client)
        : client_(client)
    {
    }

    // ── Generic Command ────────────────────────────────────────────────

    /**
     * @struct CommandResult
     * @brief Outcome of sendRelayCommand().
     *
     * @details On success @c success is true, @c response contains the raw
     * relay output, and @c error is empty. On failure @c success is false,
     * @c response contains whatever was received before the error (may be
     * empty), and @c error holds a short human-readable reason suitable for
     * logging or propagation to the UI.
     */
    struct CommandResult
    {
        bool        success  = false;  ///< true when relay responded with ACK/OK/prompt.
        std::string response;          ///< Raw relay response text.
        std::string error;             ///< Populated on failure; empty on success.
    };

    /**
     * @brief Send a command to the relay and classify the response.
     *
     * @details Takes the service mutex so only one command is in flight at
     * a time (the underlying TelnetClient is not reentrant), checks that
     * the client is connected, issues the command via
     * TelnetClient::SendCmdReceiveData(), then scans the uppercased
     * response for any of @c ACK, @c OK, or the relay prompt @c "=>" to
     * decide whether the relay accepted the command.
     *
     * @param cmd  Command string (e.g. @c "PAS LEVEL1 TAIL1").
     * @return A CommandResult — see its documentation for the field
     *         semantics on success vs. failure.
     *
     * @pre The caller must have connected @c client_ (typically via the
     *      owning pipeline's login sequence) before invoking this method.
     * @note Thread-safe; callers in different threads are serialised by
     *       @c mutex_.
     */
    CommandResult sendRelayCommand(const std::string& cmd)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        CommandResult result;

        if (!client_.isConnected())
        {
            result.error = "Relay not connected";
            return result;
        }

        std::string rawResponse;
        bool ok = client_.SendCmdReceiveData(cmd, rawResponse);

        if (!ok)
        {
            result.error = "Command failed or empty response";
            return result;
        }

        result.response = rawResponse;

        // Check for ACK / OK in response
        std::string upper = rawResponse;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

        if (upper.find("ACK") != std::string::npos
            || upper.find("OK") != std::string::npos
            || upper.find("=>") != std::string::npos)
        {
            result.success = true;
        }
        else
        {
            result.error = "No ACK received from relay";
        }

        return result;
    }

private:
    TelnetClient& client_; ///< Externally-owned Telnet client used for all I/O.
    std::mutex mutex_;     ///< Serialises concurrent sendRelayCommand() calls.
};
