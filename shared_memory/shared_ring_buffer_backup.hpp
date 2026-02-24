// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file shared_ring_buffer_backup.hpp
 * @brief Legacy single-consumer shared-memory ring buffer (SPSC backup).
 *
 * @details Implements a lock-based, inter-process ring buffer backed by
 * Boost.Interprocess shared memory. Data is stored as variable-length
 * payloads prefixed by a 4-byte little-endian length header.
 *
 * ## Design Overview
 *
 * - **Overwrite policy**: When the buffer is full, the oldest unread
 *   payload is silently dropped to make room for the new write.
 *   An overwrite flag and drop counter track lost data.
 * - **Blocking read**: waitRead() blocks the consumer on an
 *   interprocess condition variable until data is available.
 * - **Magic number**: A 4-byte magic (`0x53524E47` = "SRNG") in the
 *   shared header distinguishes initialised memory from stale data.
 *
 * ## Memory Layout
 *
 * ```
 * ┌──────────────────────────────┬──────────────────────────────────┐
 * │        SharedHeader          │         Ring Buffer Data         │
 * │  (mutex, CV, positions, …)   │  [len][payload][len][payload]…   │
 * └──────────────────────────────┴──────────────────────────────────┘
 * ```
 *
 * @note This is the **backup / legacy** version superseded by the
 *       fully-documented MPMC version in shared_ring_buffer.hpp.
 *
 * @see shared_ring_buffer.hpp  Current multi-consumer version.
 *
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>

/**
 * @class SharedRingBuffer
 * @brief Inter-process, lock-based, single-consumer ring buffer.
 *
 * @details Provides a bounded FIFO queue residing in POSIX / Windows
 * shared memory. A single mutex protects all read/write operations,
 * and a condition variable allows the consumer to block efficiently.
 *
 * ### Thread / Process Safety
 * - Multiple processes may hold a SharedRingBuffer instance mapped
 *   to the same shared-memory name.
 * - Write-side: multiple writers are safe (serialised by the mutex).
 * - Read-side: designed for a **single** consumer calling waitRead().
 *
 * @invariant header_->magic == kMagic after construction.
 * @invariant 0 <= read_pos, write_pos < capacity.
 */
class SharedRingBuffer
{
public:
    /**
     * @enum OpenMode
     * @brief Controls shared-memory creation semantics.
     */
    enum class OpenMode
    {
        CreateOnly,    ///< Create new shared memory; fail if it already exists.
        OpenOnly,      ///< Open existing shared memory; fail if it does not exist.
        OpenOrCreate   ///< Create if absent, open if already present (default).
    };

    /**
     * @brief Construct and map a shared-memory ring buffer.
     *
     * @details Depending on the OpenMode, either creates a new shared-memory
     *          segment or opens an existing one. If the header magic does not
     *          match, the segment is zero-initialised.
     *
     * @param name             Unique OS-level name for the shared-memory object.
     * @param capacity_bytes   Usable data capacity in bytes (excluding the header).
     * @param mode             Creation mode (default: OpenOrCreate).
     * @param remove_on_destroy If true, the shared-memory object is removed
     *                          from the OS when this instance is destroyed.
     *
     * @throws std::invalid_argument  If name is empty or capacity <= kHeaderSize.
     * @throws std::runtime_error     If the mapped region is too small or
     *                                capacity mismatches an existing segment.
     */
    explicit SharedRingBuffer(const std::string& name,
                              std::size_t capacity_bytes,
                              OpenMode mode = OpenMode::OpenOrCreate,
                              bool remove_on_destroy = false);

    /**
     * @brief Destroy the ring buffer and optionally remove shared memory.
     *
     * @details If remove_on_destroy was set to true in the constructor,
     *          the shared-memory object is deleted from the OS. Otherwise
     *          the memory persists for other processes.
     */
    ~SharedRingBuffer();

    /**
     * @brief Remove a named shared-memory object from the operating system.
     *
     * @param name  The OS-level shared-memory name to remove.
     *
     * @return true   Successfully removed.
     * @return false  Object did not exist or removal failed.
     */
    static bool remove(const std::string& name);

    /**
     * @brief Get the usable data capacity of the ring buffer.
     *
     * @return std::size_t  Buffer capacity in bytes (excluding internal header).
     */
    std::size_t capacity() const
    {
        return header_->capacity;
    }

