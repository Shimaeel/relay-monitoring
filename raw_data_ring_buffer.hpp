// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file raw_data_ring_buffer.hpp
 * @brief MPMC (Multiple-Producer Multiple-Consumer) Ring Buffer for Raw Telnet Data
 * 
 * @details This header implements a Multiple-Producer Multiple-Consumer (MPMC)
 * ring buffer for raw Telnet data. Any number of producer threads can push data,
 * and multiple independent consumer threads can each receive every message
 * (broadcast / pub-sub pattern).
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
 *     Ring [label="RawDataRingBuffer\n(MPMC)", fillcolor=lightgreen];
 *     FSM [label="Consumer 1\n(Processing)"];
 *     Other [label="Consumer N\n(Analytics etc.)", style=dashed];
 *     
 *     Relay -> Telnet [label="TCP"];
 *     Telnet -> Ring [label="push\n(non-blocking)"];
 *     Ring -> FSM [label="waitPop\n(blocking)"];
 *     Ring -> Other [label="waitPop\n(blocking)", style=dashed];
 * }
 * @enddot
 * 
 * ## Design Goals
 * 
 * - **Multiple producers**: Any thread can push() safely (concurrent writes OK)
 * - **Multiple consumers**: Up to kMaxReaders independent readers (broadcast)
 * - **Non-blocking producer**: Push never blocks; drops oldest if full
 * - **Blocking consumer**: Each reader waits efficiently for its next message
 * - **Message-oriented**: Each push/pop is a complete message (command + response)
 * - **Backpressure**: Auto-drops oldest messages when buffer is full (advances slowest reader)
 * 
 * ## Usage
 * 
 * @code{.cpp}
 * RawDataRingBuffer buffer(100);  // Max 100 messages
 * 
 * // Register readers BEFORE producers start pushing
 * auto id1 = buffer.registerReader();   // Consumer 1
 * auto id2 = buffer.registerReader();   // Consumer 2
 * 
 * // Producer (any thread)
 * buffer.push({"SER", rawResponse});
 * 
 * // Consumer 1 (own thread)
 * std::atomic<bool> stop{false};
 * auto msg = buffer.waitPop(id1, stop);
 * if (msg) { /* process */ }
 * 
 * // Consumer 2 also gets the SAME message independently
 * auto msg2 = buffer.waitPop(id2, stop);
 * 
 * // Cleanup
 * buffer.unregisterReader(id1);
 * buffer.unregisterReader(id2);
 * @endcode
 * 
 * @author Telnet-SML Development Team
 * @version 2.0.0
 * @date 2026
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
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
 * @brief MPMC ring buffer for decoupling Telnet reception from multiple consumers
 * 
 * @details Provides a thread-safe queue for passing raw Telnet data from
 * multiple producer threads to multiple consumer threads. Each registered
 * reader receives every message (broadcast / pub-sub pattern).
 * 
 * Internally uses a deque with per-reader logical cursors. Messages are only
 * removed from the deque once ALL active readers have consumed them.
 * 
 * ## Thread Safety
 * - Multiple producers can call push() concurrently
 * - Multiple consumers each call waitPop() with their own ReaderId
 * - All public methods are thread-safe
 * 
 * ## Usage Example
 * 
 * @code{.cpp}
 * RawDataRingBuffer buffer(100);  // Max 100 messages
 * 
 * // Register readers
 * auto reader1 = buffer.registerReader();
 * auto reader2 = buffer.registerReader();
 * 
 * // Producer (any thread)
 * buffer.push({"SER", rawResponse});
 * 
 * // Consumer 1 (its own thread)
 * std::atomic<bool> stop{false};
 * auto msg = buffer.waitPop(reader1, stop);
 * 
 * // Consumer 2 also gets the same message
 * auto msg2 = buffer.waitPop(reader2, stop);
 * 
 * // Cleanup
 * buffer.unregisterReader(reader1);
 * buffer.unregisterReader(reader2);
 * @endcode
 */
class RawDataRingBuffer
{
public:
    /// Maximum number of concurrent readers supported
    static constexpr uint32_t kMaxReaders = 8U;

    /// Opaque reader identifier returned by registerReader()
    using ReaderId = uint32_t;

