// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file relay_service.hpp
 * @brief Thread-safe relay communication service for DATE operations.
 *
 * @details Provides ASN.1 BER encoding/decoding for DATE (tag 33) and
 * a thin service layer around TelnetClient for DATE read/write.
 *
 * ## Data Flow
 * @code
 * Browser (ui.html)
 *       │  { "action": "read_time" }
 *       │  { "action": "sync_time" }
 *       ▼
 * WebSocket Server
 *       │
 *       ▼
 * TimeSyncManager  ──►  RelayService  ──►  TelnetClient  ──►  Relay
 *       │                                                         │
 *       ◄─────────────── ASN.1 Tag=33 response ──────────────────┘
 *       │
 *       ▼
 * JSON WebSocket response to Browser
 * @endcode
 *
 * @see time_sync_manager.hpp  High-level sync orchestration
 * @see asn_tlv_codec.hpp      Shared BER utilities
 *
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

#include "dll_export.hpp"
#include "asn_tlv_codec.hpp"
#include "client.hpp"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

// ============================================================================
//  ASN.1 DATE tag (context-specific)
// ============================================================================
static constexpr uint8_t ASN1_DATE_TAG = 0x21;  // Tag 33 = 0x21

// ============================================================================
//  ASN.1 DATE helpers (header-only, reuses asn_tlv utilities)
// ============================================================================
namespace asn_date
{

/**
 * @brief Encode a datetime string as ASN.1 BER TLV with Tag 33.
 *
 * @param datetime Formatted string: "YYYY-MM-DD HH:MM:SS"
 * @return std::vector<uint8_t> BER-encoded TLV payload
 */
inline std::vector<uint8_t> encodeASN1Date(const std::string& datetime)
{
    std::vector<uint8_t> payload;
    asn_tlv::berAppendString(payload, ASN1_DATE_TAG, datetime);
    return payload;
}

/**
 * @brief Decode an ASN.1 BER TLV payload with Tag 33 to a datetime string.
 *
 * @param data     Raw BER bytes
 * @param size     Number of bytes
 * @param[out] datetime  Decoded datetime string
 * @param[out] error     Optional error message on failure
 *
 * @return true on success
 */
inline bool decodeASN1Date(const uint8_t* data, std::size_t size,
                           std::string& datetime, std::string* error = nullptr)
{
    if (data == nullptr || size == 0U)
    {
        if (error) *error = "Empty DATE buffer";
        return false;
    }

    asn_tlv::TlvInfo info;
    if (!asn_tlv::readTlv(data, size, 0U, info, error))
        return false;

    if (info.tag != ASN1_DATE_TAG)
    {
        if (error)
            *error = "Unexpected tag " + std::to_string(info.tag)
                   + " (expected " + std::to_string(ASN1_DATE_TAG) + ")";
        return false;
    }

    datetime.assign(reinterpret_cast<const char*>(data + info.valueStart), info.length);
    return true;
}

}  // namespace asn_date

// ============================================================================
//  RelayService — thread-safe DATE read / write via TelnetClient
// ============================================================================

/**
 * @class RelayService
 * @brief Thread-safe wrapper around TelnetClient for DATE operations.
 *
 * @details All public methods lock the internal mutex so that the
 * ReceptionWorker's command queue and the TimeSyncManager can share
 * the same underlying TelnetClient safely (only one caller at a time
 * sends Telnet I/O).
 *
 * ## Usage (inside ReceptionWorker or ProcessingWorker)
 * @code
 * RelayService relay(client);
 * auto [ok, dt] = relay.readRelayTime();
 * if (ok)
 *     std::cout << "Relay time: " << dt << "\n";
 * @endcode
 */
class TELNET_SML_API RelayService
{
public:
    /**
     * @brief Result of a relay DATE operation.
     */
    struct DateResult
    {
        bool     success = false;   ///< true when relay responded with valid data
        std::string datetime;       ///< Decoded datetime string (YYYY-MM-DD HH:MM:SS)
        std::string error;          ///< Error message on failure
    };

