// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file relay_manager.hpp
 * @brief On-demand coordinator for multiple relay pipelines.
 *
 * @details RelayManager holds the static relay configuration registry and
 * creates / destroys RelayPipeline instances on demand.  The browser UI
 * sends a WebSocket message to start or stop a specific relay, and this
 * class translates that into pipeline lifecycle operations.
 *
 * ## On-Demand Model
 *
 * @code
 * Browser:  "start relay 2"
 *       │
 *       ▼
 * SERWebSocketServer  (action handler)
 *       │
 *       ▼
 * RelayManager::startRelay("2")
 *       │
 *       ▼
 * new RelayPipeline(config[2], db, ws, shm)
 *       │
 *       ▼
 * TelnetClient → RingBuffer → ProcessingWorker → DB → WS → UI
 * @endcode
 *
 * ## Thread Safety
 *
 * All public methods are protected by a mutex so that concurrent
 * start/stop requests from multiple WebSocket sessions are serialized.
 *
 * @see relay_config.hpp    Static configuration registry
 * @see relay_pipeline.hpp  Per-relay pipeline definition
 * @see telnet_sml_app.cpp  Integrates RelayManager into the application
 *
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "relay_config.hpp"
#include "relay_pipeline.hpp"
#include "ser_database.hpp"
#include "shared_memory/shared_ring_buffer.hpp"
#include "ws_server.hpp"

/**
 * @class RelayManager
 * @brief Coordinates on-demand creation and destruction of relay pipelines.
 *
 * @details Maintains:
 * - A static list of relay configurations (from relay_config.hpp)
 * - A map of active relay pipelines (id → unique_ptr<RelayPipeline>)
 * - References to shared resources (DB, WS server, SHM ring)
 *
 * All operations are thread-safe via a mutex.
 */
class RelayManager
{
    std::vector<RelayConfig> configs_;                              ///< All known relay configs
    std::unordered_map<std::string,
                       std::unique_ptr<RelayPipeline>> active_;     ///< Active pipelines
    mutable std::mutex mutex_;                                      ///< Thread safety

    // Shared resources (not owned — lifetime managed by TelnetSmlApp::Impl)
    SERDatabase& db_;                ///< Single SQLite DB shared by every pipeline.
    SERWebSocketServer& wsServer_;   ///< Single WebSocket server used for broadcasts.
    SharedRingBuffer& shmRing_;      ///< Out-of-process shared-memory ring for JSON export.
    std::atomic<bool>& app_running_; ///< Global shutdown flag; pipelines watch this.

public:
    /**
     * @brief Construct a RelayManager.
     *
     * @param db          Shared SQLite database
     * @param wsServer    Shared WebSocket server
     * @param shmRing     Shared ring buffer for JSON file writer
     * @param app_running Global application running flag
     */
    RelayManager(SERDatabase& db,
                 SERWebSocketServer& wsServer,
                 SharedRingBuffer& shmRing,
                 std::atomic<bool>& app_running)
        : configs_(getRelayConfigs())
        , db_(db)
        , wsServer_(wsServer)
        , shmRing_(shmRing)
        , app_running_(app_running)
    {
    }

    // ─── Lifecycle ──────────────────────────────────────────────

    /**
     * @brief Start a relay pipeline on demand.
     *
     * @details Looks up the config by ID, creates a RelayPipeline, and
     * calls start().  If the relay is already active, this is a no-op.
     *
     * @param relayId  Unique relay identifier (e.g. "1", "2", "3")
     *
     * @return true   Pipeline started (or was already running)
     * @return false  Unknown relay ID or start failed
     */
    bool startRelay(const std::string& relayId)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Already running?
        if (active_.count(relayId))
        {
            std::cout << "[RelayMgr] Relay " << relayId << " already active\n";
            return true;
        }

        // Find config
        const RelayConfig* cfg = nullptr;
        for (const auto& c : configs_)
        {
            if (c.id == relayId)
            {
                cfg = &c;
                break;
            }
        }

        if (!cfg)
        {
            std::cerr << "[RelayMgr] Unknown relay ID: " << relayId << "\n";
            return false;
        }

        // Create and start pipeline
        auto pipeline = std::make_unique<RelayPipeline>(
            *cfg, db_, wsServer_, shmRing_, app_running_);

        if (!pipeline->start())
        {
            std::cerr << "[RelayMgr] Failed to start pipeline for relay " << relayId << "\n";
            return false;
        }