    /**
     * @brief Write a variable-length payload into the ring buffer.
     *
     * @details Acquires the interprocess mutex, checks available space,
     *          and drops the oldest entry if necessary to make room.
     *          The payload is written as: [4-byte LE length][data bytes].
     *          After writing, wakes exactly one waiting consumer via
     *          notify_one() to avoid thundering-herd problems.
     *
     * @param data  Pointer to the raw bytes to write.
     * @param len   Number of bytes to write (must be <= maxPayloadSize()).
     *
     * @return true   Payload written successfully.
     * @return false  data is null, len is 0, or len exceeds capacity.
     *
     * @note If the buffer is full, the oldest unread payload is silently
     *       dropped and overwrite_flag / drop_count are updated.
     */
    bool write(const uint8_t* data, std::size_t len)
    {
        if (data == nullptr || len == 0U)
            return false;

        if (len > maxPayloadSize())
            return false;

        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        std::size_t readPos = normalizePos(header_->read_pos);
        std::size_t writePos = normalizePos(header_->write_pos);

        if (writePos + kHeaderSize + len > header_->capacity)
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
            header_->overwrite_flag = true;
            ++header_->drop_count;
            freeSpace = freeBytes(readPos, writePos);
        }

        writeLength(writePos, static_cast<uint32_t>(len));
        std::memcpy(&buffer_[writePos + kHeaderSize], data, len);

        std::size_t newWrite = writePos + kHeaderSize + len;
        if (newWrite >= header_->capacity)
        {
            newWrite = 0U;
        }

        header_->read_pos = readPos;
        header_->write_pos = newWrite;
        ++header_->total_writes;

        // notify_one(): wake exactly ONE waiting consumer per payload.
        // Avoids thundering-herd when multiple consumers block in waitRead().
        header_->not_empty_cv.notify_one();
        return true;
    }

    /**
     * @brief Block until a payload is available, then read it.
     *
     * @details Waits on the interprocess condition variable until either:
     *          - Data becomes available (read_pos != write_pos), or
     *          - The stop_flag is set externally (for graceful shutdown).
     *
     *          On success, the oldest unread payload is copied into `out`
     *          and the read position is advanced.
     *
     * @param[out] out        Vector that receives the payload bytes.
     *                        Resized automatically.
     * @param[in]  stop_flag  Atomic flag; set to true to abort the wait.
     *
     * @return true   Payload read successfully.
     * @return false  Stopped by stop_flag, buffer empty after wake,
     *                or corrupted length encountered.
     */
    bool waitRead(std::vector<uint8_t>& out, std::atomic<bool>& stop_flag)
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        header_->not_empty_cv.wait(lock, [&]() {
            return stop_flag.load(std::memory_order_relaxed)
                || header_->read_pos != header_->write_pos;
        });

        if (stop_flag.load(std::memory_order_relaxed))
            return false;

        std::size_t readPos = normalizePos(header_->read_pos);
        std::size_t writePos = normalizePos(header_->write_pos);

        if (readPos == writePos)
            return false;

        if (readPos + kHeaderSize > header_->capacity)
        {
            readPos = 0U;
        }

        uint32_t len = readLength(readPos);
        if (len == 0U || len > maxPayloadSize())
        {
            header_->read_pos = writePos;
            return false;
        }

        if (readPos + kHeaderSize + len > header_->capacity)
        {
            readPos = 0U;
            if (readPos == writePos)
            {
                header_->read_pos = readPos;
                return false;
            }
            len = readLength(readPos);
            if (len == 0U || len > maxPayloadSize())
            {
                header_->read_pos = writePos;
                return false;
            }
        }

        out.assign(buffer_ + readPos + kHeaderSize,
                   buffer_ + readPos + kHeaderSize + len);

        std::size_t newRead = readPos + kHeaderSize + len;
        if (newRead >= header_->capacity)
        {
            newRead = 0U;
        }

        header_->read_pos = newRead;
        ++header_->total_reads;
        return true;
    }

    /**
     * @brief Wake all threads / processes currently blocked in waitRead().
     *
     * @details Useful during shutdown to unblock consumers so they can
     *          check their stop_flag and exit cleanly.
     */
    void notifyAll()
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        header_->not_empty_cv.notify_all();
    }

    /**
     * @brief Check whether an overwrite has occurred since the last clear.
     *
     * @details Returns true if at least one old payload was dropped to
     *          make room for a new write. Use clearOverwriteFlag() to reset.
     *
     * @return true   At least one payload was overwritten.
     * @return false  No overwrites since last clear.
     */
    bool overwriteOccurred() const
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        return header_->overwrite_flag;
    }

    /**
     * @brief Reset the overwrite flag to false.
     *
     * @details Call after handling the overwrite condition.
     *          Does not reset the cumulative drop_count.
     */
    void clearOverwriteFlag()
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        header_->overwrite_flag = false;
    }

    /**
     * @brief Get the cumulative number of payloads dropped due to overwrite.
     *
     * @return uint64_t  Total drop count since buffer creation.
     */
    uint64_t dropCount() const
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        return header_->drop_count;
    }

    /**
     * @brief Get the total number of successful write operations.
     *
     * @return uint64_t  Cumulative write count since buffer creation.
     */
    uint64_t totalWrites() const
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        return header_->total_writes;
    }

    /**
     * @brief Get the total number of successful read operations.
     *
     * @return uint64_t  Cumulative read count since buffer creation.
     */
    uint64_t totalReads() const
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        return header_->total_reads;
    }

