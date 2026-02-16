// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file shared_db_memory.hpp
 * @brief Interprocess shared-memory state block with MPMC mutex for multiple writers and readers.
 *
 * @details Provides a fixed-capacity shared-memory region protected by an
 * interprocess mutex and condition variable.  Multiple writers (across threads
 * or processes) can update the region, and multiple readers can observe the
 * latest state or block until a new version arrives.
 *
 * Unlike SharedRingBuffer (a FIFO queue), SharedDBMemory is a *latest-state*
 * block: each write replaces the previous payload.  This is ideal for
 * publishing the most recent database snapshot, configuration, or status.
 *
 * @section features Feature Summary
 *
 * | Feature | Description |
 * |---------|-------------|
 * | **MPMC** | Any number of writers and readers, all serialised by one mutex |
 * | **Version counter** | Monotonically increasing; readers detect stale data |
 * | **Blocking read** | `waitForUpdate()` sleeps until a new version is posted |
 * | **Non-blocking read** | `tryRead()` / `read()` return immediately |
 * | **Binary-safe** | Arbitrary byte payloads up to the configured capacity |
 *
 * @section memory_layout Memory Layout
 *
 * ```
 * +──────────────────────────────────────────────────────────+
 * | SharedHeader (magic, version, counts, mutex, cv, ...)   |
 * +──────────────────────────────────────────────────────────+
 * | data buffer  [0 .. capacity)                            |
 * +──────────────────────────────────────────────────────────+
 * ```
 *
 * @section thread_safety Thread / Process Safety
 *
 * All public methods acquire the interprocess mutex internally.
 * No external locking is required.
 *
 * - **Writers**: Multiple threads/processes may call write()
 * - **Readers**: Multiple threads/processes may call read() / tryRead() / waitForUpdate()
 * - **Shutdown**: Any thread/process may call notifyAll()
 *
 * @section example Example Usage
 *
 * ```cpp
 * // Process A — writer
 * SharedDBMemory shm("DbNotify", 128 * 1024);
 * std::string msg = R"({"table":"ser_records","changes":1})";
 * shm.write(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
 *
 * // Process B — blocking reader
 * SharedDBMemory shm("DbNotify", 128 * 1024);
 * std::vector<uint8_t> buf;
 * std::atomic<bool> stop{false};
 * while (!stop.load()) {
 *     if (shm.waitForUpdate(buf, stop)) {
 *         // process buf ...
 *     }
 * }
 *
 * // Shutdown
 * stop.store(true);
 * shm.notifyAll();
 * ```
 *
 * @see shared_ring_buffer.hpp  Ring-buffer variant for streaming payloads
 * @see ws_db_server.hpp        WebSocket API that optionally publishes change
 *                              notifications via SharedDBMemory
 *
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

#include "../dll_export.hpp"

#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

/**
 * @class SharedDBMemory
 * @brief Shared-memory block with interprocess mutex for multiple writers and readers.
 *
 * @details Provides a fixed-capacity shared-memory region where:
 * - **Any number of writers** (threads or processes) can post new data,
 *   each write serialised by an interprocess mutex.
 * - **Any number of readers** can observe the latest data or block until
 *   a new version is published (condition variable).
 *
 * Unlike SharedRingBuffer (a FIFO queue), SharedDBMemory is a *latest-state*
 * block: each write replaces the previous payload.  This is ideal for
 * publishing the most recent database snapshot, configuration, or status.
 *
 * @par Thread / Process Safety
 * All public methods acquire the interprocess mutex internally.
 * No external locking is required.
 *
 * @invariant capacity() > 0
 * @invariant dataLength() <= capacity()
 */
class TELNET_SML_API SharedDBMemory
{
public:
    /// Controls shared-memory create/open semantics.
    enum class OpenMode
    {
        CreateOnly,    ///< Fail if segment already exists
        OpenOnly,      ///< Fail if segment does not exist
        OpenOrCreate   ///< Create if absent, open if present (default)
    };

    // =========================================================================
    /// @name Construction / Destruction
    // =========================================================================
    /// @{

    /**
     * @brief Creates or opens a shared-memory state block.
     *
     * @param name           Shared memory segment name (OS-global).
     * @param capacity_bytes Maximum payload size in bytes.
     * @param mode           Create/open behaviour.
     * @param remove_on_destroy If true, remove the segment in destructor.
     *
     * @throws std::invalid_argument if name is empty or capacity is 0.
     * @throws std::runtime_error    on OS-level shared memory failure.
     */
    explicit SharedDBMemory(const std::string& name,
                            std::size_t capacity_bytes,
                            OpenMode mode = OpenMode::OpenOrCreate,
                            bool remove_on_destroy = false);