    /// Sentinel value indicating an invalid / unregistered reader
    static constexpr ReaderId kInvalidReader = UINT32_MAX;

    /**
     * @brief Construct buffer with maximum capacity
     * 
     * @param maxMessages Maximum number of messages to hold (oldest dropped when full)
     */
    explicit RawDataRingBuffer(std::size_t maxMessages = 100)
        : max_size_(maxMessages)
    {
    }

    // ─── Reader Management ───────────────────────────────────────────

    /**
     * @brief Register a new reader and obtain a ReaderId.
     * 
     * @details The new reader's cursor starts at the current write position,
     * so it will only see messages written after registration.
     * 
     * @return ReaderId on success, kInvalidReader if all slots are full.
     * @note Thread-safe.
     */
    ReaderId registerReader()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (uint32_t i = 0U; i < kMaxReaders; ++i)
        {
            if (!readers_[i].active)
            {
                readers_[i].active      = true;
                readers_[i].read_index  = base_index_ + queue_.size();
                readers_[i].total_reads = 0U;
                return i;
            }
        }
        return kInvalidReader;
    }

    /**
     * @brief Unregister a reader, freeing its slot.
     * 
     * @param id ReaderId previously returned by registerReader().
     * @note Thread-safe. Safe to call with kInvalidReader (no-op).
     */
    void unregisterReader(ReaderId id)
    {
        if (id >= kMaxReaders)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        readers_[id].active = false;
        trimConsumed();
    }

    // ─── Write (MPMC-safe) ───────────────────────────────────────────

    /**
     * @brief Push a message into the buffer (non-blocking, multiple writers OK)
     * 
     * @details If buffer is full, oldest message is dropped to make room
     * and all readers at that position are advanced. This ensures the
     * producer path never blocks.
     * 
     * @param msg Message to push (moved into buffer)
     * @return true Message was added without dropping
     * @return false Message was added but oldest was dropped (overwrite)
     * @note Thread-safe. Multiple threads can call push() concurrently.
     */
    bool push(RawDataMessage msg)
    {
        bool dropped = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // Drop oldest if at capacity (never block producer)
            while (queue_.size() >= max_size_)
            {
                // Advance any reader still pointing at the oldest message
                for (auto& r : readers_)
                {
                    if (r.active && r.read_index <= base_index_)
                        r.read_index = base_index_ + 1U;
                }
                queue_.pop_front();
                ++base_index_;
                dropped = true;
                ++drop_count_;
            }

            queue_.push_back(std::move(msg));
            ++total_writes_;
        }
        cv_.notify_all();   // wake ALL waiting consumers
        return !dropped;
    }

    // ─── Read (per-reader cursor) ────────────────────────────────────

    /**
     * @brief Wait for and pop a message for a specific registered reader.
     * 
     * @details Blocks until a message is available for this reader or
     * stop flag is set. Each reader maintains its own cursor and receives
     * every message independently.
     * 
     * @param readerId ReaderId from registerReader()
     * @param stopFlag Atomic flag to signal thread termination
     * @return std::optional<RawDataMessage> Message if available, nullopt if stopped
     * @note Thread-safe.
     */
    std::optional<RawDataMessage> waitPop(ReaderId readerId, std::atomic<bool>& stopFlag)
    {
        if (readerId >= kMaxReaders)
            return std::nullopt;

        std::unique_lock<std::mutex> lock(mutex_);

        cv_.wait(lock, [&]() {
            return stopFlag.load(std::memory_order_relaxed)
                || (readers_[readerId].active
                    && readers_[readerId].read_index < base_index_ + queue_.size());
        });

        if (stopFlag.load(std::memory_order_relaxed))
            return std::nullopt;

        auto& reader = readers_[readerId];
        if (!reader.active || reader.read_index >= base_index_ + queue_.size())
            return std::nullopt;

        std::size_t dequeIdx = reader.read_index - base_index_;
        RawDataMessage msg = queue_[dequeIdx];   // COPY — other readers still need it
        ++reader.read_index;
        ++reader.total_reads;

        trimConsumed();
        return msg;
    }

    /**
     * @brief Try to pop a message for a reader without waiting.
     * 
     * @param readerId ReaderId from registerReader()
     * @return std::optional<RawDataMessage> Message if available, nullopt if empty
     * @note Thread-safe.
     */
    std::optional<RawDataMessage> tryPop(ReaderId readerId)
    {
        if (readerId >= kMaxReaders)
            return std::nullopt;

        std::lock_guard<std::mutex> lock(mutex_);

        auto& reader = readers_[readerId];
        if (!reader.active || reader.read_index >= base_index_ + queue_.size())
            return std::nullopt;

        std::size_t dequeIdx = reader.read_index - base_index_;
        RawDataMessage msg = queue_[dequeIdx];   // COPY
        ++reader.read_index;
        ++reader.total_reads;

        trimConsumed();
        return msg;
    }

    // ─── Capacity ────────────────────────────────────────────────────

    /**
     * @brief Get current number of messages in buffer
     * @note Thread-safe.
     */
    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    /**
     * @brief Check if buffer is empty
     * @note Thread-safe.
     */
    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    // ─── Notification ────────────────────────────────────────────────

    /**
     * @brief Wake up all waiting consumers (for shutdown)
     */
    void notifyAll()
    {
        cv_.notify_all();
    }

    /**
     * @brief Clear all messages from buffer
     * @note Thread-safe. Resets all reader cursors to the new base.
     */
    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        base_index_ += queue_.size();
        queue_.clear();
        for (auto& r : readers_)
        {
            if (r.active)
                r.read_index = base_index_;
        }
    }

    // ─── Statistics ──────────────────────────────────────────────────

    /**
     * @brief Number of currently registered (active) readers.
     * @note Thread-safe.
     */
    uint32_t activeReaderCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        uint32_t count = 0U;
        for (const auto& r : readers_)
        {
            if (r.active)
                ++count;
        }
        return count;
    }

    /**
     * @brief Total number of messages successfully written.
     * @note Thread-safe.
     */
    uint64_t totalWrites() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return total_writes_;
    }

    /**
     * @brief Total reads performed by a specific reader.
     * @param readerId ReaderId to query.
     * @return Read count, or 0 if readerId is invalid.
     * @note Thread-safe.
     */
    uint64_t totalReads(ReaderId readerId) const
    {
        if (readerId >= kMaxReaders)
            return 0U;
        std::lock_guard<std::mutex> lock(mutex_);
        return readers_[readerId].total_reads;
    }

    /**
     * @brief Total number of messages dropped due to buffer overflow.
     * @note Thread-safe.
     */
    uint64_t dropCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return drop_count_;
    }

