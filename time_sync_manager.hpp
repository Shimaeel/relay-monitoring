// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file time_sync_manager.hpp
 * @brief DATE synchronization manager for relay ↔ local-PC time operations.
 *
 * @details Orchestrates reading relay time, comparing with local PC time,
 * and synchronizing the relay clock.  All relay commands are routed through
 * a CommandSender callback (typically pipeline->handleUserCommand) to avoid
 * race conditions with the pipeline's background worker thread.
 *
 * ## Supported WebSocket Actions
 *
 * | Action            | Description                              |
 * |-------------------|------------------------------------------|
 * | read_time         | Read relay DATE + local PC time          |
 * | sync_time         | Write local PC time to relay             |
 * | sntp_sync         | Enable SNTP and set NTP server on relay  |
 *
 * @see relay_pipeline.hpp  Pipeline that provides the CommandSender
 * @see ws_server.hpp       WebSocket session that calls this manager
 *
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

#include "dll_export.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <ctime>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

/**
 * @class TimeSyncManager
 * @brief High-level orchestrator for DATE synchronization.
 *
 * All relay I/O is routed through a CommandSender callback — typically
 * bound to pipeline->handleUserCommand() — so that commands are
 * serialised with the pipeline's background worker and never race.
 */
class TELNET_SML_API TimeSyncManager
{
public:
    /// Callback type: sends a single command, returns the raw response.
    using CommandSender = std::function<std::string(const std::string&)>;

    /// Callback type: sends multiple commands atomically, returns responses.
    using BatchSender = std::function<std::vector<std::string>(const std::vector<std::string>&)>;

    /**
     * @brief Construct with a command-sender and optional batch-sender.
     *
     * @param sender  Callable for single commands (thread-safe via pipeline)
     * @param batch   Callable for atomic batch commands (holds lock for entire sequence)
     */
    TimeSyncManager(CommandSender sender, BatchSender batch = nullptr)
        : send_(std::move(sender))
        , sendBatch_(std::move(batch))
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

    std::string handleReadTime()
    {
        std::string pcTime = getLocalPCTime();
        std::string rawResponse = send_("DATE");

        if (rawResponse.empty())
        {
            std::ostringstream json;
            json << "{"
                 << "\"action\":\"read_time\","
                 << "\"status\":\"error\","
                 << "\"pc_time\":\"" << escapeJson(pcTime) << "\","
                 << "\"error\":\"DATE command failed\""
                 << "}";
            return json.str();
        }

        std::string relayTime = cleanRelayText(rawResponse);
        if (relayTime.empty())
        {
            std::ostringstream json;
            json << "{"
                 << "\"action\":\"read_time\","
                 << "\"status\":\"error\","
                 << "\"pc_time\":\"" << escapeJson(pcTime) << "\","
                 << "\"error\":\"Cannot parse DATE response\""
                 << "}";
            return json.str();
        }

        auto [syncStatus, diffSec] = compareTime(relayTime, pcTime);

        std::ostringstream json;
        json << "{"
             << "\"action\":\"read_time\","
             << "\"status\":\"success\","
             << "\"relay_time\":\"" << escapeJson(relayTime) << "\","
             << "\"pc_time\":\"" << escapeJson(pcTime) << "\","
             << "\"sync_status\":\"" << syncStatus << "\","
             << "\"diff_seconds\":" << diffSec
             << "}";
        return json.str();
    }

    // ── sync_time handler ───────────────────────────────────────────────

    std::string handleSyncTime()
    {
        std::string pcTime = getLocalPCTime();
        std::string resp = send_("SETTIME " + pcTime);

        if (resp.empty())
            return buildErrorJson("sync_time", "SETTIME command failed");

        std::string upper = resp;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

        if (upper.find("ERR") != std::string::npos
            || upper.find("DENIED") != std::string::npos)
        {
            return buildErrorJson("sync_time", "Relay rejected SETTIME: " + resp.substr(0, 120));
        }

        std::ostringstream json;
        json << "{"
             << "\"action\":\"sync_time\","
             << "\"status\":\"success\","
             << "\"new_time\":\"" << escapeJson(pcTime) << "\""
             << "}";
        return json.str();
    }

