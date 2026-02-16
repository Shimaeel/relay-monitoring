// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

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

class SharedRingBuffer
{
public:
    enum class OpenMode
    {
        CreateOnly,
        OpenOnly,
        OpenOrCreate
    };

    explicit SharedRingBuffer(const std::string& name,
                              std::size_t capacity_bytes,
                              OpenMode mode = OpenMode::OpenOrCreate,
                              bool remove_on_destroy = false);

    ~SharedRingBuffer();

    static bool remove(const std::string& name);

    std::size_t capacity() const
    {
        return header_->capacity;
    }

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

    void notifyAll()
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        header_->not_empty_cv.notify_all();
    }

    bool overwriteOccurred() const
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        return header_->overwrite_flag;
    }

    void clearOverwriteFlag()
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        header_->overwrite_flag = false;
    }

    uint64_t dropCount() const
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        return header_->drop_count;
    }

    uint64_t totalWrites() const
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        return header_->total_writes;
    }

    uint64_t totalReads() const
    {
        boost::interprocess::scoped_lock<boost::interprocess::interprocess_mutex> lock(header_->mutex);
        return header_->total_reads;
    }

private:
    static constexpr std::size_t kHeaderSize = 4U;

    struct SharedHeader
    {
        uint32_t magic = 0U;
        std::size_t capacity = 0U;
        std::size_t write_pos = 0U;
        std::size_t read_pos = 0U;
        bool overwrite_flag = false;
        uint64_t drop_count = 0U;
        uint64_t total_writes = 0U;
        uint64_t total_reads = 0U;
        boost::interprocess::interprocess_mutex mutex;
        boost::interprocess::interprocess_condition not_empty_cv;
    };

    static constexpr uint32_t kMagic = 0x53524E47U;  // "SRNG"

    std::string name_;
    bool remove_on_destroy_ = false;
    boost::interprocess::shared_memory_object shm_;
    boost::interprocess::mapped_region region_;
    SharedHeader* header_ = nullptr;
    uint8_t* buffer_ = nullptr;

    std::size_t maxPayloadSize() const
    {
        return header_->capacity > kHeaderSize ? header_->capacity - kHeaderSize : 0U;
    }

    std::size_t normalizePos(std::size_t pos) const
    {
        if (pos > header_->capacity - kHeaderSize)
            return 0U;
        return pos;
    }

    std::size_t usedBytes(std::size_t readPos, std::size_t writePos) const
    {
        if (writePos >= readPos)
            return writePos - readPos;
        return header_->capacity - (readPos - writePos);
    }

    std::size_t freeBytes(std::size_t readPos, std::size_t writePos) const
    {
        return header_->capacity - usedBytes(readPos, writePos);
    }

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

    uint32_t readLength(std::size_t pos) const
    {
        uint32_t value = 0U;
        value |= static_cast<uint32_t>(buffer_[pos]);
        value |= static_cast<uint32_t>(buffer_[pos + 1U]) << 8U;
        value |= static_cast<uint32_t>(buffer_[pos + 2U]) << 16U;
        value |= static_cast<uint32_t>(buffer_[pos + 3U]) << 24U;
        return value;
    }

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