    /**
     * @brief Construct with external TelnetClient reference.
     *
     * The caller retains ownership; the client must outlive the service.
     */
    explicit RelayService(TelnetClient& client)
        : client_(client)
    {
    }

    // ── DATE Read ───────────────────────────────────────────────────────

    /**
     * @brief Send DATE read command and decode ASN.1 response.
     *
     * @return DateResult with success flag and decoded datetime
     */
    DateResult readRelayTime()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        DateResult result;

        if (!client_.isConnected())
        {
            result.error = "Relay not connected";
            return result;
        }

        std::string rawResponse;
        bool ok = client_.SendCmdReceiveData("DATE", rawResponse);
        if (!ok || rawResponse.empty())
        {
            result.error = "DATE command failed or empty response";
            return result;
        }

        // The relay may respond with plain text datetime "YYYY-MM-DD HH:MM:SS"
        // or with ASN.1 TLV.  Try ASN.1 first, then fall back to plain text.
        const auto* bytes = reinterpret_cast<const uint8_t*>(rawResponse.data());
        std::string decoded;
        std::string decodeErr;

        if (asn_date::decodeASN1Date(bytes, rawResponse.size(), decoded, &decodeErr))
        {
            result.success  = true;
            result.datetime = decoded;
        }
        else
        {
            // Fallback: treat the cleaned text as a datetime string
            std::string cleaned = cleanRelayText(rawResponse);
            if (!cleaned.empty())
            {
                result.success  = true;
                result.datetime = cleaned;
            }
            else
            {
                result.error = "Cannot parse DATE response: " + decodeErr;
            }
        }

        return result;
    }

    // ── DATE Write (SETTIME) ────────────────────────────────────────────

    /**
     * @brief Send SETTIME command with the given datetime to the relay.
     *
     * @param datetime  Formatted string: "YYYY-MM-DD HH:MM:SS"
     * @return DateResult with success/failure and any error message
     */
    DateResult syncRelayTime(const std::string& datetime)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        DateResult result;

        if (!client_.isConnected())
        {
            result.error = "Relay not connected";
            return result;
        }

        // Build ASN.1 encoded payload
        auto payload = asn_date::encodeASN1Date(datetime);

        // Send SETTIME command with the datetime string
        // Relay expects: SETTIME YYYY-MM-DD HH:MM:SS
        std::string cmd = "SETTIME " + datetime;
        std::string rawResponse;
        bool ok = client_.SendCmdReceiveData(cmd, rawResponse);

        if (!ok)
        {
            result.error = "SETTIME command failed";
            return result;
        }

        // Check for ACK in response
        std::string upper = rawResponse;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

        if (upper.find("ACK") != std::string::npos
            || upper.find("OK") != std::string::npos
            || upper.find("=>") != std::string::npos)
        {
            result.success  = true;
            result.datetime = datetime;
        }
        else
        {
            result.error = "No ACK received from relay: " + rawResponse.substr(0, 120);
        }

        return result;
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
    /**
     * @brief Strip Telnet echo/prompts from raw response, return trimmed datetime.
     */
    static std::string cleanRelayText(const std::string& raw)
    {
        // Look for a line containing a date-like pattern YYYY-MM-DD or DD/MM/YY
        std::istringstream iss(raw);
        std::string line;
        while (std::getline(iss, line))
        {
            // Trim
            auto start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            auto end = line.find_last_not_of(" \t\r\n");
            std::string trimmed = line.substr(start, end - start + 1);

            // Skip echo of the command itself
            if (trimmed.find("DATE") == 0 || trimmed.find("date") == 0)
                continue;
            // Skip prompts
            if (trimmed.find("=>") != std::string::npos && trimmed.size() < 5)
                continue;

            // Accept if it contains digits and separators
            bool hasDigit = false;
            bool hasSep   = false;
            for (char c : trimmed)
            {
                if (std::isdigit(static_cast<unsigned char>(c))) hasDigit = true;
                if (c == '-' || c == '/' || c == ':') hasSep = true;
            }
            if (hasDigit && hasSep)
                return trimmed;
        }
        return {};
    }

    TelnetClient& client_;
    std::mutex mutex_;
};
