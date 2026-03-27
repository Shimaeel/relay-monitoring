// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file sntp_client.hpp
 * @brief SNTP (Simple Network Time Protocol) client using Boost.Asio.
 *
 * @details Implements an SNTPv4 client (RFC 4330) that queries a public
 * NTP server over UDP port 123 and returns the current UTC time.
 * Used to provide an authoritative time source for relay time
 * synchronisation in the web UI.
 *
 * ## Protocol Overview
 *
 * An SNTP request is a 48-byte UDP packet.  The client sets:
 *   - Byte 0 = 0x1B  (LI=0, VN=3, Mode=3 — client request)
 *   - Bytes 1-47 = 0
 *
 * The server replies with a 48-byte packet.  The *Transmit Timestamp*
 * at bytes 40-47 gives seconds (big-endian uint32 at [40]) and
 * fractional seconds (uint32 at [44]) since 1 January 1900 00:00 UTC.
 *
 * @note This is a *simple* client — no drift compensation, no
 *       symmetric/broadcast modes, no authentication.
 *
 * @see telnet_sml_app.cpp  Integration point (action handler)
 *
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

#include <boost/asio.hpp>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

/**
 * @class SntpClient
 * @brief Queries an NTP server via UDP and returns the current UTC time.
 *
 * @details Thread-safe: each call to queryTime() creates its own
 * io_context / socket, so multiple threads can call it concurrently.
 *
 * ## Usage
 * @code
 * SntpClient sntp;                             // uses pool.ntp.org
 * auto result = sntp.queryTime();
 * if (result.success)
 *     std::cout << "UTC: " << result.iso8601 << "\n";
 * @endcode
 */
class SntpClient
{
public:
    /// Result of an SNTP query.
    struct TimeResult
    {
        bool        success = false;    ///< true if query succeeded
        std::string iso8601;            ///< UTC time in ISO 8601 format
        std::string dateTime;           ///< UTC time as "MM/DD/YY HH:MM:SS"
        std::string error;              ///< Error description on failure
        int64_t     epochSeconds = 0;   ///< Unix epoch seconds (UTC)
        int         milliseconds = 0;   ///< Fractional milliseconds
    };

    /**
     * @brief Construct an SNTP client targeting a given NTP server.
     * @param server  NTP server hostname (default: "pool.ntp.org")
     * @param timeoutMs  UDP round-trip timeout in milliseconds (default 3000)
     */
    explicit SntpClient(const std::string& server = "pool.ntp.org",
                        int timeoutMs = 3000)
        : server_(server)
        , timeoutMs_(timeoutMs)
    {
    }

