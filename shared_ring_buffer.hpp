#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

class SharedRingBuffer
{
public:
    explicit SharedRingBuffer(std::size_t capacity_bytes)
        : capacity_(capacity_bytes), buffer_(capacity_bytes, 0U)
    {
    }

    std::size_t capacity() const
    {
        return capacity_;
    }

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

    void notifyAll()
    {
        cv_.notify_all();
    }

    bool overwriteOccurred() const
    {
        return overwrite_flag_.load(std::memory_order_relaxed);
    }

    void clearOverwriteFlag()
    {
        overwrite_flag_.store(false, std::memory_order_relaxed);
    }

private:
    static constexpr std::size_t kHeaderSize = 4U;

    std::size_t capacity_;
    std::vector<uint8_t> buffer_;
    std::atomic<std::size_t> write_pos_{0U};
    std::atomic<std::size_t> read_pos_{0U};
    std::atomic<bool> overwrite_flag_{false};
    std::mutex cv_mutex_;
    std::condition_variable cv_;

    std::size_t maxPayloadSize() const
    {
        return capacity_ > kHeaderSize ? capacity_ - kHeaderSize : 0U;
    }

    std::size_t normalizePos(std::size_t pos) const
    {
        if (pos > capacity_ - kHeaderSize)
            return 0U;
        return pos;
    }

    std::size_t usedBytes(std::size_t readPos, std::size_t writePos) const
    {
        if (writePos >= readPos)
            return writePos - readPos;
        return capacity_ - (readPos - writePos);
    }

    std::size_t freeBytes(std::size_t readPos, std::size_t writePos) const
    {
        return capacity_ - usedBytes(readPos, writePos);
    }

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
