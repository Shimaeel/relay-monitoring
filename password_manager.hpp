// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file password_manager.hpp
 * @brief Password management for relay LEVEL1 / LEVEL2 password changes.
 *
 * @details Provides validation, ASN.1 BER encoding (Tag 34), and relay
 * communication for password change operations.  Designed to be called
 * from the WebSocket action handler so that incoming JSON requests are
 * processed without blocking the main I/O thread.
 *
 * ## Supported WebSocket Actions
 *
 * | Action            | Description                              |
 * |-------------------|------------------------------------------|
 * | change_password   | Change relay password (LEVEL1 / LEVEL2)  |
 *
 * ## Data Flow
 *
 * @code
 * Browser
 *   │  { "action": "change_password", "level": "LEVEL1", "value": "TAIL1" }
 *   ▼
 * WebSocket Session
 *   │
 *   ▼
 * PasswordManager::handleAction()
 *   │
 *   ├── validatePassword()    → check TAIL1 / OTTER1
 *   ├── encodeASN1Password()  → ASN.1 Tag=34 encoding
 *   ├── changePassword()      → RelayService → TelnetClient → Relay
 *   │
 *   ▼
 * JSON response → WebSocket → Browser
 * @endcode
 *
 * @see relay_service.hpp  Low-level relay I/O
 * @see ws_server.hpp      WebSocket session that calls this manager
 *
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

#include "dll_export.hpp"
#include "relay_service.hpp"
#include "asn_tlv_codec.hpp"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// ============================================================================
//  ASN.1 PASSWORD tag (context-specific, Tag 34 = 0x22)
// ============================================================================
static constexpr uint8_t ASN1_PASSWORD_TAG = 0x22;  // Tag 34

// ============================================================================
//  ASN.1 PASSWORD helpers (header-only, reuses asn_tlv utilities)
// ============================================================================
namespace asn_password
{

/**
 * @brief Encode a password string as ASN.1 BER TLV with Tag 34.
 *
 * @param password  Password string to encode
 * @return std::vector<uint8_t> BER-encoded TLV payload
 */
inline std::vector<uint8_t> encodeASN1Password(const std::string& password)
{
    std::vector<uint8_t> payload;
    asn_tlv::berAppendString(payload, ASN1_PASSWORD_TAG, password);
    return payload;
}

/**
 * @brief Decode an ASN.1 BER TLV payload with Tag 34 to a password string.
 *
 * @param data     Raw BER bytes
 * @param size     Number of bytes
 * @param[out] password  Decoded password string
 * @param[out] error     Optional error message on failure
 *
 * @return true on success
 */
inline bool decodeASN1Password(const uint8_t* data, std::size_t size,
                               std::string& password, std::string* error = nullptr)
{
    if (data == nullptr || size == 0U)
    {
        if (error) *error = "Empty PASSWORD buffer";
        return false;
    }

    asn_tlv::TlvInfo info;
    if (!asn_tlv::readTlv(data, size, 0U, info, error))
        return false;

    if (info.tag != ASN1_PASSWORD_TAG)
    {
        if (error)
            *error = "Unexpected tag " + std::to_string(info.tag)
                   + " (expected " + std::to_string(ASN1_PASSWORD_TAG) + ")";
        return false;
    }

    password.assign(reinterpret_cast<const char*>(data + info.valueStart), info.length);
    return true;
}

}  // namespace asn_password

// ============================================================================
//  PasswordManager — orchestrates password change operations
// ============================================================================

/**
 * @class PasswordManager
 * @brief Validates and executes relay password changes (LEVEL1 / LEVEL2).
 *
 * @details Provides a high-level interface for:
 * - Validating password values against the allowed set (TAIL1 / OTTER1)
 * - Building ASN.1 Tag 34 encoded payloads
 * - Sending password change commands to the relay
 * - Handling WebSocket JSON requests end-to-end
 *
 * Thread-safety: each public method acquires RelayService's internal mutex
 * through the RelayService calls, so this class itself is safe to call
 * from any thread (e.g. posted on a Boost.Asio strand).
 *
 * ## WebSocket Protocol
 *
 * Request:
 * @code{.json}
 * {
 *   "action": "change_password",
 *   "level": "LEVEL1",
 *   "value": "TAIL1"
 * }
 * @endcode
 *
 * Success Response:
 * @code{.json}
 * { "action": "change_password", "status": "success" }
 * @endcode
 *
 * Failure Response:
 * @code{.json}
 * { "action": "change_password", "status": "failed", "error": "..." }
 * @endcode
 */
class TELNET_SML_API PasswordManager
{
public:
    /**
     * @brief Result of a password change operation.
     */
    struct PasswordResult
    {
        bool        success = false;   ///< true when relay acknowledged the change
        std::string error;             ///< Error message on failure
    };

    /**
     * @brief Construct with a RelayService instance.
     *
     * @param relay  RelayService wrapping TelnetClient (must outlive this manager)
     */
    explicit PasswordManager(RelayService& relay)
        : relay_(relay)
    {
    }