    ~SharedDBMemory();

    SharedDBMemory(const SharedDBMemory&)            = delete;
    SharedDBMemory& operator=(const SharedDBMemory&) = delete;

    /// @brief Remove a shared memory segment by name.
    static bool remove(const std::string& name);

    /// @}

    // =========================================================================
    /// @name Writer Interface (MPMC-safe)
    // =========================================================================
    /// @{

    /**
     * @brief Writes a payload, replacing any previous data.
     *
     * @param data Pointer to payload bytes.
     * @param len  Payload length in bytes.
     *
     * @retval true  Write succeeded.
     * @retval false data==nullptr, len==0, or len exceeds capacity.
     *
     * @post version() is incremented.
     * @post All threads blocked in waitForUpdate() are woken.
     *
     * @par Thread Safety
     * Safe for concurrent calls from any number of threads/processes.
     */
    bool write(const uint8_t* data, std::size_t len)
    {
        if (!data || len == 0 || len > header_->capacity)
            return false;

        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex>
            lock(header_->mutex);

        std::memcpy(buffer_, data, len);
        header_->data_len = len;
        ++header_->version;
        ++header_->total_writes;

        header_->cv.notify_all();
        return true;
    }

    /**
     * @brief Convenience overload: write a std::string payload.
     */
    bool write(const std::string& payload)
    {
        return write(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    }

    /// @}

    // =========================================================================
    /// @name Reader Interface (MPMC-safe)
    // =========================================================================
    /// @{

    /**
     * @brief Non-blocking read of the latest payload.
     *
     * @param[out] out     Vector receives a copy of the current data.
     * @param[out] ver     (optional) Receives the current version number.
     *
     * @retval true  Data was available (out is filled).
     * @retval false No data has been written yet.
     */
    bool read(std::vector<uint8_t>& out, uint64_t* ver = nullptr) const
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex>
            lock(header_->mutex);

        if (header_->data_len == 0)
            return false;

        out.assign(buffer_, buffer_ + header_->data_len);
        ++header_->total_reads;
        if (ver) *ver = header_->version;
        return true;
    }

    /**
     * @brief Non-blocking read returning a string.
     */
    std::string readString() const
    {
        std::vector<uint8_t> buf;
        if (!read(buf)) return {};
        return std::string(buf.begin(), buf.end());
    }

    /**
     * @brief Attempts a non-blocking read only if the version is newer.
     *
     * @param[out]    out         Receives data if version changed.
     * @param[in,out] knownVer   On entry: last version seen by caller.
     *                           On exit: current version (updated only on success).
     *
     * @retval true  A newer version was available; out + knownVer updated.
     * @retval false No change since knownVer.
     */
    bool tryRead(std::vector<uint8_t>& out, uint64_t& knownVer) const
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex>
            lock(header_->mutex);

        if (header_->version == knownVer || header_->data_len == 0)
            return false;

        out.assign(buffer_, buffer_ + header_->data_len);
        knownVer = header_->version;
        ++header_->total_reads;
        return true;
    }

    /**
     * @brief Blocking read — waits until a version newer than @p sinceVer arrives.
     *
     * @param[out] out       Receives the payload.
     * @param[in]  stop_flag Set to true externally to abort the wait.
     * @param[in]  sinceVer  Version already seen (default 0 = wait for first write).
     *
     * @retval true  New data is available in @p out.
     * @retval false Stopped (stop_flag was set).
     *
     * @par Thread Safety
     * Safe for multiple concurrent readers.
     */
    bool waitForUpdate(std::vector<uint8_t>& out,
                       std::atomic<bool>& stop_flag,
                       uint64_t sinceVer = 0)
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex>
            lock(header_->mutex);

        header_->cv.wait(lock, [&]() {
            return stop_flag.load(std::memory_order_relaxed)
                || header_->version > sinceVer;
        });

        if (stop_flag.load(std::memory_order_relaxed))
            return false;

        if (header_->data_len == 0)
            return false;

        out.assign(buffer_, buffer_ + header_->data_len);
        ++header_->total_reads;
        return true;
    }

    /// @}

    // =========================================================================
    /// @name Synchronization
    // =========================================================================
    /// @{

    /**
     * @brief Wakes all threads blocked in waitForUpdate().
     *
     * @details Call this after setting stop flags so consumers unblock.
     */
    void notifyAll()
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex>
            lock(header_->mutex);
        header_->cv.notify_all();
    }

    /// @}

    // =========================================================================
    /// @name Introspection
    // =========================================================================
    /// @{

    /** @brief Current monotonic version (incremented on each write). */
    uint64_t version() const
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex>
            lock(header_->mutex);
        return header_->version;
    }

    /** @brief Total successful writes across all producers. */
    uint64_t totalWrites() const
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex>
            lock(header_->mutex);
        return header_->total_writes;
    }

    /** @brief Total successful reads across all consumers. */
    uint64_t totalReads() const
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex>
            lock(header_->mutex);
        return header_->total_reads;
    }

    /** @brief Maximum payload capacity in bytes. */
    std::size_t capacity() const
    {
        return header_->capacity;
    }

    /** @brief Current payload length in bytes (0 if never written). */
    std::size_t dataLength() const
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex>
            lock(header_->mutex);
        return header_->data_len;
    }

    /// @}

