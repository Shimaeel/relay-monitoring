// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file shared_ring_buffer.hpp
 * @brief MPMC (Multiple-Producer Multiple-Consumer) ring buffer over shared memory.
 *
 * @details Thread-safe ring buffer that supports multiple concurrent writers and
 * multiple independent readers (broadcast/pub-sub pattern). Each registered reader
 * receives every message written to the buffer.
 *
 * ## Key Features
 * - **Multiple Writers**: Any number of threads/processes can call write() safely
 * - **Multiple Readers**: Up to kMaxReaders independent consumers, each with own cursor
 * - **Broadcast**: Every registered reader sees every message (pub-sub pattern)
 * - **Backpressure**: Auto-drops oldest messages when buffer is full (advances slowest reader)
 * - **Cross-process**: Uses Boost.Interprocess shared memory, accessible across processes
 * - **Blocking read**: waitRead() sleeps efficiently via interprocess condition variable
 *
 * ## Usage
 * @code{.cpp}
 * // Producer side (multiple writers OK)
 * SharedRingBuffer ring("MyRing", 500U * 1024U);
 * ring.write(data, len);  // thread-safe, multiple writers OK
 *
 * // Consumer side (each consumer registers independently)
 * auto id = ring.registerReader();
 * std::vector<uint8_t> out;
 * std::atomic<bool> stop{false};
 * while (ring.waitRead(id, out, stop)) {
 *     // process out — every reader gets every message
 * }
 * ring.unregisterReader(id);
 * @endcode
 *
 * @see shared_db_memory.hpp Shared memory variant for structured database data
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
 * @brief Thread-safe MPMC ring buffer for variable-length binary payloads.
 *
 * @details Supports multiple producers writing concurrently and multiple consumers
 * each receiving every message (broadcast pattern). Uses a single interprocess mutex
 * for all synchronisation, suitable for both inter-thread and inter-process use.
 *
 * Each reader gets its own cursor via registerReader(). The writer is constrained
 * by the slowest active reader; when the buffer is full the oldest unread message
 * is dropped for the slowest reader(s) and an overwrite flag is set.
 */
class SharedRingBuffer
{
public:
    /// Maximum number of concurrent readers supported
    static constexpr uint32_t kMaxReaders = 8U;

    /// Opaque reader identifier returned by registerReader()
    using ReaderId = uint32_t;

    /// Sentinel value indicating an invalid / unregistered reader
    static constexpr ReaderId kInvalidReader = UINT32_MAX;

    /**
     * @enum OpenMode
     * @brief Controls how the shared-memory segment is opened.
     *
     * @details Determines whether the constructor creates a new segment,
     * opens an existing one, or does either depending on existence.
     *
     * @code{.cpp}
     * // Create a fresh ring, removing any previous one with the same name
     * SharedRingBuffer ring("MyRing", 64 * 1024, SharedRingBuffer::OpenMode::CreateOnly);
     *
     * // Attach to an existing ring created by another process
     * SharedRingBuffer ring2("MyRing", 0, SharedRingBuffer::OpenMode::OpenOnly);
     * @endcode
     */
    enum class OpenMode
    {
        CreateOnly,   ///< Remove any existing segment and create a new one.
        OpenOnly,     ///< Open an existing segment; throws if not found.
        OpenOrCreate  ///< Open if it exists, otherwise create a new one.
    };

    /**
     * @brief Construct and map a shared ring buffer.
     *
     * @details Creates or opens a named shared-memory segment, maps it into
     * the process address space, and initialises the header if the segment is
     * newly created (detected via a magic number check).
     *
     * @param name             Unique name for the shared-memory segment.
     * @param capacity_bytes   Usable data capacity in bytes (excluding internal header).
     * @param mode             How the segment is opened (default: OpenOrCreate).
     * @param remove_on_destroy If true, the segment is removed when this object is destroyed.
     *
     * @throws std::invalid_argument If @p name is empty or @p capacity_bytes is too small.
     * @throws std::runtime_error    If the segment exists but has a different capacity.
     *
     * @code{.cpp}
     * SharedRingBuffer ring("SER_Ring", 500U * 1024U,
     *                       SharedRingBuffer::OpenMode::CreateOnly, true);
     * @endcode
     */
    explicit SharedRingBuffer(const std::string& name,
                              std::size_t capacity_bytes,
                              OpenMode mode = OpenMode::OpenOrCreate,
                              bool remove_on_destroy = false);

