// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file relay_config.hpp
 * @brief Relay device configuration registry.
 *
 * @details Defines the static configuration for all known relay devices
 * in the substation monitoring system. Each relay has a unique ID, network
 * address, credentials, and display metadata.
 *
 * ## Adding a New Relay
 *
 * To add a new relay, append a RelayConfig entry to the vector returned
 * by getRelayConfigs(). Each relay must have a unique `id`.
 *
 * ## Data Flow
 *
 * @code
 * relay_config.hpp
 *       │
 *       ▼
 * RelayManager::startRelay(id)
 *       │
 *       ▼
 * RelayPipeline (created with matching config)
 * @endcode
 *
 * @see relay_manager.hpp  Pipeline coordinator
 * @see relay_pipeline.hpp Per-relay pipeline bundle
 *
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

#include <chrono>
#include <string>
#include <vector>

/**
 * @struct RelayConfig
 * @brief Configuration for a single relay device.
 *
 * @details Contains all parameters needed to connect to, authenticate with,
 * and identify a relay device. Used by RelayManager to create on-demand
 * RelayPipeline instances.
 *
 * @invariant `id` must be unique across all relay configurations.
 */
struct RelayConfig
{
    std::string id;            ///< Unique relay identifier (e.g., "1", "2", "3")
    std::string name;          ///< Display name (e.g., "SEL-751", "SEL-421")
    std::string host;          ///< IP address (e.g., "192.168.0.2")
    int         port = 23;     ///< Telnet port (default: 23)
    std::string username;      ///< Level 1 login username
    std::string password;      ///< Level 1 login password
    std::string substation;    ///< Substation name for UI display
    std::string bay;           ///< Bay identifier for UI display

    /// Connection timeout (default: 2000 ms)
    std::chrono::milliseconds timeout{2000};
};

/**
 * @brief Get all known relay configurations.
 *
 * @details Returns the static list of relay devices. In production this
 * could be loaded from a JSON file or database; for now it is hardcoded.
 *
 * @return std::vector<RelayConfig> List of all relay configurations.
 *
 * @note To add a relay, append a new entry to the returned vector.
 */
inline std::vector<RelayConfig> getRelayConfigs()
{
    return {
        {
            "1",                    // id
            "SEL-751",              // name
            "192.168.0.2",          // host
            23,                     // port
            "acc",                  // username
            "OTTER",                // password
            "Substation Alpha",     // substation
            "Bay 1"                 // bay
        },
        
        {
            "2",                    // id
            "SEL-451",              // name
            "192.168.0.3",          // host
            23,                     // port
            "ACC",                  // username
            "OTTER",                // password
            "Substation Alpha",     // substation
            "Bay 3"                 // bay
        }
    };
}