private:
    // =========================================================================
    /// @name Internal Types
    // =========================================================================
    /// @{

    /**
     * @brief Layout of the control block stored at the start of the segment.
     *
     * @details All fields protected by @c mutex.  Constructed in-place via
     * placement-new when the segment is first created (detected by @c magic).
     */
    struct SharedHeader
    {
        uint32_t    magic         = 0U;         ///< Initialization sentinel
        std::size_t capacity      = 0U;         ///< Max payload bytes
        std::size_t data_len      = 0U;         ///< Current payload bytes
        uint64_t    version       = 0U;         ///< Monotonic write counter
        uint64_t    total_writes  = 0U;         ///< Cumulative writes
        mutable uint64_t total_reads = 0U;      ///< Cumulative reads
        mutable boost::interprocess::interprocess_mutex     mutex;  ///< MPMC mutex
        boost::interprocess::interprocess_condition cv;     ///< Reader notification
    };

    static constexpr uint32_t kMagic = 0x44424D53U;  // "DBMS"

    /// @}

    // =========================================================================
    /// @name Member Variables
    // =========================================================================
    /// @{

    std::string name_;                                  ///< Shared memory segment name
    bool remove_on_destroy_ = false;                    ///< Remove segment on destruction
    boost::interprocess::shared_memory_object shm_;     ///< OS shared memory object
    boost::interprocess::mapped_region        region_;  ///< Mapped memory region
    SharedHeader* header_ = nullptr;                    ///< Pointer to control block
    uint8_t*      buffer_ = nullptr;                    ///< Pointer to data region

    /// @}
};

// ═══════════════════════════════════════════════════════════════════════════
//  Inline Implementation
// ═══════════════════════════════════════════════════════════════════════════

inline SharedDBMemory::SharedDBMemory(const std::string& name,
                                      std::size_t capacity_bytes,
                                      OpenMode mode,
                                      bool remove_on_destroy)
    : name_(name), remove_on_destroy_(remove_on_destroy)
{
    if (name_.empty())
        throw std::invalid_argument("SharedDBMemory name is empty");
    if (capacity_bytes == 0)
        throw std::invalid_argument("SharedDBMemory capacity is 0");

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
        throw std::runtime_error("SharedDBMemory region too small");

    header_ = static_cast<SharedHeader*>(region_.get_address());
    buffer_ = reinterpret_cast<uint8_t*>(header_ + 1U);

    const bool needs_init = (header_->magic != kMagic);
    if (needs_init)
    {
        header_ = new (region_.get_address()) SharedHeader();
        header_->magic        = kMagic;
        header_->capacity     = capacity_bytes;
        header_->data_len     = 0U;
        header_->version      = 0U;
        header_->total_writes = 0U;
        header_->total_reads  = 0U;
        buffer_ = reinterpret_cast<uint8_t*>(header_ + 1U);
        std::memset(buffer_, 0, capacity_bytes);
    }
    else if (header_->capacity != capacity_bytes)
    {
        throw std::runtime_error("SharedDBMemory capacity mismatch");
    }
}

inline SharedDBMemory::~SharedDBMemory()
{
    if (remove_on_destroy_ && !name_.empty())
        boost::interprocess::shared_memory_object::remove(name_.c_str());
}

inline bool SharedDBMemory::remove(const std::string& name)
{
    return boost::interprocess::shared_memory_object::remove(name.c_str());
}