    /**
     * @brief Destroy the ring buffer and optionally remove the shared segment.
     *
     * @details If @c remove_on_destroy was set in the constructor, the
     * underlying shared-memory object is unlinked so that subsequent
     * open attempts by other processes will fail.
     */
    ~SharedRingBuffer();

    /**
     * @brief Remove a named shared-memory segment.
     *
     * @details Unlinks the shared-memory object identified by @p name.
     * Useful for manual cleanup when the ring was not created with
     * @c remove_on_destroy.
     *
     * @param name The segment name passed to the constructor.
     * @return true if the segment was removed, false otherwise.
     *
     * @code{.cpp}
     * SharedRingBuffer::remove("SER_Ring");  // clean up from a previous run
     * @endcode
     */
    static bool remove(const std::string& name);

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
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        for (uint32_t i = 0U; i < kMaxReaders; ++i)
        {
            if (!header_->readers[i].active)
            {
                header_->readers[i].active      = true;
                header_->readers[i].read_pos    = header_->write_pos;
                header_->readers[i].total_reads = 0U;
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
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        header_->readers[id].active = false;
    }

    // ─── Capacity ────────────────────────────────────────────────────

    /**
     * @brief Return the usable data capacity of the ring buffer in bytes.
     *
     * @details This is the raw payload area, excluding the internal shared-memory
     * header. Individual messages consume their payload size plus a 4-byte
     * length prefix.
     *
     * @return Data capacity in bytes.
     *
     * @code{.cpp}
     * SharedRingBuffer ring("Test", 1024);
     * assert(ring.capacity() == 1024);  // usable payload area
     * @endcode
     */
    std::size_t capacity() const
    {
        return header_->capacity;
    }

    // ─── Write (MPMC-safe) ───────────────────────────────────────────

    /**
     * @brief Write a variable-length payload into the ring buffer.
     *
     * @details Multiple threads/processes may call write() concurrently.
     * If the buffer is full (constrained by the slowest reader), the oldest
     * message is dropped to make room. All readers at that oldest position
     * are advanced past the dropped message.
     *
     * @param data Pointer to payload bytes.
     * @param len  Number of bytes to write.
     * @return true on success, false if data is null, len is 0, or len exceeds capacity.
     * @note Thread-safe.
     */
    bool write(const uint8_t* data, std::size_t len)
    {
        if (data == nullptr || len == 0U)
            return false;

        if (len > maxPayloadSize())
            return false;

        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        std::size_t writePos = normalizePos(header_->write_pos);

        // Wrap to start if message won't fit at end of buffer
        if (writePos + kHeaderSize + len > header_->capacity)
        {
            writePos = 0U;
        }

        // Calculate free space based on slowest active reader
        std::size_t slowReadPos = effectiveReadPos(writePos);
        std::size_t free = freeBytes(slowReadPos, writePos);

        while (free < kHeaderSize + len)
        {
            if (!dropOldestForSlowest(writePos))
            {
                return false;
            }
            header_->overwrite_flag = true;
            ++header_->drop_count;

            slowReadPos = effectiveReadPos(writePos);
            free = freeBytes(slowReadPos, writePos);
        }

        writeLength(writePos, static_cast<uint32_t>(len));
        std::memcpy(&buffer_[writePos + kHeaderSize], data, len);

        std::size_t newWrite = writePos + kHeaderSize + len;
        if (newWrite >= header_->capacity)
        {
            newWrite = 0U;
        }

        header_->write_pos = newWrite;
        ++header_->total_writes;

        // notify_all(): wake ALL waiting consumers so every reader sees the message.
        header_->not_empty_cv.notify_all();
        return true;
    }

    // ─── Read (per-reader cursor) ────────────────────────────────────

    /**
     * @brief Blocking read for a specific registered reader.
     *
     * @details Blocks until a payload is available for this reader or
     * stop_flag becomes true. Each reader maintains its own cursor and
     * receives every message independently.
     *
     * @param reader_id ReaderId from registerReader().
     * @param out       Output vector filled with payload bytes.
     * @param stop_flag Atomic flag; set to true to unblock and return false.
     * @return true if a payload was read, false on stop or error.
     * @note Thread-safe.
     */
    bool waitRead(ReaderId reader_id, std::vector<uint8_t>& out, std::atomic<bool>& stop_flag)
    {
        if (reader_id >= kMaxReaders)
            return false;

        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);

        ReaderSlot& reader = header_->readers[reader_id];
        if (!reader.active)
            return false;

        header_->not_empty_cv.wait(lock, [&]() {
            return stop_flag.load(std::memory_order_relaxed)
                || reader.read_pos != header_->write_pos;
        });

        if (stop_flag.load(std::memory_order_relaxed))
            return false;

        std::size_t readPos  = normalizePos(reader.read_pos);
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
            reader.read_pos = writePos;
            return false;
        }

