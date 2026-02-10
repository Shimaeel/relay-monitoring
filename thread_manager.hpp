/**
 * @file thread_manager.hpp
 * @brief Thread Management for Multi-threaded Application
 * 
 * @details Provides thread-safe utilities for the Telnet-SML application:
 * - Thread-safe database write queue
 * - Periodic SER polling scheduler
 * - Graceful shutdown management
 * 
 * ## Threading Model
 * 
 * @dot
 * digraph ThreadModel {
 *     rankdir=TB;
 *     node [shape=box, style=filled, fillcolor=lightyellow];
 *     
 *     subgraph cluster_threads {
 *         label="Application Threads";
 *         main [label="Main Thread\n(FSM Control)"];
 *         ws [label="WebSocket Thread\n(UI Server)"];
 *         poll [label="Polling Thread\n(SER Fetcher)"];
 *         db [label="DB Writer Thread\n(Async Writes)"];
 *     }
 *     
 *     subgraph cluster_sync {
 *         label="Synchronization";
 *         queue [label="Write Queue\n(mutex protected)", shape=cylinder, fillcolor=lightblue];
 *         stop [label="Stop Flag\n(atomic)", shape=diamond, fillcolor=lightcoral];
 *     }
 *     
 *     main -> stop [label="sets"];
 *     poll -> queue [label="pushes"];
 *     db -> queue [label="pops"];
 *     ws -> stop [label="checks"];
 *     poll -> stop [label="checks"];
 *     db -> stop [label="checks"];
 * }
 * @enddot
 * 
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <functional>
#include <chrono>
#include <iostream>
#include <vector>

#include "ser_database.hpp"
#include "ser_record.hpp"

/**
 * @class ThreadSafeQueue
 * @brief Thread-safe queue for producer-consumer pattern
 * 
 * @tparam T Type of elements stored in queue
 * 
 * @details Provides mutex-protected enqueue/dequeue operations with
 * condition variable signaling for efficient waiting.
 */
template<typename T>
class ThreadSafeQueue
{
    std::queue<T> queue_;               ///< Underlying queue storage
    mutable std::mutex mutex_;          ///< Mutex for thread safety
    std::condition_variable cv_;        ///< Condition variable for signaling

public:
    /**
     * @brief Add item to queue
     * @param item Item to enqueue (moved)
     */
    void push(T item)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(item));
        }
        cv_.notify_one();
    }

    /**
     * @brief Remove and return front item (blocking)
     * @param stop_flag Reference to atomic flag for graceful shutdown
     * @return std::optional<T> Item if available, nullopt if stopped
     */
    bool pop(T& item, std::atomic<bool>& stop_flag)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this, &stop_flag]() {
            return !queue_.empty() || stop_flag.load();
        });
        
        if (stop_flag.load() && queue_.empty())
            return false;
        
        item = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    /**
     * @brief Check if queue is empty
     * @return true if empty
     */
    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    /**
     * @brief Get queue size
     * @return Number of items in queue
     */
    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    /**
     * @brief Wake up all waiting threads (for shutdown)
     */
    void notify_all()
    {
        cv_.notify_all();
    }
};

/**
 * @class DatabaseWriter
 * @brief Asynchronous database writer thread
 * 
 * @details Processes SER records from a queue and writes them to the database
 * in a background thread. This prevents database I/O from blocking the main
 * application flow.
 * 
 * ## Operation Flow
 * 
 * @dot
 * digraph DBWriter {
 *     rankdir=LR;
 *     node [shape=box, style=filled];
 *     
 *     Queue [label="Write Queue", fillcolor=lightyellow];
 *     Thread [label="Writer Thread", fillcolor=lightblue];
 *     DB [label="SQLite DB", fillcolor=lightgreen, shape=cylinder];
 *     
 *     Queue -> Thread [label="pop()"];
 *     Thread -> DB [label="insertRecord()"];
 * }
 * @enddot
 */
class DatabaseWriter
{
    SERDatabase& db_;                                   ///< Reference to database
    ThreadSafeQueue<SERRecord> write_queue_;           ///< Queue of records to write
    std::thread writer_thread_;                         ///< Background writer thread
    std::atomic<bool> stop_flag_{false};               ///< Shutdown flag
    std::mutex db_mutex_;                               ///< Mutex for database access

public:
    /**
     * @brief Construct database writer
     * @param db Reference to SER database
     */
    explicit DatabaseWriter(SERDatabase& db) : db_(db) {}

    /**
     * @brief Destructor - stops writer thread
     */
    ~DatabaseWriter()
    {
        stop();
    }

