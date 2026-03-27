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
    * @param interval Seconds between polls (default: 30)
     */
    explicit SERPoller(std::chrono::seconds interval = std::chrono::seconds(30))
        : poll_interval_(interval)
    {
    }

    /**
    * @brief Destructor that stops the polling thread.
     */
    ~SERPoller()
    {
        stop();
    }

    /**
    * @brief Set the callback to execute on each poll.
    * @param callback Function to call (e.g., FSM trigger)
     */
    void setCallback(std::function<void()> callback)
    {
        poll_callback_ = std::move(callback);
    }

    /**
    * @brief Start the polling thread.
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
    * @param poll_interval Polling interval in seconds
     */
    explicit ThreadManager(std::chrono::seconds poll_interval = std::chrono::seconds(30))
        : poller_(std::make_unique<SERPoller>(poll_interval))
    {
    }

    /**
    * @brief Set the SER polling callback.
    * @param callback Function to call on each poll cycle
     */
    void setPollingCallback(std::function<void()> callback)
    {
        poller_->setCallback(std::move(callback));
    }

    /**
    * @brief Start background threads.
     */
    void startAll()
    {
        std::cout << "[ThreadManager] Starting poller...\n";
        poller_->start();
        std::cout << "[ThreadManager] Poller started\n";
    }

    /**
    * @brief Stop background threads.
     */
    void stopAll()
    {
        std::cout << "[ThreadManager] Stopping poller...\n";
        poller_->stop();
        std::cout << "[ThreadManager] Poller stopped\n";
    }
};