        if (readPos + kHeaderSize + len > header_->capacity)
        {
            readPos = 0U;
            if (readPos == writePos)
            {
                reader.read_pos = readPos;
                return false;
            }
            len = readLength(readPos);
            if (len == 0U || len > maxPayloadSize())
            {
                reader.read_pos = writePos;
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

        reader.read_pos = newRead;
        ++reader.total_reads;
        return true;
    }

    // ─── Notification ────────────────────────────────────────────────

    /**
     * @brief Wake all threads blocked in waitRead().
     *
     * @details Typically called during shutdown so that consumers can
     * re-check their stop flag and exit cleanly.
     */
    void notifyAll()
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        header_->not_empty_cv.notify_all();
    }

    // ─── Statistics ──────────────────────────────────────────────────

    /**
     * @brief Check whether any message has been dropped due to buffer overflow.
     *
     * @details Returns true if write() had to advance a slow reader past an
     * unread message to make room. The flag is sticky — it stays true until
     * explicitly cleared with clearOverwriteFlag().
     *
     * @return true if at least one overwrite has occurred.
     * @note Thread-safe.
     *
     * @code{.cpp}
     * if (ring.overwriteOccurred()) {
     *     std::cerr << "Warning: " << ring.dropCount() << " messages dropped\n";
     *     ring.clearOverwriteFlag();
     * }
     * @endcode
     */
    bool overwriteOccurred() const
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        return header_->overwrite_flag;
    }

    /**
     * @brief Reset the overwrite-occurred flag to false.
     *
     * @details Call after logging or handling the overwrite condition.
     * @note Thread-safe.
     */
    void clearOverwriteFlag()
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        header_->overwrite_flag = false;
    }

    /**
     * @brief Return the total number of messages dropped since creation.
     *
     * @details Incremented each time write() has to discard the oldest
     * unread message for the slowest reader.
     *
     * @return Cumulative drop count.
     * @note Thread-safe.
     */
    uint64_t dropCount() const
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        return header_->drop_count;
    }

    /**
     * @brief Return the total number of messages successfully written.
     *
     * @details Monotonically increasing counter, useful for throughput monitoring.
     *
     * @return Cumulative write count.
     * @note Thread-safe.
     *
     * @code{.cpp}
     * std::cout << "Writes: " << ring.totalWrites()
     *           << "  Drops: " << ring.dropCount() << '\n';
     * @endcode
     */
    uint64_t totalWrites() const
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        return header_->total_writes;
    }

    /**
     * @brief Total reads performed by a specific reader.
     *
     * @param reader_id ReaderId to query.
     * @return Read count, or 0 if reader_id is invalid.
     */
    uint64_t totalReads(ReaderId reader_id) const
    {
        if (reader_id >= kMaxReaders)
            return 0U;
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        return header_->readers[reader_id].total_reads;
    }

    /**
     * @brief Number of currently registered (active) readers.
     */
    uint32_t activeReaderCount() const
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        uint32_t count = 0U;
        for (uint32_t i = 0U; i < kMaxReaders; ++i)
        {
            if (header_->readers[i].active)
                ++count;
        }
        return count;
    }