    /**
     * @brief Query the NTP server and return the current UTC time.
     *
     * @details Creates a transient UDP socket, sends a 48-byte SNTP
     * request, waits for the response (with timeout), and parses the
     * Transmit Timestamp field.
     *
     * @return TimeResult with UTC time on success, or error string on failure.
     */
    TimeResult queryTime() const
    {
        TimeResult result;

        try
        {
            namespace asio = boost::asio;
            using udp = asio::ip::udp;

            asio::io_context ioc;

            // Resolve the NTP server
            udp::resolver resolver(ioc);
            auto endpoints = resolver.resolve(udp::v4(), server_, "123");

            udp::socket socket(ioc, udp::v4());

            // Build SNTP request (48 bytes, mode 3 = client)
            std::array<uint8_t, 48> request{};
            request[0] = 0x1B;  // LI=0, VN=3, Mode=3

            // Send to first resolved endpoint
            udp::endpoint serverEp = *endpoints.begin();
            socket.send_to(asio::buffer(request), serverEp);

            // Receive with timeout
            std::array<uint8_t, 48> response{};
            udp::endpoint senderEp;
            std::size_t bytesReceived = 0;
            bool received = false;

            // Set up async receive with a deadline
            socket.non_blocking(true);

            auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeoutMs_);

            while (std::chrono::steady_clock::now() < deadline)
            {
                boost::system::error_code ec;
                bytesReceived = socket.receive_from(
                    asio::buffer(response), senderEp, 0, ec);

                if (!ec && bytesReceived >= 48)
                {
                    received = true;
                    break;
                }

                if (ec != boost::asio::error::would_block)
                {
                    // Real error
                    result.error = "UDP receive error: " + ec.message();
                    return result;
                }

                // Brief sleep before retry to avoid busy-spin
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            if (!received)
            {
                result.error = "SNTP timeout (no response within "
                             + std::to_string(timeoutMs_) + " ms)";
                return result;
            }

            // Parse Transmit Timestamp (bytes 40-47)
            // Seconds since 1 January 1900 (big-endian)
            uint32_t ntpSeconds = (static_cast<uint32_t>(response[40]) << 24)
                                | (static_cast<uint32_t>(response[41]) << 16)
                                | (static_cast<uint32_t>(response[42]) <<  8)
                                | (static_cast<uint32_t>(response[43]));

            uint32_t ntpFrac    = (static_cast<uint32_t>(response[44]) << 24)
                                | (static_cast<uint32_t>(response[45]) << 16)
                                | (static_cast<uint32_t>(response[46]) <<  8)
                                | (static_cast<uint32_t>(response[47]));

            // NTP epoch is 1 Jan 1900; Unix epoch is 1 Jan 1970.
            // Difference = 70 years worth of seconds (including leap years).
            constexpr uint32_t NTP_UNIX_DELTA = 2208988800U;

            if (ntpSeconds < NTP_UNIX_DELTA)
            {
                result.error = "Invalid NTP timestamp (before Unix epoch)";
                return result;
            }

            int64_t unixSeconds = static_cast<int64_t>(ntpSeconds) - NTP_UNIX_DELTA;
            int millis = static_cast<int>((static_cast<uint64_t>(ntpFrac) * 1000) >> 32);

            result.success      = true;
            result.epochSeconds = unixSeconds;
            result.milliseconds = millis;
            result.iso8601      = formatISO8601(unixSeconds, millis);
            result.dateTime     = formatRelayDateTime(unixSeconds, millis);
        }
        catch (const std::exception& ex)
        {
            result.error = std::string("SNTP exception: ") + ex.what();
        }

        return result;
    }

    /**
     * @brief Get the current PC (local) time formatted for relay comparison.
     * @return TimeResult with local PC time.
     */
    static TimeResult getPcTime()
    {
        TimeResult result;
        auto now   = std::chrono::system_clock::now();
        auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                         now.time_since_epoch()).count();
        auto ms    = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now.time_since_epoch()).count() % 1000;

        result.success      = true;
        result.epochSeconds = epoch;
        result.milliseconds = static_cast<int>(ms);
        result.iso8601      = formatISO8601(epoch, static_cast<int>(ms));
        result.dateTime     = formatRelayDateTime(epoch, static_cast<int>(ms));
        return result;
    }

private:
    std::string server_;
    int         timeoutMs_;

    /**
     * @brief Format Unix timestamp as ISO 8601 string.
     * @param epoch  Seconds since 1970-01-01 00:00:00 UTC
     * @param millis Fractional milliseconds
     * @return e.g. "2026-03-27T14:05:30.123Z"
     */
    static std::string formatISO8601(int64_t epoch, int millis)
    {
        std::time_t t = static_cast<std::time_t>(epoch);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &t);
#else
        gmtime_r(&t, &utc);
#endif
        std::ostringstream oss;
        oss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3) << millis << 'Z';
        return oss.str();
    }

    /**
     * @brief Format Unix timestamp as relay-style "MM/DD/YY HH:MM:SS.mmm".
     * @param epoch  Seconds since 1970-01-01 00:00:00 UTC
     * @param millis Fractional milliseconds
     * @return e.g. "03/27/26 14:05:30.123"
     */
    static std::string formatRelayDateTime(int64_t epoch, int millis)
    {
        std::time_t t = static_cast<std::time_t>(epoch);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &t);
#else
        gmtime_r(&t, &utc);
#endif
        std::ostringstream oss;
        oss << std::put_time(&utc, "%m/%d/%y %H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3) << millis;
        return oss.str();
    }
};
