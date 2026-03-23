// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file sntp_client.hpp
 * @brief Lightweight SNTP client using Boost.Asio UDP (RFC 4330).
 *
 * @details Queries an NTP server for the current UTC time, then converts
 * to local time and returns it as "YYYY-MM-DD HH:MM:SS".  Uses a single
 * synchronous UDP send/receive with a configurable timeout.
 *
 * No external NTP library is required — the 48-byte NTP packet is
 * assembled and parsed inline.
 *
 * @see time_sync_manager.hpp  Orchestrator that calls this client
 */

#pragma once

#include "dll_export.hpp"

#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

/**
 * @class SNTPClient
 * @brief Fetch current time from an NTP server via a single UDP round-trip.
 *
 * Usage:
 * @code
 *   SNTPClient ntp;
 *   auto [ok, datetime, err] = ntp.query("pool.ntp.org", 5000);
 *   // datetime == "2026-03-23 14:05:22" (local time)
 * @endcode
 */
class TELNET_SML_API SNTPClient
{
public:
    struct Result
    {
        bool        success = false;
        std::string datetime;          ///< "YYYY-MM-DD HH:MM:SS" in local time
        std::string error;
    };

    /**
     * @brief Query an NTP server and return local time.
     *
     * @param server      Hostname or IP (e.g. "pool.ntp.org", "time.nist.gov")
     * @param timeoutMs   UDP round-trip timeout in milliseconds
     * @return Result with success flag, local datetime, and error string
     */
    Result query(const std::string& server, int timeoutMs = 5000) const
    {
        Result result;
        try
        {
            namespace asio = boost::asio;
            using udp = asio::ip::udp;

            asio::io_context io;
            udp::resolver resolver(io);
            udp::resolver::results_type endpoints = resolver.resolve(server, "123");

            udp::socket socket(io, udp::v4());
            socket.connect(*endpoints.begin());

            // ── Build NTP request packet (48 bytes) ─────────────────────
            // LI=0, VN=3, Mode=3 (client) → byte 0 = 0x1B
            uint8_t packet[48] = {};
            packet[0] = 0x1B;

            socket.send(asio::buffer(packet, 48));

            // ── Receive with timeout ────────────────────────────────────
            socket.non_blocking(true);
            auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeoutMs);

            size_t received = 0;
            boost::system::error_code ec;

            while (std::chrono::steady_clock::now() < deadline)
            {
                received = socket.receive(asio::buffer(packet, 48), 0, ec);
                if (ec == boost::asio::error::would_block)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                break;
            }

            socket.close();

            if (ec && ec != boost::asio::error::would_block)
            {
                result.error = "UDP receive error: " + ec.message();
                return result;
            }
            if (received < 48)
            {
                result.error = "Incomplete NTP response (" + std::to_string(received) + " bytes)";
                return result;
            }

            // ── Parse transmit timestamp (bytes 40-43 = seconds since 1900-01-01) ──
            uint32_t ntpSec = (static_cast<uint32_t>(packet[40]) << 24)
                            | (static_cast<uint32_t>(packet[41]) << 16)
                            | (static_cast<uint32_t>(packet[42]) <<  8)
                            | (static_cast<uint32_t>(packet[43]));

            // NTP epoch = 1900-01-01, Unix epoch = 1970-01-01
            // Difference = 70 years = 2208988800 seconds
            constexpr uint32_t NTP_UNIX_DELTA = 2208988800U;
            std::time_t unixTime = static_cast<std::time_t>(ntpSec - NTP_UNIX_DELTA);

            // Convert to local time
            std::tm local{};
#ifdef _WIN32
            localtime_s(&local, &unixTime);
#else
            localtime_r(&unixTime, &local);
#endif

            std::ostringstream oss;
            oss << std::put_time(&local, "%Y-%m-%d %H:%M:%S");

            result.success  = true;
            result.datetime = oss.str();
        }
        catch (const std::exception& e)
        {
            result.error = std::string("SNTP query failed: ") + e.what();
        }
        return result;
    }
};
