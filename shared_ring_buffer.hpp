/**
 * @file shared_ring_buffer.hpp
 * @brief Lock-free Shared Ring Buffer for Inter-thread Communication.
 *
 * @details Implements a high-performance SPSC (Single-Producer Single-Consumer)
 * ring buffer for sharing binary data between threads. Primarily used for
 * passing ASN.1 TLV-encoded SER records to the JSON file writer thread.
 *
 * @section arch_position Architecture Position
 *
 * @dot
 * digraph SharedRingFlow {
 *     rankdir=LR;
 *     node [shape=box, style=filled, fillcolor=lightyellow];
 *
 *     Processing [label="ProcessingWorker\n(Producer)"];
 *     Ring [label="SharedRingBuffer\n(500KB)", fillcolor=lightgreen];
 *     Reader [label="SharedSerReader\n(Consumer)"];
 *     JSON [label="ui/data.json\n(Output)", shape=note];
 *
 *     Processing -> Ring [label="write()\nASN.1 TLV"];
 *     Ring -> Reader [label="waitRead()\n(blocking)"];
 *     Reader -> JSON [label="writeJsonToFile()"];
 * }
 * @enddot
 *
 * @section buffer_structure Buffer Memory Layout
 *
 * Each payload is stored with a 4-byte little-endian length header:
 * @verbatim
 * +----------+------------------+----------+------------------+------+
 * | 4B len_0 | payload_0[0..n] | 4B len_1 | payload_1[0..m] | ...  |
 * +----------+------------------+----------+------------------+------+
 *            ^                             ^
 *         read_pos                      write_pos
 * @endverbatim
 *
 * @section design_goals Design Goals
 *
 * | Goal | Description |
 * |------|-------------|
 * | Non-blocking producer | write() never blocks; drops oldest on overflow |
 * | Blocking consumer | waitRead() efficiently waits via condition variable |
 * | Binary-safe | Handles arbitrary byte sequences (ASN.1 TLV payloads) |
 * | Overwrite detection | Tracks when data was dropped due to overflow |
 *
 * @section thread_safety Thread Safety Model
 *
 * - **Producer**: Single thread calls write()
 * - **Consumer**: Single thread calls waitRead()
 * - **Shutdown**: Any thread may call notifyAll()
 *
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 *
 * @see ProcessingWorker Producer that writes TLV payloads
 * @see SharedSerReader Consumer that reads and converts to JSON
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

/**
 * @class SharedRingBuffer
 * @brief Thread-safe SPSC ring buffer for variable-length binary payloads.
 *
 * @details Provides a circular buffer where each payload is prefixed with a
 * 4-byte little-endian length header. Designed for efficient inter-thread
 * communication with minimal synchronization overhead.
 *
 * @par Thread Safety
 * - Single producer thread calls write()
 * - Single consumer thread calls waitRead()
 * - Multiple threads may call notifyAll() for coordinated shutdown
 *
 * @par Memory Model
 * Uses acquire-release semantics for lock-free position updates.
 * Condition variable provides efficient blocking for the consumer.
 *
 * @par Example Usage
 * @code{.cpp}
 * // Initialize buffer (500KB capacity)
 * SharedRingBuffer ring(500 * 1024);
 *
 * // Producer thread
 * void producer() {
 *     std::vector<uint8_t> payload = encodeData();
 *     if (!ring.write(payload.data(), payload.size())) {
 *         // Handle write failure (payload too large)
 *     }
 * }
 *
 * // Consumer thread
 * void consumer(std::atomic<bool>& stop) {
 *     std::vector<uint8_t> data;
 *     while (!stop.load()) {
 *         if (ring.waitRead(data, stop)) {
 *             processPayload(data);
 *         }
 *     }
 * }
 *
 * // Shutdown
 * void shutdown() {
 *     stop.store(true);
 *     ring.notifyAll();  // Wake consumer
 * }
 * @endcode
 *
 * @invariant capacity_ > kHeaderSize (minimum 5 bytes)
 * @invariant read_pos_ <= capacity_ && write_pos_ <= capacity_
 */
