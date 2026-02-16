// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file telnet_sml_app.hpp
 * @brief Telnet-SML application facade (public API).
 *
 * @details Defines the public interface for the Telnet-SML application.
 * The facade hides the multi-threaded implementation behind a small, stable
 * API intended for simple start/stop orchestration.
 *
 * ## Architecture Overview
 *
 * @dot
 * digraph TelnetSmlApp {
 *     rankdir=TB;
 *     node [shape=box, style=filled, fillcolor=lightyellow];
 *
 *     subgraph cluster_public {
 *         label="Public API";
 *         App [label="TelnetSmlApp\n(Facade)", fillcolor=lightgreen];
 *     }
 *
 *     subgraph cluster_impl {
 *         label="Implementation (Hidden)";
 *         Impl [label="Impl\n(PIMPL)"];
 *         Reception [label="ReceptionWorker"];
 *         Processing [label="ProcessingWorker"];
 *         WS [label="WebSocketServer"];
 *         DB [label="SERDatabase"];
 *     }
 *
 *     App -> Impl [label="owns"];
 *     Impl -> Reception;
 *     Impl -> Processing;
 *     Impl -> WS;
 *     Impl -> DB;
 * }
 * @enddot
 *
 * ## Thread Model
 *
 * The application spawns multiple threads on start():
 * | Thread | Purpose |
 * |--------|---------|
 * | Reception | Telnet I/O with relay |
 * | Processing | Parse + DB + broadcast |
 * | WebSocket | Browser communication |
 * | Poller | Periodic data fetch |
 * | SHM Reader | JSON file output |
 *
 * ## Usage Example
 *
 * @code{.cpp}
 * #include "telnet_sml_app.hpp"
 *
 * int main() {
 *     TelnetSmlApp app;
 *
 *     if (!app.start()) {
 *         return 1;
 *     }
 *
 *     app.waitForExit();
 *     app.stop();
 *     return 0;
 * }
 * @endcode
 *
 * @see telnet_sml_app.cpp Implementation details
 * @see architecture-daigram.mmd Full architecture diagram
 *
 * @author Telnet-SML Development Team
 * @version 2.0.0
 * @date 2026
 */

#pragma once

#include <memory>

#include "dll_export.hpp"

/**
 * @class TelnetSmlApp
 * @brief Main application facade for the Telnet-SML system.
 *
 * @details Provides a simplified interface for the multi-threaded Telnet-SML
 * application. Uses the PIMPL idiom to hide implementation details and
 * maintain ABI stability.
 *
 * ## Lifecycle
 *
 * @msc
 * User,App,Impl;
 * User->App [label="create"];
 * App->Impl [label="create Impl"];
 * User->App [label="start()"];
 * Impl->Impl [label="spawn threads"];
 * App->User [label="true"];
 * User->App [label="waitForExit()"];
 * User note User [label="...running..."];
 * User->App [label="stop()"];
 * Impl->Impl [label="join threads"];
 * User->App [label="destroy"];
 * App->Impl [label="destroy Impl"];
 * @endmsc
 *
 * @invariant impl_ is always valid after construction
 * @invariant Thread-safe for single-threaded control (start/stop/wait)
 *
 * @note Non-copyable and non-movable to prevent resource confusion
 */
class TELNET_SML_API TelnetSmlApp
{
public:
    /**
     * @brief Construct a TelnetSmlApp instance.
     *
     * @details Creates the private implementation but does not start
     * any background threads. Call start() to begin operation.
     *
     * @post isRunning() == false
     */
    TelnetSmlApp();

    /**
     * @brief Destroy the TelnetSmlApp instance.
     *
     * @details Ensures graceful shutdown by calling stop() if still running.
     */
    ~TelnetSmlApp();

    /// @name Deleted Copy/Move Operations
    /// @{
    TelnetSmlApp(const TelnetSmlApp&) = delete;
    TelnetSmlApp& operator=(const TelnetSmlApp&) = delete;
    TelnetSmlApp(TelnetSmlApp&&) = delete;
    TelnetSmlApp& operator=(TelnetSmlApp&&) = delete;
    /// @}

    /**
     * @brief Start the application and all background threads.
     *
     * @details Initializes and starts:
     * - SQLite database connection
     * - WebSocket server (port 8765)
     * - Reception worker thread
     * - Processing worker thread
     * - Polling scheduler
     * - JSON file writer
     *
     * @return true if all components started successfully
     * @return false if startup failed (see console for details)
     *
     * @pre isRunning() == false
     * @post On success: isRunning() == true
     */
    bool start();

    /**
     * @brief Block until user requests application exit.
     *
     * @details Waits for Enter key press. Typically called from main()
     * after start() returns successfully.
     *
     * @pre isRunning() == true
     */
    void waitForExit();

    /**
     * @brief Stop the application and all background threads.
     *
     * @details Gracefully shuts down all components in reverse order.
     * Safe to call multiple times or even if not started.
     *
     * @post isRunning() == false
     */
    void stop();

    /**
     * @brief Check if the application is currently running.
     *
     * @return true if the application is running
     * @return false if the application is stopped
     */
    bool isRunning() const;

private:
    class Impl;                     ///< Forward declaration of implementation
    std::unique_ptr<Impl> impl_;    ///< PIMPL pointer to implementation
};