        std::cout << "[RelayMgr] Started relay " << relayId
                  << " (" << cfg->name << ")\n";
        active_[relayId] = std::move(pipeline);
        return true;
    }

    /**
     * @brief Stop and destroy a relay pipeline.
     *
     * @param relayId  Unique relay identifier
     *
     * @return true   Pipeline stopped and removed
     * @return false  Relay was not active
     */
    bool stopRelay(const std::string& relayId)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = active_.find(relayId);
        if (it == active_.end())
        {
            std::cout << "[RelayMgr] Relay " << relayId << " is not active\n";
            return false;
        }

        it->second->stop();
        active_.erase(it);
        std::cout << "[RelayMgr] Stopped relay " << relayId << "\n";
        return true;
    }

    /**
     * @brief Stop every active relay pipeline and clear the active set.
     *
     * @details Called during application shutdown, after the WebSocket
     * server and its handler threads have been joined, so no new
     * start/stop requests can arrive concurrently. Each pipeline's stop()
     * blocks until its worker threads join.
     *
     * @post @c active_ is empty — isActive() returns false for every relay.
     */
    void stopAll()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        for (auto& [id, pipeline] : active_)
        {
            std::cout << "[RelayMgr] Stopping relay " << id << "\n";
            pipeline->stop();
        }
        active_.clear();
        std::cout << "[RelayMgr] All relays stopped\n";
    }

    // ─── Query ──────────────────────────────────────────────────

    /**
     * @brief Check if a relay pipeline is currently active.
     *
     * @param relayId  Relay identifier
     * @return true if the relay has a running pipeline
     */
    bool isActive(const std::string& relayId) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_.count(relayId) > 0;
    }

    /**
     * @brief Queue a command to a specific relay's pipeline.
     *
     * @param relayId Relay identifier
     * @param cmd     Telnet command (e.g. "SER", "FIL DIR")
     *
     * @return true if command was queued, false if relay not active
     */
    bool queueCommand(const std::string& relayId, const std::string& cmd)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = active_.find(relayId);
        if (it == active_.end())
            return false;

        it->second->queueCommand(cmd);
        return true;
    }

    /**
     * @brief Run a user-initiated command on a specific relay synchronously.
     *
     * @details Looks up the active pipeline for @p relayId and delegates to
     * RelayPipeline::handleUserCommand(), which serialises the call against
     * the pipeline's own queue-driven execution loop. Blocks the caller
     * until the command completes or exhausts its retries.
     *
     * @param relayId Relay identifier.
     * @param cmd     Telnet command (e.g. @c "SER", @c "TAR 0", @c "EVE",
     *                @c "SET ...", @c "CTRL+C", @c "CTRL+D").
     * @return Raw relay response on success; empty string if the relay is
     *         not active or the command failed all retries.
     *
     * @note Thread-safe; the manager's mutex serialises the pipeline lookup.
     *       The actual command runs under the pipeline's internal lock.
     */
    std::string handleUserCommand(const std::string& relayId, const std::string& cmd)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = active_.find(relayId);
        if (it == active_.end())
            return "";

        return it->second->handleUserCommand(cmd);
    }

    /**
     * @brief Get the list of all known relay configurations.
     *
     * @return const reference to the relay config vector
     */
    const std::vector<RelayConfig>& getConfigs() const
    {
        return configs_;
    }

    /**
     * @brief Get all active relay IDs.
     *
     * @return Vector of currently active relay identifiers
     */
    std::vector<std::string> getActiveRelayIds() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> ids;
        ids.reserve(active_.size());
        for (const auto& [id, _] : active_)
            ids.push_back(id);
        return ids;
    }

    /**
     * @brief Update the stored Level-1 password for a relay config.
     *
     * @details Used after a successful on-relay password change so that
     * future reconnects / pipeline restarts authenticate with the new
     * value. Does not affect currently-open Telnet sessions — those are
     * already authenticated.
     *
     * @param relayId       Relay identifier
     * @param newPassword   New password value to store
     * @return true if config found and updated
     */
    bool updateStoredPassword(const std::string& relayId,
                              const std::string& newPassword)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& c : configs_)
        {
            if (c.id == relayId)
            {
                c.password = newPassword;
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Update the stored Level-2 password (2AC) for a relay config.
     *
     * @param relayId       Relay identifier
     * @param newPassword   New Level-2 password value to store
     * @return true if config found and updated
     */
    bool updateStoredPasswordL2(const std::string& relayId,
                                const std::string& newPassword)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& c : configs_)
        {
            if (c.id == relayId)
            {
                c.password_l2 = newPassword;
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Get the number of active relay pipelines.
     */
    std::size_t activeCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_.size();
    }

    /**
     * @brief Get a pointer to an active relay pipeline by ID.
     * @param relayId  Relay identifier
     * @return Pointer to RelayPipeline, or nullptr if not active
     */
    RelayPipeline* getPipeline(const std::string& relayId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = active_.find(relayId);
        return (it != active_.end()) ? it->second.get() : nullptr;
    }
};
