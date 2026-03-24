// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file time_sync_manager.hpp
 * @brief DATE synchronization manager for relay ↔ local-PC time operations.
 *
 * @details Orchestrates reading relay time, comparing with local PC time,
 * and synchronizing the relay clock.  Designed to be called from the
 * WebSocket message handler so that incoming JSON actions are processed
 * without blocking the main I/O thread.
 *
 * ## Supported WebSocket Actions
 *
 * | Action            | Description                              |
 * |-------------------|------------------------------------------|
 * | read_time         | Read relay DATE + local PC time          |
 * | sync_time         | Write local PC time to relay             |
 * | sntp_sync         | Enable SNTP and set NTP server on relay  |

 *
 * ## Data Flow
 *
 * @code
 * Browser
 *   │  { "action": "read_time" }
 *   ▼
 * WebSocket Session
 *   │
 *   ▼
 * TimeSyncManager::handleAction()
 *   │
 *   ├── readRelayTime()   → RelayService → TelnetClient → Relay
 *   ├── getLocalPCTime()  → std::chrono
 *   ├── compareTime()     → abs diff in seconds
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
#include "client.hpp"

#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

/**
 * @class TimeSyncManager
 * @brief High-level orchestrator for DATE synchronization.
 *
 * Thread-safety: each public method acquires RelayService's internal mutex
 * through the RelayService calls, so this class itself is safe to call
 * from any thread (e.g. posted on a Boost.Asio strand).
 */
class TELNET_SML_API TimeSyncManager
{
public:
    /**
     * @brief Construct with a RelayService instance.
     *
     * @param relay  RelayService wrapping TelnetClient (must outlive this manager)
     */
    explicit TimeSyncManager(RelayService& relay)
        : relay_(relay)
    {
    }

    // ── Local PC Time ───────────────────────────────────────────────────

    /**
     * @brief Get current local PC time formatted as "YYYY-MM-DD HH:MM:SS".
     *
     * @return Formatted datetime string with second-level precision
     */
    static std::string getLocalPCTime()
    {
        auto now  = std::chrono::system_clock::now();
        auto tt   = std::chrono::system_clock::to_time_t(now);
        std::tm   local{};

#ifdef _WIN32
        localtime_s(&local, &tt);
#else
        localtime_r(&tt, &local);
#endif

        std::ostringstream oss;
        oss << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }

    // ── Compare ─────────────────────────────────────────────────────────

    /**
     * @brief Compare relay time and PC time.
     *
     * @param relayTime   Datetime string from relay
     * @param pcTime      Datetime string from local PC
     *
     * @return Pair of { status, diff_seconds }.
     *         status = "in_sync" if |diff| <= 1 s, otherwise "out_of_sync".
     */
    static std::pair<std::string, int64_t> compareTime(const std::string& relayTime,
                                                       const std::string& pcTime)
    {
        auto relayTp = parseToTimePoint(relayTime);
        auto pcTp    = parseToTimePoint(pcTime);

        int64_t diffSec = std::abs(
            std::chrono::duration_cast<std::chrono::seconds>(relayTp - pcTp).count());

        std::string status = (diffSec <= 1) ? "in_sync" : "out_of_sync";
        return {status, diffSec};
    }

    // ── WebSocket Action Handler ────────────────────────────────────────

    /**
     * @brief Process a JSON action string from WebSocket and return a JSON response.
     *
     * Supported actions:
     *   - "read_time"       → reads relay time, gets PC time, compares
     *   - "sync_time"       → writes PC time to relay
     *   - "sntp_sync"       → configure relay SNTP with given server
     *
     * @param action       The "action" field value from the incoming JSON
     * @param sntpServer   Optional SNTP server address (used by sntp_sync)
     * @param relayPassword Relay password for Level 2 elevation (from config)
     * @return JSON response string to send back via WebSocket
     */
    std::string handleAction(const std::string& action,
                             const std::string& sntpServer = "",
                             const std::string& relayPassword = "")
    {
        if (action == "read_time")
            return handleReadTime();
        if (action == "sync_time")
            return handleSyncTime();
        if (action == "sntp_sync")
            return handleSntpSync(sntpServer, relayPassword);

        return buildErrorJson(action, "Unknown action: " + action);
    }

private:
    // ── read_time handler ───────────────────────────────────────────────

    /**
     * @brief Handle the "read_time" action.
     * @details Reads relay time via RelayService, gets local PC time,
     *          compares both, and returns a JSON response.
     * @return JSON string with relay_time, pc_time, sync_status, diff_seconds.
     */
    std::string handleReadTime()
    {
        auto relayResult = relay_.readRelayTime();
        std::string pcTime = getLocalPCTime();

        if (!relayResult.success)
        {
            // Return what we can (PC time) plus the error
            std::ostringstream json;
            json << "{"
                 << "\"action\":\"read_time\","
                 << "\"status\":\"error\","
                 << "\"pc_time\":\"" << escapeJson(pcTime) << "\","
                 << "\"error\":\"" << escapeJson(relayResult.error) << "\""
                 << "}";
            return json.str();
        }

        auto [syncStatus, diffSec] = compareTime(relayResult.datetime, pcTime);

        std::ostringstream json;
        json << "{"
             << "\"action\":\"read_time\","
             << "\"status\":\"success\","
             << "\"relay_time\":\"" << escapeJson(relayResult.datetime) << "\","
             << "\"pc_time\":\"" << escapeJson(pcTime) << "\","
             << "\"sync_status\":\"" << syncStatus << "\","
             << "\"diff_seconds\":" << diffSec
             << "}";
        return json.str();
    }