private:
    /** @brief Number of bytes used for the per-payload length prefix (4 bytes LE). */
    static constexpr std::size_t kHeaderSize = 4U;

    /**
     * @struct SharedHeader
     * @brief Control block stored at the start of the shared-memory segment.
     *
     * @details Contains synchronisation primitives (mutex + CV),
     *          ring buffer positions, and diagnostic counters.
     *          Placed via placement-new on first creation.
     */
    struct SharedHeader
    {
        uint32_t magic = 0U;                 ///< Magic number for initialisation detection (kMagic).
        std::size_t capacity = 0U;           ///< Usable data area size in bytes.
        std::size_t write_pos = 0U;          ///< Current write offset into the data area.
        std::size_t read_pos = 0U;           ///< Current read offset into the data area.
        bool overwrite_flag = false;         ///< Set when a payload is dropped to make room.
        uint64_t drop_count = 0U;            ///< Cumulative number of dropped payloads.
        uint64_t total_writes = 0U;          ///< Cumulative successful write count.
        uint64_t total_reads = 0U;           ///< Cumulative successful read count.
        boost::interprocess::interprocess_mutex mutex;         ///< Guards all shared state.
        boost::interprocess::interprocess_condition not_empty_cv; ///< Signalled on write.
    };

    static constexpr uint32_t kMagic = 0x53524E47U;  ///< "SRNG" — marks an initialised header.

    std::string name_;                                          ///< OS-level shared-memory name.
    bool remove_on_destroy_ = false;                            ///< Remove shm on destruction?
    boost::interprocess::shared_memory_object shm_;             ///< Boost.Interprocess shm handle.
    boost::interprocess::mapped_region region_;                 ///< Memory-mapped region.
    SharedHeader* header_ = nullptr;                            ///< Pointer to the control block.
    uint8_t* buffer_ = nullptr;                                 ///< Pointer to the data area.

    /**
     * @brief Maximum payload size a single write can accept.
     *
     * @return std::size_t  capacity minus the 4-byte length header.
     */
    std::size_t maxPayloadSize() const
    {
        return header_->capacity > kHeaderSize ? header_->capacity - kHeaderSize : 0U;
    }

    /**
     * @brief Clamp a position to a valid range within the data area.
     *
     * @param pos  Raw position value.
     * @return std::size_t  0 if pos overflows, otherwise pos unchanged.
     */
    std::size_t normalizePos(std::size_t pos) const
    {
        if (pos > header_->capacity - kHeaderSize)
            return 0U;
        return pos;
    }

    /**
     * @brief Calculate the number of occupied bytes between read and write.
     *
     * @param readPos   Current read position.
     * @param writePos  Current write position.
     * @return std::size_t  Number of bytes currently in use.
     */
    std::size_t usedBytes(std::size_t readPos, std::size_t writePos) const
    {
        if (writePos >= readPos)
            return writePos - readPos;
        return header_->capacity - (readPos - writePos);
    }

    /**
     * @brief Calculate the number of free bytes available for writing.
     *
     * @param readPos   Current read position.
     * @param writePos  Current write position.
     * @return std::size_t  Number of bytes available.
     */
    std::size_t freeBytes(std::size_t readPos, std::size_t writePos) const
    {
        return header_->capacity - usedBytes(readPos, writePos);
    }

    /**
     * @brief Drop the oldest unread payload to free space.
     *
     * @details Advances readPos past one [length][data] entry.
     *          Called by write() when the buffer is full.
     *
     * @param[in,out] readPos   Updated to skip the dropped payload.
     * @param[in]     writePos  Current write position (unchanged).
     *
     * @return true   An entry was dropped.
     * @return false  Buffer already empty (readPos == writePos).
     */
    bool dropOldest(std::size_t& readPos, std::size_t writePos)
    {
        if (readPos == writePos)
            return false;

        if (readPos > header_->capacity - kHeaderSize)
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
        if (newRead >= header_->capacity)
        {
            newRead = 0U;
        }
        readPos = newRead;
        return true;
    }

    /**
     * @brief Read the 4-byte little-endian payload length at a given position.
     *
     * @param pos  Byte offset into the data area.
     * @return uint32_t  Decoded length value.
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
     * @brief Write a 4-byte little-endian payload length at a given position.
     *
     * @param pos  Byte offset into the data area.
     * @param len  Length value to encode.
     */
    void writeLength(std::size_t pos, uint32_t len)
    {
        buffer_[pos] = static_cast<uint8_t>(len & 0xFFU);
        buffer_[pos + 1U] = static_cast<uint8_t>((len >> 8U) & 0xFFU);
        buffer_[pos + 2U] = static_cast<uint8_t>((len >> 16U) & 0xFFU);
        buffer_[pos + 3U] = static_cast<uint8_t>((len >> 24U) & 0xFFU);
    }
};