private:
    /**
     * @brief Per-reader state.
     */
    struct ReaderSlot
    {
        bool active          = false;   ///< true if this slot is in use
        std::size_t read_index = 0U;    ///< Logical index into the stream
        uint64_t total_reads = 0U;      ///< Number of messages read by this reader
    };

    /**
     * @brief Remove messages that ALL active readers have consumed.
     * 
     * @details Finds the minimum read_index among all active readers and
     * removes messages from the front of the deque up to that point.
     * 
     * @pre Called with mutex held.
     */
    void trimConsumed()
    {
        std::size_t minIdx = base_index_ + queue_.size();
        bool anyActive = false;

        for (const auto& r : readers_)
        {
            if (r.active)
            {
                anyActive = true;
                if (r.read_index < minIdx)
                    minIdx = r.read_index;
            }
        }

        if (!anyActive)
            return;   // No active readers — keep messages for future readers

        // Remove messages before the slowest reader
        while (base_index_ < minIdx && !queue_.empty())
        {
            queue_.pop_front();
            ++base_index_;
        }
    }

    std::size_t max_size_;                      ///< Maximum buffer capacity
    std::deque<RawDataMessage> queue_;           ///< Message storage
    ReaderSlot readers_[kMaxReaders] = {};       ///< Per-reader cursors
    std::size_t base_index_ = 0U;               ///< Logical index of queue_[0]
    uint64_t total_writes_ = 0U;                ///< Total messages written
    uint64_t drop_count_ = 0U;                  ///< Total messages dropped
    mutable std::mutex mutex_;                  ///< Protects all state
    std::condition_variable cv_;                ///< Notifies waiting consumers
};
