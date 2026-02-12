/**
 * @file raw_data_ring_buffer.hpp
 * @brief Lock-free SPSC Ring Buffer for Raw Telnet Data
 * 
 * @details This header implements a Single-Producer Single-Consumer (SPSC)
 * ring buffer specifically designed for non-blocking reception of raw
 * Telnet data. The reception thread pushes data immediately, while the
 * processing thread reads and forwards to the FSM.
 * 
 * ## Architecture Position
 * 
 * @dot
 * digraph DataFlow {
 *     rankdir=LR;
 *     node [shape=box, style=filled, fillcolor=lightyellow];
 *     
 *     Relay [label="Relay Device", shape=ellipse, fillcolor=lightblue];
 *     Telnet [label="TelnetClient\n(Reception)"];
 *     Ring [label="RawDataRingBuffer\n(SPSC)", fillcolor=lightgreen];
 *     FSM [label="FSM Worker\n(Processing)"];
 *     
 *     Relay -> Telnet [label="TCP"];
 *     Telnet -> Ring [label="push\n(non-blocking)"];
 *     Ring -> FSM [label="pop\n(blocking wait)"];
 * }
 * @enddot
 * 
 * ## Design Goals
 * 
 * - **Non-blocking producer**: Reception thread never blocks on buffer full
 * - **Blocking consumer**: Processing thread waits efficiently for data
 * - **Message-oriented**: Each push/pop is a complete message (command + response)
 * 
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <optional>

/**
 * @struct RawDataMessage
 * @brief Container for raw Telnet command/response pair
 */
struct RawDataMessage
{
    std::string command;    ///< Command that was sent (e.g., "SER")
    std::string response;   ///< Raw response data from relay
};

/**
 * @class RawDataRingBuffer
 * @brief SPSC ring buffer for decoupling Telnet reception from FSM processing
 * 
 * @details Provides a thread-safe queue for passing raw Telnet data from
 * the reception thread to the processing thread. Uses a deque internally
 * with mutex protection and condition variable for efficient waiting.
 * 
 * ## Thread Safety
 * - Single producer (reception thread) calls push()
 * - Single consumer (processing thread) calls waitPop()
 * - Multiple threads can safely call size() and empty()
 * 
 * ## Usage Example
 * 
 * @code{.cpp}
 * RawDataRingBuffer buffer(100);  // Max 100 messages
 * 
 * // Producer (reception thread)
 * buffer.push({"SER", rawResponse});
 * 
 * // Consumer (processing thread)
 * std::atomic<bool> stop{false};
 * auto msg = buffer.waitPop(stop);
 * if (msg) {
 *     // Process msg->response through FSM
 * }
 * @endcode
 */
class RawDataRingBuffer
{
public:
    /**
     * @brief Construct buffer with maximum capacity
     * 
     * @param maxMessages Maximum number of messages to hold (oldest dropped when full)
     */
    explicit RawDataRingBuffer(std::size_t maxMessages = 100)
        : max_size_(maxMessages)
    {
    }

    /**
     * @brief Push a message into the buffer (non-blocking)
     * 
     * @details If buffer is full, oldest message is dropped to make room.
     * This ensures the reception path never blocks.
     * 
     * @param msg Message to push (moved into buffer)
     * @return true Message was added
     * @return false Message was added but oldest was dropped (overwrite)
     */
    bool push(RawDataMessage msg)
    {
        bool dropped = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            
            // Drop oldest if at capacity (never block producer)
            if (queue_.size() >= max_size_)
            {
                queue_.pop_front();
                dropped = true;
            }
            
            queue_.push_back(std::move(msg));
        }
        cv_.notify_one();
        return !dropped;
    }

    /**
     * @brief Wait for and pop a message from the buffer
     * 
     * @details Blocks until a message is available or stop flag is set.
     * Used by the processing thread to wait efficiently for data.
     * 
     * @param stopFlag Atomic flag to signal thread termination
     * @return std::optional<RawDataMessage> Message if available, nullopt if stopped
     */
    std::optional<RawDataMessage> waitPop(std::atomic<bool>& stopFlag)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        
        cv_.wait(lock, [&]() {
            return stopFlag.load(std::memory_order_relaxed) || !queue_.empty();
        });
        
        if (stopFlag.load(std::memory_order_relaxed))
            return std::nullopt;
        
        if (queue_.empty())
            return std::nullopt;
        
        RawDataMessage msg = std::move(queue_.front());
        queue_.pop_front();
        return msg;
    }

    /**
     * @brief Try to pop a message without waiting
     * 
     * @return std::optional<RawDataMessage> Message if available, nullopt if empty
     */
    std::optional<RawDataMessage> tryPop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (queue_.empty())
            return std::nullopt;
        
        RawDataMessage msg = std::move(queue_.front());
        queue_.pop_front();
        return msg;
    }

    /**
     * @brief Get current number of messages in buffer
     */
    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    /**
     * @brief Check if buffer is empty
     */
    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    /**
     * @brief Wake up any waiting consumers (for shutdown)
     */
    void notifyAll()
    {
        cv_.notify_all();
    }

    /**
     * @brief Clear all messages from buffer
     */
    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }

private:
    std::size_t max_size_;
    std::deque<RawDataMessage> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};