    // ── sync_time handler ───────────────────────────────────────────────

    /**
     * @brief Handle the "sync_time" action.
     * @details Writes the current local PC time to the relay via SETTIME.
     * @return JSON string with status and new_time or error.
     */
    std::string handleSyncTime()
    {
        std::string pcTime = getLocalPCTime();
        auto result = relay_.syncRelayTime(pcTime);

        if (result.success)
        {
            std::ostringstream json;
            json << "{"
                 << "\"action\":\"sync_time\","
                 << "\"status\":\"success\","
                 << "\"new_time\":\"" << escapeJson(result.datetime) << "\""
                 << "}";
            return json.str();
        }
        else
        {
            return buildErrorJson("sync_time", result.error);
        }
    }

    // ── sntp_sync handler ────────────────────────────────────────────────

    /**
     * @brief Handle the "sntp_sync" action.
     * @details Sends SNTP configuration commands to the relay:
     *          1. SET E_SNTP Y   — enable SNTP on the relay
     *          2. SET SNTP_SERV1 <server> — set primary SNTP server address
     * @param sntpServer  NTP server hostname or IP (e.g. "pool.ntp.org")
     * @return JSON string with status and detail or error.
     */
    std::string handleSntpSync(const std::string& sntpServer,
                               const std::string& relayPassword)
    {
        if (sntpServer.empty())
            return buildErrorJson("sntp_sync", "SNTP server address is required");

        if (relayPassword.empty())
            return buildErrorJson("sntp_sync", "Relay password not available in config");

        // Read device time BEFORE sync
        auto beforeResult = relay_.readRelayTime();
        std::string oldTime = beforeResult.success ? beforeResult.datetime : "unknown";

        // Step 1: Elevate to Level 2 (SET commands require 2AC access)
        auto loginResult = relay_.elevateToLevel2(relayPassword);
        if (!loginResult.success)
        {
            std::ostringstream json;
            json << "{"
                 << "\"action\":\"sntp_sync\","
                 << "\"status\":\"error\","
                 << "\"step\":\"level2_access\","
                 << "\"old_time\":\"" << escapeJson(oldTime) << "\","
                 << "\"error\":\"" << escapeJson(loginResult.error) << "\""
                 << "}";
            return json.str();
        }

        // Step 2: Enable SNTP on the relay
        auto enableResult = relay_.sendRelayCommandStrict("SET E_SNTP Y");
        if (!enableResult.success)
        {
            relay_.sendRelayCommand("ACC");
            std::ostringstream json;
            json << "{"
                 << "\"action\":\"sntp_sync\","
                 << "\"status\":\"error\","
                 << "\"step\":\"enable\","
                 << "\"old_time\":\"" << escapeJson(oldTime) << "\","
                 << "\"error\":\"" << escapeJson(enableResult.error) << "\""
                 << "}";
            return json.str();
        }

        // Step 3: Set the SNTP server address
        auto serverResult = relay_.sendRelayCommandStrict("SET SNTP_SERV1 " + sntpServer);
        if (!serverResult.success)
        {
            relay_.sendRelayCommand("ACC");
            std::ostringstream json;
            json << "{"
                 << "\"action\":\"sntp_sync\","
                 << "\"status\":\"error\","
                 << "\"step\":\"set_server\","
                 << "\"old_time\":\"" << escapeJson(oldTime) << "\","
                 << "\"error\":\"" << escapeJson(serverResult.error) << "\""
                 << "}";
            return json.str();
        }

        // Step 4: Drop back to Level 1
        relay_.sendRelayCommand("ACC");

        // Read device time AFTER sync
        auto afterResult = relay_.readRelayTime();
        std::string newTime = afterResult.success ? afterResult.datetime : "pending";

        // Success
        std::ostringstream json;
        json << "{"
             << "\"action\":\"sntp_sync\","
             << "\"status\":\"success\","
             << "\"sntp_server\":\"" << escapeJson(sntpServer) << "\","
             << "\"old_time\":\"" << escapeJson(oldTime) << "\","
             << "\"new_time\":\"" << escapeJson(newTime) << "\""
             << "}";
        return json.str();
    }

    // ── Helpers ─────────────────────────────────────────────────────────

    /**
     * @brief Parse "YYYY-MM-DD HH:MM:SS" to system_clock::time_point.
     */
    static std::chrono::system_clock::time_point parseToTimePoint(const std::string& dt)
    {
        std::tm tm{};
        std::istringstream iss(dt);
        iss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        if (iss.fail())
        {
            // Try alternate format DD/MM/YY HH:MM:SS (relay style)
            std::istringstream iss2(dt);
            iss2 >> std::get_time(&tm, "%m/%d/%y %H:%M:%S");
            if (iss2.fail())
            {
                return std::chrono::system_clock::now();  // fallback
            }
        }
        std::time_t t = std::mktime(&tm);
        return std::chrono::system_clock::from_time_t(t);
    }

    /**
     * @brief Escape special characters for JSON string values.
     * @param s Raw string.
     * @return JSON-safe escaped string.
     */
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

    /**
     * @brief Build a JSON error response.
     * @param action The action name (e.g. "read_time", "sync_time").
     * @param msg    Human-readable error description.
     * @return JSON string with action, status, and error fields.
     */
    static std::string buildErrorJson(const std::string& action, const std::string& msg)
    {
        std::ostringstream json;
        json << "{"
             << "\"action\":\"" << action << "\","
             << "\"status\":\"failed\","
             << "\"error\":\"" << escapeJson(msg) << "\""
             << "}";
        return json.str();
    }

    RelayService& relay_;
};