class SharedRingBuffer
{
public:
    // =========================================================================
    /// @name Construction
    // =========================================================================
    /// @{

    /**
     * @brief Constructs a ring buffer with specified capacity.
     *
     * @param capacity_bytes Total buffer size in bytes (including headers).
     *        Recommended minimum: 1024 bytes for practical use.
     *
     * @post capacity() == capacity_bytes
     * @post Buffer is empty (no pending data)
     *
     * @note Actual usable payload capacity is `capacity_bytes - 4` per message
     *       due to the 4-byte length header required for each payload.
     */
    explicit SharedRingBuffer(std::size_t capacity_bytes)
        : capacity_(capacity_bytes), buffer_(capacity_bytes, 0U)
    {
    }

    /// @}

    // =========================================================================
    /// @name Capacity
    // =========================================================================
    /// @{

    /**
     * @brief Returns the total buffer capacity in bytes.
     * @return Total capacity including header overhead.
     */
    std::size_t capacity() const
    {
        return capacity_;
    }

    /// @}

    // =========================================================================
    /// @name Producer Interface
    // =========================================================================
    /// @{

    // =========================================================================
    /// @name Producer Interface
    // =========================================================================
    /// @{

    /**
     * @brief Writes a payload to the buffer (non-blocking).
     *
     * @details Appends a length-prefixed payload to the ring buffer.
     * If insufficient space exists, oldest payloads are automatically
     * dropped until enough room is available.
     *
     * @param[in] data Pointer to payload data (must not be nullptr).
     * @param[in] len  Length of payload in bytes (must be > 0).
     *
     * @retval true  Write succeeded.
     * @retval false Write failed due to:
     *               - nullptr data pointer
     *               - Zero length
     *               - Payload exceeds maximum size (capacity - 4)
     *
     * @post If true: Data is available for consumer.
     * @post If data was dropped: overwriteOccurred() returns true.
     *
     * @note This method NEVER blocks the calling thread.
     * @note Maximum payload size: capacity() - kHeaderSize bytes.
     *
     * @par Thread Safety
     * Must be called from a single producer thread only.
     */
    bool write(const uint8_t* data, std::size_t len)
    {
        if (data == nullptr || len == 0U)
            return false;

        if (len > maxPayloadSize())
            return false;

        std::size_t readPos = read_pos_.load(std::memory_order_acquire);
        std::size_t writePos = write_pos_.load(std::memory_order_relaxed);

        readPos = normalizePos(readPos);
        writePos = normalizePos(writePos);

        if (writePos + kHeaderSize + len > capacity_)
        {
            writePos = 0U;
        }

        std::size_t freeSpace = freeBytes(readPos, writePos);
        while (freeSpace < kHeaderSize + len)
        {
            if (!dropOldest(readPos, writePos))
            {
                return false;
            }
            overwrite_flag_.store(true, std::memory_order_relaxed);
            freeSpace = freeBytes(readPos, writePos);
        }

        writeLength(writePos, static_cast<uint32_t>(len));
        std::memcpy(&buffer_[writePos + kHeaderSize], data, len);

        std::size_t newWrite = writePos + kHeaderSize + len;
        if (newWrite >= capacity_)
        {
            newWrite = 0U;
        }

        read_pos_.store(readPos, std::memory_order_release);
        write_pos_.store(newWrite, std::memory_order_release);

        cv_.notify_one();
        return true;
    }

    /// @}

    // =========================================================================
    /// @name Consumer Interface
    // =========================================================================
    /// @{