private:
    /// Size in bytes of the per-message length prefix
    static constexpr std::size_t kHeaderSize = 4U;

    /**
     * @brief Per-reader state stored in shared memory.
     */
    struct ReaderSlot
    {
        bool active          = false;   ///< true if this slot is in use
        std::size_t read_pos = 0U;      ///< This reader's current read cursor
        uint64_t total_reads = 0U;      ///< Number of messages read by this reader
    };

    /**
     * @brief Shared-memory header placed at the start of the mapped region.
     *
     * @details Contains all synchronisation primitives and per-reader state.
     * Must remain POD-compatible for placement new in shared memory.
     */
    struct SharedHeader
    {
        uint32_t magic          = 0U;
        std::size_t capacity    = 0U;
        std::size_t write_pos   = 0U;
        bool overwrite_flag     = false;
        uint64_t drop_count     = 0U;
        uint64_t total_writes   = 0U;
        ReaderSlot readers[kMaxReaders];
        boost::interprocess::interprocess_mutex mutex;
        boost::interprocess::interprocess_condition not_empty_cv;
    };

    /// Magic number "MRNG" — identifies MPMC ring format
    static constexpr uint32_t kMagic = 0x4D524E47U;

    std::string name_;
    bool remove_on_destroy_ = false;
    boost::interprocess::shared_memory_object shm_;
    boost::interprocess::mapped_region region_;
    SharedHeader* header_ = nullptr;
    uint8_t* buffer_      = nullptr;

    // ─── Internal Helpers ────────────────────────────────────────────

    /**
     * @brief Maximum payload size for a single message.
     *
     * @details Equals capacity minus the 4-byte length header. Any write
     * exceeding this limit is rejected.
     *
     * @return Max payload in bytes, or 0 if capacity is too small.
     */
    std::size_t maxPayloadSize() const
    {
        return header_->capacity > kHeaderSize ? header_->capacity - kHeaderSize : 0U;
    }

    /**
     * @brief Clamp a position to a valid buffer offset.
     *
     * @details If @p pos extends past the last possible message start
     * (capacity − 4), it wraps back to 0.
     *
     * @param pos Raw position value.
     * @return Normalised position within [0, capacity − kHeaderSize].
     */
    std::size_t normalizePos(std::size_t pos) const
    {
        if (pos > header_->capacity - kHeaderSize)
            return 0U;
        return pos;
    }

    /**
     * @brief Bytes occupied between a read position and the write position.
     *
     * @param readPos  Current reader cursor.
     * @param writePos Current writer cursor.
     * @return Number of bytes that have been written but not yet read.
     */
    std::size_t usedBytes(std::size_t readPos, std::size_t writePos) const
    {
        if (writePos >= readPos)
            return writePos - readPos;
        return header_->capacity - (readPos - writePos);
    }

    /**
     * @brief Available free space in the ring given a read and write position.
     *
     * @param readPos  Current reader cursor.
     * @param writePos Current writer cursor.
     * @return Free bytes available for new writes.
     */
    std::size_t freeBytes(std::size_t readPos, std::size_t writePos) const
    {
        return header_->capacity - usedBytes(readPos, writePos);
    }

    /**
     * @brief Find the effective read position (slowest active reader).
     *
     * @details Returns the read_pos of the active reader with the most unread
     * data. If no readers are active, returns writePos (all space is free).
     *
     * @param writePos Current (possibly wrapped) write position.
     * @return The read_pos that constrains the writer.
     * @pre Called with mutex held.
     */
    std::size_t effectiveReadPos(std::size_t writePos) const
    {
        std::size_t slowest  = writePos;
        std::size_t maxUsed  = 0U;
        bool anyActive       = false;

        for (uint32_t i = 0U; i < kMaxReaders; ++i)
        {
            if (!header_->readers[i].active)
                continue;
            anyActive = true;
            std::size_t rp   = normalizePos(header_->readers[i].read_pos);
            std::size_t used = usedBytes(rp, writePos);
            if (used > maxUsed)
            {
                maxUsed = used;
                slowest = rp;
            }
        }

        if (!anyActive)
            return writePos;

        return slowest;
    }

    /**
     * @brief Index of the slowest (most behind) active reader.
     *
     * @param writePos Current write position.
     * @return Reader index [0..kMaxReaders), or kMaxReaders if none active.
     * @pre Called with mutex held.
     */
    uint32_t slowestReaderIndex(std::size_t writePos) const
    {
        uint32_t    slowest = kMaxReaders;
        std::size_t maxUsed = 0U;

        for (uint32_t i = 0U; i < kMaxReaders; ++i)
        {
            if (!header_->readers[i].active)
                continue;
            std::size_t rp   = normalizePos(header_->readers[i].read_pos);
            std::size_t used = usedBytes(rp, writePos);
            if (slowest == kMaxReaders || used > maxUsed)
            {
                maxUsed = used;
                slowest = i;
            }
        }
        return slowest;
    }

    /**
     * @brief Drop the oldest message for the slowest reader(s).
     *
     * @details Finds all active readers at the same (slowest) position and
     * advances them past one message. This frees buffer space for new writes.
     *
     * @param writePos Current write position.
     * @return true if a message was dropped, false if nothing to drop.
     * @pre Called with mutex held.
     */
    bool dropOldestForSlowest(std::size_t writePos)
    {
        uint32_t si = slowestReaderIndex(writePos);
        if (si >= kMaxReaders)
            return false;   // No active readers

        std::size_t origPos = normalizePos(header_->readers[si].read_pos);
        if (origPos == writePos)
            return false;   // Buffer empty for this reader

        std::size_t readPos = origPos;
        if (readPos + kHeaderSize > header_->capacity)
        {
            readPos = 0U;
        }
        if (readPos == writePos)
            return false;

        uint32_t len = readLength(readPos);
        if (len == 0U || len > maxPayloadSize())
        {
            // Corrupt entry — skip all readers at this position to writePos
            for (uint32_t i = 0U; i < kMaxReaders; ++i)
            {
                if (header_->readers[i].active &&
                    normalizePos(header_->readers[i].read_pos) == origPos)
                {
                    header_->readers[i].read_pos = writePos;
                }
            }
            return true;
        }

        std::size_t skipTo = readPos + kHeaderSize + len;
        if (skipTo >= header_->capacity)
        {
            skipTo = 0U;
        }

        // Advance ALL readers that are at the same oldest position
        for (uint32_t i = 0U; i < kMaxReaders; ++i)
        {
            if (header_->readers[i].active)
            {
                std::size_t rp = normalizePos(header_->readers[i].read_pos);
                if (rp == origPos)
                    header_->readers[i].read_pos = skipTo;
            }
        }
        return true;
    }

    /**
     * @brief Read a little-endian 32-bit message length from the buffer.
     *
     * @details Decodes the 4-byte length prefix stored at @p pos.
     *
     * @param pos Byte offset within the data area.
     * @return Decoded payload length.
     * @pre @p pos + 4 ≤ capacity.
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
     * @brief Write a little-endian 32-bit message length into the buffer.
     *
     * @details Encodes @p len as a 4-byte prefix at @p pos, used by write()
     * before copying the payload bytes.
     *
     * @param pos Byte offset within the data area.
     * @param len Payload length to encode.
     * @pre @p pos + 4 ≤ capacity.
     */
    void writeLength(std::size_t pos, uint32_t len)
    {
        buffer_[pos] = static_cast<uint8_t>(len & 0xFFU);
        buffer_[pos + 1U] = static_cast<uint8_t>((len >> 8U) & 0xFFU);
        buffer_[pos + 2U] = static_cast<uint8_t>((len >> 16U) & 0xFFU);
        buffer_[pos + 3U] = static_cast<uint8_t>((len >> 24U) & 0xFFU);
    }
};

// ═══════════════════════════════════════════════════════════════════════
// Out-of-line constructor / destructor / static remove
// ═══════════════════════════════════════════════════════════════════════

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
        header_->magic          = kMagic;
        header_->capacity       = capacity_bytes;
        header_->write_pos      = 0U;
        header_->overwrite_flag = false;
        header_->drop_count     = 0U;
        header_->total_writes   = 0U;

        for (uint32_t i = 0U; i < kMaxReaders; ++i)
        {
            header_->readers[i].active      = false;
            header_->readers[i].read_pos    = 0U;
            header_->readers[i].total_reads = 0U;
        }

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