    // ── sntp_sync handler ────────────────────────────────────────────────

    std::string handleSntpSync(const std::string& sntpServer,
                               const std::string& relayPassword)
    {
        if (sntpServer.empty())
            return buildErrorJson("sntp_sync", "SNTP server address is required");

        if (relayPassword.empty())
            return buildErrorJson("sntp_sync", "Relay password not available in config");

        // Build the full command sequence
        std::vector<std::string> cmds = {
            "DATE",                              // 0: read old time
            "2AC",                               // 1: request Level 2
            relayPassword,                       // 2: send password
            "SET E_SNTP Y",                      // 3: enable SNTP
            "SET SNTP_SERV1 " + sntpServer,      // 4: set NTP server
            "ACC",                               // 5: drop to Level 1
            "DATE"                               // 6: read new time
        };

        // Execute atomically (single lock, no interleaving)
        std::vector<std::string> results;
        if (sendBatch_)
        {
            results = sendBatch_(cmds);
        }
        else
        {
            // Fallback: send one by one (less safe but still works)
            for (const auto& cmd : cmds)
                results.push_back(send_(cmd));
        }

        // Parse results
        std::string oldTime = (results.size() > 0) ? cleanRelayText(results[0]) : "";
        if (oldTime.empty()) oldTime = "unknown";

        // Check Level 2 access (step 1+2)
        if (results.size() < 3 || results[2].empty() || hasError(results[2]))
            return buildSntpError("level2_access", "Level 2 access failed", oldTime);

        // Check SNTP enable (step 3)
        if (results.size() < 4 || results[3].empty() || hasError(results[3]))
            return buildSntpError("enable", "SET E_SNTP Y failed", oldTime);

        // Check server set (step 4)
        if (results.size() < 5 || results[4].empty() || hasError(results[4]))
            return buildSntpError("set_server", "SET SNTP_SERV1 failed", oldTime);

        // Parse new time (step 6)
        std::string newTime = (results.size() > 6) ? cleanRelayText(results[6]) : "";
        if (newTime.empty()) newTime = "pending";

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

    // ── Internal Helpers ────────────────────────────────────────────────

    /// Read relay time using the DATE command through the pipeline.
    std::string readRelayTimeViaCmd()
    {
        std::string resp = send_("DATE");
        if (resp.empty()) return "unknown";
        std::string t = cleanRelayText(resp);
        return t.empty() ? "unknown" : t;
    }

    /// Check if a raw relay response contains error indicators.
    static bool hasError(const std::string& resp)
    {
        std::string upper = resp;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        return upper.find("ERR") != std::string::npos
            || upper.find("ACCESS") != std::string::npos
            || upper.find("DENIED") != std::string::npos
            || upper.find("INVALID") != std::string::npos;
    }

    /// Strip Telnet echo/prompts from raw DATE response, return datetime string.
    static std::string cleanRelayText(const std::string& raw)
    {
        std::istringstream iss(raw);
        std::string line;
        while (std::getline(iss, line))
        {
            auto start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            auto end = line.find_last_not_of(" \t\r\n");
            std::string trimmed = line.substr(start, end - start + 1);

            if (trimmed.find("DATE") == 0 || trimmed.find("date") == 0)
                continue;
            if (trimmed.find("=>") != std::string::npos && trimmed.size() < 5)
                continue;

            bool hasDigit = false, hasSep = false;
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

    /// Build a JSON error for sntp_sync with step and old_time.
    std::string buildSntpError(const std::string& step,
                               const std::string& error,
                               const std::string& oldTime)
    {
        std::ostringstream json;
        json << "{"
             << "\"action\":\"sntp_sync\","
             << "\"status\":\"error\","
             << "\"step\":\"" << step << "\","
             << "\"old_time\":\"" << escapeJson(oldTime) << "\","
             << "\"error\":\"" << escapeJson(error) << "\""
             << "}";
        return json.str();
    }

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

    CommandSender send_;
    BatchSender sendBatch_;
};