    /**
     * @brief Waits for and reads a payload from the buffer (blocking).
     *
     * @details Blocks the calling thread until one of:
     * - A payload becomes available (returns with data)
     * - The stop_flag is set externally (returns false)
     *
     * @param[out] out       Vector to receive the payload data.
     *                       Cleared and resized automatically.
     * @param[in]  stop_flag Atomic flag checked during wait.
     *                       Set to true externally to abort the wait.
     *
     * @retval true  Successfully read a payload into @p out.
     * @retval false Stopped (stop_flag set) or internal error.
     *
     * @pre  Consumer thread owns this call exclusively.
     * @post If true: @p out contains the payload; read position advanced.
     * @post If false: @p out may be in an undefined state.
     *
     * @par Thread Safety
     * Must be called from a single consumer thread only.
     *
     * @par Blocking Behavior
     * Uses condition_variable::wait() internally.
     * Call notifyAll() from another thread to unblock.
     */
    bool waitRead(std::vector<uint8_t>& out, std::atomic<bool>& stop_flag)
    {
        std::unique_lock<std::mutex> lock(cv_mutex_);
        cv_.wait(lock, [&]() {
            return stop_flag.load(std::memory_order_relaxed)
                || read_pos_.load(std::memory_order_acquire) != write_pos_.load(std::memory_order_acquire);
        });

        if (stop_flag.load(std::memory_order_relaxed))
            return false;

        lock.unlock();

        std::size_t readPos = read_pos_.load(std::memory_order_acquire);
        std::size_t writePos = write_pos_.load(std::memory_order_acquire);
        readPos = normalizePos(readPos);

        if (readPos == writePos)
            return false;

        if (readPos + kHeaderSize > capacity_)
        {
            readPos = 0U;
        }

        uint32_t len = readLength(readPos);
        if (len == 0U || len > maxPayloadSize())
        {
            read_pos_.store(writePos, std::memory_order_release);
            return false;
        }

        if (readPos + kHeaderSize + len > capacity_)
        {
            readPos = 0U;
            if (readPos == writePos)
            {
                read_pos_.store(readPos, std::memory_order_release);
                return false;
            }
            len = readLength(readPos);
            if (len == 0U || len > maxPayloadSize())
            {
                read_pos_.store(writePos, std::memory_order_release);
                return false;
            }
        }

        out.assign(buffer_.begin() + static_cast<std::ptrdiff_t>(readPos + kHeaderSize),
                   buffer_.begin() + static_cast<std::ptrdiff_t>(readPos + kHeaderSize + len));

        std::size_t newRead = readPos + kHeaderSize + len;
        if (newRead >= capacity_)
        {
            newRead = 0U;
        }

        read_pos_.store(newRead, std::memory_order_release);
        return true;
    }

    /// @}

    // =========================================================================
    /// @name Synchronization
    // =========================================================================
    /// @{

    /**
     * @brief Wakes all threads waiting on the buffer.
     *
     * @details Used during shutdown to unblock consumers waiting in
     * waitRead(). Should be called after setting stop flags.
     *
     * @par Thread Safety
     * Safe to call from any thread.
     */
    void notifyAll()
    {
        cv_.notify_all();
    }

    /// @}

    // =========================================================================
    /// @name Overflow Detection
    // =========================================================================
    /// @{