inline SharedRingBuffer::SharedRingBuffer(const std::string& name,
                                          std::size_t capacity_bytes,
                                          OpenMode mode,
                                          bool remove_on_destroy)
    : name_(name), remove_on_destroy_(remove_on_destroy)
{
    if (name_.empty())
        throw std::invalid_argument("SharedRingBuffer name is empty");

    const std::size_t total_size = sizeof(SharedHeader) + capacity_bytes;

    if (mode == OpenMode::CreateOnly)
    {
        boost::interprocess::shared_memory_object::remove(name_.c_str());
        shm_ = boost::interprocess::shared_memory_object(
            boost::interprocess::create_only, name_.c_str(), boost::interprocess::read_write);
        shm_.truncate(static_cast<boost::interprocess::offset_t>(total_size));
    }
    else if (mode == OpenMode::OpenOnly)
    {
        shm_ = boost::interprocess::shared_memory_object(
            boost::interprocess::open_only, name_.c_str(), boost::interprocess::read_write);
    }
    else
    {
        shm_ = boost::interprocess::shared_memory_object(
            boost::interprocess::open_or_create, name_.c_str(), boost::interprocess::read_write);
        shm_.truncate(static_cast<boost::interprocess::offset_t>(total_size));
    }

    region_ = boost::interprocess::mapped_region(shm_, boost::interprocess::read_write);
    if (region_.get_size() < sizeof(SharedHeader))
        throw std::runtime_error("SharedRingBuffer region too small");

    header_ = static_cast<SharedHeader*>(region_.get_address());
    buffer_ = reinterpret_cast<uint8_t*>(header_ + 1U);

    const bool needs_init = (header_->magic != kMagic);
    if (needs_init)
    {
        if (capacity_bytes <= kHeaderSize)
            throw std::invalid_argument("SharedRingBuffer capacity too small");

        header_ = new (region_.get_address()) SharedHeader();
        header_->magic = kMagic;
        header_->capacity = capacity_bytes;
        header_->write_pos = 0U;
        header_->read_pos = 0U;
        header_->overwrite_flag = false;
        header_->drop_count = 0U;
        header_->total_writes = 0U;
        header_->total_reads = 0U;
        buffer_ = reinterpret_cast<uint8_t*>(header_ + 1U);
        std::memset(buffer_, 0, capacity_bytes);
    }
    else if (capacity_bytes != 0U && header_->capacity != capacity_bytes)
    {
        throw std::runtime_error("SharedRingBuffer capacity mismatch");
    }
}

inline SharedRingBuffer::~SharedRingBuffer()
{
    if (remove_on_destroy_ && !name_.empty())
    {
        boost::interprocess::shared_memory_object::remove(name_.c_str());
    }
}

inline bool SharedRingBuffer::remove(const std::string& name)
{
    return boost::interprocess::shared_memory_object::remove(name.c_str());
}
