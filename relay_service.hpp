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
     * @brief Construct with external TelnetClient reference.
     *
     * The caller retains ownership; the client must outlive the service.
     */
    explicit RelayService(TelnetClient& client)
        : client_(client)
    {
    }

    // ── Generic Command ────────────────────────────────────────────────

    /**
     * @brief Result of a generic relay command.
     */
    struct CommandResult
    {
        bool        success  = false;  ///< true when relay responded with ACK/OK
        std::string response;          ///< Raw relay response text
        std::string error;             ///< Error message on failure
    };

    /**
     * @brief Send an arbitrary command to the relay and check for ACK.
     *
     * @param cmd  Command string to send (e.g. "PAS LEVEL1 TAIL1")
     * @return CommandResult with success flag, raw response, and optional error
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
    TelnetClient& client_;
    std::mutex mutex_;
};
