// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file shared_db_memory.hpp
 * @brief Stub for the optional shared-memory DB change notifier.
 *
 * @details `WSDBServer` accepts an optional `SharedDBMemory*` so external
 * processes can subscribe to DB mutations without polling. The full
 * implementation lives in a separate IPC subsystem; this header provides
 * the minimum surface required to compile WSDBServer in builds that do
 * not need shared-memory notifications.
 *
 * Pass `nullptr` for the `shm` parameter when constructing WSDBServer
 * to disable change notifications entirely.
 */

#pragma once

#include <string>

/**
 * @class SharedDBMemory
 * @brief No-op stub. Real implementation publishes JSON change events to a
 *        named shared-memory region.
 *
 * The fields/methods below match the call sites in `ws_db_server.hpp` so
 * that linking succeeds even when the real IPC notifier is not built.
 * All methods are no-ops; any pointer-typed callers must check for null
 * before invoking (which they already do — `shm_` defaults to `nullptr`).
 */
class SharedDBMemory
{
public:
    /// Publish a JSON change notification (no-op stub).
    void write(const std::string& /*json*/) {}

    /// Publish a JSON change notification (alias used by some call sites).
    void notify(const std::string& json) { write(json); }

    /// Publish a structured event (no-op stub).
    void notifyEvent(const std::string& /*table*/,
                     const std::string& /*action*/,
                     const std::string& /*payload*/) {}
};