    // ── Validation ──────────────────────────────────────────────────────

    /**
     * @brief Validate that a password value is in the allowed set.
     *
     * Only "TAIL1" and "OTTER1" are permitted.
     *
     * @param value  Password string to validate
     * @return true if value is allowed
     */
    static bool validatePassword(const std::string& value)
    {
        return (value == "TAIL1" || value == "OTTER1");
    }

    /**
     * @brief Validate that a level string is valid.
     *
     * @param level  Level string to validate
     * @return true if level is "LEVEL1" or "LEVEL2"
     */
    static bool validateLevel(const std::string& level)
    {
        return (level == "LEVEL1" || level == "LEVEL2");
    }

    // ── Password Change ─────────────────────────────────────────────────

    /**
     * @brief Change relay password for specified access level.
     *
     * @details Validates inputs, builds ASN.1 Tag 34 payload, sends
     * the password change command to the relay, and waits for ACK.
     * Plain password values are never written to logs.
     *
     * @param level  "LEVEL1" or "LEVEL2"
     * @param value  "TAIL1" or "OTTER1"
     * @return PasswordResult with success flag and optional error
     */
    PasswordResult changePassword(const std::string& level, const std::string& value)
    {
        PasswordResult result;

        // ── Input validation ────────────────────────────────────────────
        if (!validateLevel(level))
        {
            result.error = "Invalid level (must be LEVEL1 or LEVEL2)";
            return result;
        }

        if (!validatePassword(value))
        {
            result.error = "Invalid password value";
            return result;
        }

        // ── ASN.1 encoding (Tag 34) ────────────────────────────────────
        auto asnPayload = asn_password::encodeASN1Password(value);
        (void)asnPayload;  // Available for binary protocol extensions

        // ── Relay communication ─────────────────────────────────────────
        std::string cmd = "PAS " + level + " " + value;
        auto cmdResult = relay_.sendRelayCommand(cmd);

        if (cmdResult.success)
        {
            result.success = true;
            std::cout << "[PasswordMgr] Password change successful for "
                      << level << "\n";
        }
        else
        {
            result.error = cmdResult.error.empty()
                             ? "Password change failed - no ACK from relay"
                             : cmdResult.error;
            std::cout << "[PasswordMgr] Password change failed for "
                      << level << "\n";
        }

        return result;
    }

    // ── WebSocket Action Handler ────────────────────────────────────────

    /**
     * @brief Process a full JSON message for the "change_password" action.
     *
     * @details Extracts "level" and "value" from the JSON, validates,
     * executes the password change, and returns a JSON response.
     * Password values are never logged.
     *
     * @param jsonMsg  Full JSON message string from WebSocket
     * @return JSON response string to send back via WebSocket
     */
    std::string handleAction(const std::string& jsonMsg)
    {
        std::string level = extractJsonField(jsonMsg, "level");
        std::string value = extractJsonField(jsonMsg, "value");

        // Validate level
        if (!validateLevel(level))
        {
            return buildErrorJson("Invalid level: must be LEVEL1 or LEVEL2");
        }

        // Validate password value
        if (!validatePassword(value))
        {
            return buildErrorJson("Invalid password value");
        }

        // Execute password change
        auto result = changePassword(level, value);

        if (result.success)
        {
            return buildSuccessJson();
        }
        else
        {
            return buildErrorJson(result.error);
        }
    }

private:
    // ── JSON Helpers (lightweight, no library dependency) ───────────────

    /**
     * @brief Extract a string field value from a simple JSON object.
     *
     * @param json      Raw JSON string
     * @param fieldName Field name to extract (without quotes)
     * @return Extracted value string, or empty on failure
     */
    static std::string extractJsonField(const std::string& json, const std::string& fieldName)
    {
        const std::string key = "\"" + fieldName + "\"";
        auto pos = json.find(key);
        if (pos == std::string::npos) return {};
        pos = json.find(':', pos + key.size());
        if (pos == std::string::npos) return {};
        pos = json.find('"', pos + 1);
        if (pos == std::string::npos) return {};
        auto end = json.find('"', pos + 1);
        if (end == std::string::npos) return {};
        return json.substr(pos + 1, end - pos - 1);
    }

    static std::string escapeJson(const std::string& s)
    {
        std::string out;
        out.reserve(s.size());
        for (char c : s)
        {
            if (c == '"')       out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else                out += c;
        }
        return out;
    }

    static std::string buildSuccessJson()
    {
        return "{\"action\":\"change_password\",\"status\":\"success\"}";
    }

    static std::string buildErrorJson(const std::string& msg)
    {
        std::ostringstream json;
        json << "{"
             << "\"action\":\"change_password\","
             << "\"status\":\"failed\","
             << "\"error\":\"" << escapeJson(msg) << "\""
             << "}";
        return json.str();
    }

    RelayService& relay_;   ///< Thread-safe relay communication service
};