    /**
     * @brief Start the background writer thread
     */
    void start()
    {
        stop_flag_ = false;
        writer_thread_ = std::thread([this]() {
            std::cout << "[DB Writer] Thread started\n";
            
            while (!stop_flag_.load())
            {
                SERRecord record;
                if (write_queue_.pop(record, stop_flag_))
                {
                    std::lock_guard<std::mutex> lock(db_mutex_);
                    if (db_.insertRecord(record))
                    {
                        std::cout << "[DB Writer] Inserted record: " << record.description << "\n";
                    }
                }
            }
            
            std::cout << "[DB Writer] Thread stopped\n";
        });
    }

    /**
     * @brief Stop the writer thread
     */
    void stop()
    {
        stop_flag_ = true;
        write_queue_.notify_all();
        
        if (writer_thread_.joinable())
            writer_thread_.join();
    }

    /**
     * @brief Queue a single record for async writing
     * @param record Record to write
     */
    void queueRecord(const SERRecord& record)
    {
        write_queue_.push(record);
    }

    /**
     * @brief Queue multiple records for async writing
     * @param records Vector of records to write
     */
    void queueRecords(const std::vector<SERRecord>& records)
    {
        for (const auto& record : records)
        {
            write_queue_.push(record);
        }
        std::cout << "[DB Writer] Queued " << records.size() << " records\n";
    }

    /**
     * @brief Get mutex for direct database access
     * @return Reference to database mutex
     */
    std::mutex& getDatabaseMutex() { return db_mutex_; }

    /**
     * @brief Get pending write count
     * @return Number of records waiting to be written
     */
    size_t pendingWrites() const { return write_queue_.size(); }
};

/**
 * @class SERPoller
 * @brief Periodic SER data polling thread
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
     * @brief Construct SER poller
     * @param interval Seconds between polls (default: 30)
     */
    explicit SERPoller(std::chrono::seconds interval = std::chrono::seconds(30))
        : poll_interval_(interval)
    {
    }

    /**
     * @brief Destructor - stops polling thread
     */
    ~SERPoller()
    {
        stop();
    }

    /**
     * @brief Set the callback to execute on each poll
     * @param callback Function to call (e.g., FSM trigger)
     */
    void setCallback(std::function<void()> callback)
    {
        poll_callback_ = std::move(callback);
    }

    /**
     * @brief Start the polling thread
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
     * @brief Stop the polling thread
     */
    void stop()
    {
        stop_flag_ = true;
        
        if (poller_thread_.joinable())
            poller_thread_.join();
    }

    /**
     * @brief Check if poller is running
     * @return true if running
     */
    bool isRunning() const { return !stop_flag_.load(); }

    /**
     * @brief Set polling interval
     * @param interval New interval in seconds
     */
    void setInterval(std::chrono::seconds interval)
    {
        poll_interval_ = interval;
    }
};

/**
 * @class ThreadManager
 * @brief Centralized thread management for the application
 * 
 * @details Coordinates all background threads:
 * - WebSocket server (managed separately)
 * - Database writer
 * - SER poller
 * 
 * Provides unified start/stop control and graceful shutdown.
 */
class ThreadManager
{
    std::unique_ptr<DatabaseWriter> db_writer_;    ///< Database writer instance
    std::unique_ptr<SERPoller> poller_;            ///< SER poller instance
    std::atomic<bool> running_{false};             ///< Overall running state

public:
    /**
     * @brief Construct thread manager
     * @param db Reference to SER database
     * @param poll_interval Polling interval in seconds
     */
    ThreadManager(SERDatabase& db, std::chrono::seconds poll_interval = std::chrono::seconds(30))
        : db_writer_(std::make_unique<DatabaseWriter>(db))
        , poller_(std::make_unique<SERPoller>(poll_interval))
    {
    }

    /**
     * @brief Set the SER polling callback
     * @param callback Function to call on each poll cycle
     */
    void setPollingCallback(std::function<void()> callback)
    {
        poller_->setCallback(std::move(callback));
    }

    /**
     * @brief Start all background threads
     */
    void startAll()
    {
        std::cout << "[ThreadManager] Starting all threads...\n";
        db_writer_->start();
        poller_->start();
        running_ = true;
        std::cout << "[ThreadManager] All threads started\n";
    }

    /**
     * @brief Stop all background threads
     */
    void stopAll()
    {
        std::cout << "[ThreadManager] Stopping all threads...\n";
        poller_->stop();
        db_writer_->stop();
        running_ = false;
        std::cout << "[ThreadManager] All threads stopped\n";
    }

    /**
     * @brief Get database writer for queuing records
     * @return Reference to database writer
     */
    DatabaseWriter& getWriter() { return *db_writer_; }

    /**
     * @brief Get poller for configuration
     * @return Reference to SER poller
     */
    SERPoller& getPoller() { return *poller_; }

    /**
     * @brief Check if threads are running
     * @return true if running
     */
    bool isRunning() const { return running_.load(); }
};
