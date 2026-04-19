// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file thread_manager.hpp
 * @brief Thread management utilities for the Telnet-SML application.
 *
 * @details Provides:
 * - Periodic SER polling scheduler
 * - Graceful shutdown coordination
 *
 * @author Telnet-SML Development Team
 * @version 2.0.0
 * @date 2026
 */

#pragma once

#include <thread>
#include <atomic>
#include <functional>
#include <chrono>
#include <iostream>
#include <memory>

/**
 * @class SERPoller
 * @brief Periodic SER data polling thread.
 * 
 * @details Runs a background thread that periodically triggers SER data
 * fetching from the relay. This enables continuous monitoring without
 * blocking the main application.
 * 
 * ## Polling Cycle
 * 
 * @dot
 * digraph Polling {
 *     rankdir=TB;
 *     node [shape=ellipse, style=filled];
 *     
 *     Sleep [fillcolor=lightgray, label="Sleep\n(interval)"];
 *     Check [fillcolor=lightyellow, label="Check\nstop_flag"];
 *     Callback [fillcolor=lightgreen, label="Execute\ncallback"];
 *     
 *     Sleep -> Check;
 *     Check -> Callback [label="!stopped"];
 *     Check -> Sleep [label="stopped", style=dashed];
 *     Callback -> Sleep;
 * }
 * @enddot
 */
class SERPoller
{
    std::thread poller_thread_;                         ///< Polling thread
    std::atomic<bool> stop_flag_{false};               ///< Shutdown flag
    std::chrono::seconds poll_interval_;                ///< Interval between polls
    std::function<void()> poll_callback_;               ///< Function to call on each poll

public:
    /**
     * @brief Construct SER poller.
     *
     * @details Initialises the poller with the requested interval but does
     * not start the background thread.  Call setCallback() then start().
     *
     * @param interval Seconds between polls (default: 30).
     *
     * @post stop_flag_ == false
     * @post poller_thread_ is not joinable
     */
    explicit SERPoller(std::chrono::seconds interval = std::chrono::seconds(30))
        : poll_interval_(interval)
    {
    }

    /**
     * @brief Destructor — stops the polling thread if running.
     *
     * @post poller_thread_ has been joined.
     */
    ~SERPoller()
    {
        stop();
    }

    /**
     * @brief Set the callback to execute on each poll.
     *
     * @details The callback is invoked once every @c poll_interval_ seconds
     * from the background poller thread.  Must be set before calling start().
     *
     * @param callback Function to call (e.g., FSM SER trigger).
     *
     * @pre The poller is not yet started.
     */
    void setCallback(std::function<void()> callback)
    {
        poll_callback_ = std::move(callback);
    }

    /**
     * @brief Start the polling thread.
     *
     * @details Spawns a background thread that sleeps for @c poll_interval_
     * seconds, then invokes the callback.  The loop continues until stop()
     * is called.  If no callback has been set, the method prints an error
     * and returns without spawning a thread.
     *
     * @pre poll_callback_ has been set via setCallback().
     * @post poller_thread_ is joinable.
     */
    void start()
    {
        if (!poll_callback_)
        {
            std::cerr << "[Poller] No callback set!\n";
            return;
        }

        stop_flag_ = false;
        poller_thread_ = std::thread([this]() {
            std::cout << "[Poller] Thread started (interval: " 
                      << poll_interval_.count() << "s)\n";
            
            while (!stop_flag_.load())
            {
                // Wait for interval or stop signal
                for (int i = 0; i < poll_interval_.count() && !stop_flag_.load(); ++i)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                
                if (!stop_flag_.load())
                {
                    std::cout << "[Poller] Triggering SER poll...\n";
                    poll_callback_();
                }
            }
            
            std::cout << "[Poller] Thread stopped\n";
        });
    }

    /**
     * @brief Stop the polling thread.
     *
     * @details Sets the atomic stop flag and joins the background thread.
     * Safe to call multiple times or when not running.
     *
     * @post stop_flag_ == true
     * @post poller_thread_ is not joinable.
     */
    void stop()
    {
        stop_flag_ = true;
        
        if (poller_thread_.joinable())
            poller_thread_.join();
    }

};

/**
 * @class ThreadManager
 * @brief Centralized thread management for the application.
 * 
 * @details Coordinates the SER polling background thread.
 * Provides unified start/stop control and graceful shutdown.
 */
class ThreadManager
{
    std::unique_ptr<SERPoller> poller_;            ///< SER poller instance

public:
    /**
     * @brief Construct thread manager.
     *
     * @details Creates the internal SERPoller with the given interval.
     * Call setPollingCallback() and startAll() to activate.
     *
     * @param poll_interval Polling interval in seconds (default: 30 s).
     */
    explicit ThreadManager(std::chrono::seconds poll_interval = std::chrono::seconds(30))
        : poller_(std::make_unique<SERPoller>(poll_interval))
    {
    }

    /**
     * @brief Set the SER polling callback.
     *
     * @details Delegates to the underlying SERPoller.  Must be called
     * before startAll().
     *
     * @param callback Function to call on each poll cycle.
     *
     * @pre startAll() has not been called yet.
     */
    void setPollingCallback(std::function<void()> callback)
    {
        poller_->setCallback(std::move(callback));
    }

    /**
     * @brief Start all managed background threads.
     *
     * @details Currently starts the SER poller.  Future threads (e.g. a
     * heartbeat monitor) would be started here as well.
     *
     * @pre setPollingCallback() has been called.
     * @post SERPoller thread is running.
     */
    void startAll()
    {
        std::cout << "[ThreadManager] Starting poller...\n";
        poller_->start();
        std::cout << "[ThreadManager] Poller started\n";
    }

    /**
     * @brief Stop all managed background threads.
     *
     * @details Stops the SER poller and joins its thread.  Safe to call
     * even if startAll() was never invoked.
     *
     * @post All managed threads have been joined.
     */
    void stopAll()
    {
        std::cout << "[ThreadManager] Stopping poller...\n";
        poller_->stop();
        std::cout << "[ThreadManager] Poller stopped\n";
    }
};