    /**
     * @brief Checks if data was dropped due to buffer overflow.
     *
     * @retval true  At least one payload was dropped since construction
     *               or last call to clearOverwriteFlag().
     * @retval false No overflow has occurred.
     *
     * @par Thread Safety
     * Safe to call from any thread (relaxed memory order).
     */
    bool overwriteOccurred() const
    {
        return overwrite_flag_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Resets the overwrite detection flag.
     *
     * @post overwriteOccurred() returns false until next overflow.
     *
     * @par Thread Safety
     * Typically called from producer thread after handling overflow.
     */
    void clearOverwriteFlag()
    {
        overwrite_flag_.store(false, std::memory_order_relaxed);
    }

    /// @}

private:
    // =========================================================================
    /// @name Constants
    // =========================================================================
    /// @{

    /** @brief Size of the length header prefix (4 bytes, little-endian). */
    static constexpr std::size_t kHeaderSize = 4U;

    /// @}

    // =========================================================================
    /// @name Member Variables
    // =========================================================================
    /// @{

    // =========================================================================
    /// @name Member Variables
    // =========================================================================
    /// @{

    std::size_t capacity_;                     ///< Total buffer capacity in bytes.
    std::vector<uint8_t> buffer_;              ///< Contiguous storage for payloads.
    std::atomic<std::size_t> write_pos_{0U};   ///< Next write position (producer).
    std::atomic<std::size_t> read_pos_{0U};    ///< Next read position (consumer).
    std::atomic<bool> overwrite_flag_{false};  ///< True if overflow dropped data.
    std::mutex cv_mutex_;                      ///< Mutex protecting condition variable.
    std::condition_variable cv_;               ///< Signals data availability.

    /// @}

    // =========================================================================
    /// @name Private Helper Methods
    // =========================================================================
    /// @{

    /**
     * @brief Calculates maximum payload size.
     * @return capacity_ - kHeaderSize, or 0 if capacity is too small.
     */
    std::size_t maxPayloadSize() const
    {
        return capacity_ > kHeaderSize ? capacity_ - kHeaderSize : 0U;
    }

    /**
     * @brief Normalizes position to valid buffer range.
     * @param pos Position to normalize.
     * @return Normalized position (0 if near end of buffer).
     */
    std::size_t normalizePos(std::size_t pos) const
    {
        if (pos > capacity_ - kHeaderSize)
            return 0U;
        return pos;
    }

    /**
     * @brief Calculates bytes currently occupied in buffer.
     * @param readPos  Current read position.
     * @param writePos Current write position.
     * @return Number of bytes containing valid data.
     */
    std::size_t usedBytes(std::size_t readPos, std::size_t writePos) const
    {
        if (writePos >= readPos)
            return writePos - readPos;
        return capacity_ - (readPos - writePos);
    }

    /**
     * @brief Calculates bytes available for writing.
     * @param readPos  Current read position.
     * @param writePos Current write position.
     * @return Number of free bytes in buffer.
     */
    std::size_t freeBytes(std::size_t readPos, std::size_t writePos) const
    {
        return capacity_ - usedBytes(readPos, writePos);
    }

    /**
     * @brief Drops the oldest payload to make room for new data.
     *
     * @param[in,out] readPos  Read position (updated on success).
     * @param[in]     writePos Current write position.
     *
     * @retval true  Successfully dropped one payload.
     * @retval false Buffer is empty; nothing to drop.
     */
    bool dropOldest(std::size_t& readPos, std::size_t writePos)
    {
        if (readPos == writePos)
            return false;

        if (readPos > capacity_ - kHeaderSize)
        {
            readPos = 0U;
            if (readPos == writePos)
                return false;
        }

        uint32_t len = readLength(readPos);
        if (len == 0U || len > maxPayloadSize())
        {
            readPos = writePos;
            return true;
        }

        std::size_t newRead = readPos + kHeaderSize + len;
        if (newRead >= capacity_)
        {
            newRead = 0U;
        }
        readPos = newRead;
        return true;
    }

    /**
     * @brief Reads 4-byte length header (little-endian).
     * @param pos Buffer position to read from.
     * @return Decoded 32-bit length value.
     */
    uint32_t readLength(std::size_t pos) const
    {
        uint32_t value = 0U;
        value |= static_cast<uint32_t>(buffer_[pos]);
        value |= static_cast<uint32_t>(buffer_[pos + 1U]) << 8U;
        value |= static_cast<uint32_t>(buffer_[pos + 2U]) << 16U;
        value |= static_cast<uint32_t>(buffer_[pos + 3U]) << 24U;
        return value;
    }

    /**
     * @brief Writes 4-byte length header (little-endian).
     * @param pos Buffer position to write to.
     * @param len 32-bit length value to encode.
     */
    void writeLength(std::size_t pos, uint32_t len)
    {
        buffer_[pos] = static_cast<uint8_t>(len & 0xFFU);
        buffer_[pos + 1U] = static_cast<uint8_t>((len >> 8U) & 0xFFU);
        buffer_[pos + 2U] = static_cast<uint8_t>((len >> 16U) & 0xFFU);
        buffer_[pos + 3U] = static_cast<uint8_t>((len >> 24U) & 0xFFU);
    }

    /// @}
};
