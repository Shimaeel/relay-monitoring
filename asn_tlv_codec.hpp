#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ser_record.hpp"

namespace asn_tlv
{
struct TlvInfo
{
    uint8_t tag = 0U;
    bool constructed = false;
    std::size_t valueStart = 0U;
    std::size_t length = 0U;
    std::size_t nextOffset = 0U;
};

inline bool readLength(const uint8_t* buffer, std::size_t size, std::size_t offset,
                       std::size_t& length, std::size_t& nextOffset, std::string* error)
{
    if (offset >= size)
    {
        if (error)
            *error = "Invalid BER length offset";
        return false;
    }

    uint8_t first = buffer[offset];
    if ((first & 0x80U) == 0U)
    {
        length = first;
        nextOffset = offset + 1U;
        return true;
    }

    uint8_t count = first & 0x7FU;
    if (count == 0U || offset + count >= size)
    {
        if (error)
            *error = "Invalid BER length encoding";
        return false;
    }

    std::size_t value = 0U;
    for (uint8_t i = 0U; i < count; ++i)
    {
        value = (value << 8U) | buffer[offset + 1U + i];
    }

    length = value;
    nextOffset = offset + 1U + count;
    return true;
}

inline bool readTlv(const uint8_t* buffer, std::size_t size, std::size_t offset,
                    TlvInfo& info, std::string* error)
{
    if (offset >= size)
    {
        if (error)
            *error = "Invalid TLV offset";
        return false;
    }

    info.tag = buffer[offset];
    info.constructed = (info.tag & 0x20U) != 0U;

    std::size_t length = 0U;
    std::size_t nextOffset = 0U;
    if (!readLength(buffer, size, offset + 1U, length, nextOffset, error))
        return false;

    if (nextOffset + length > size)
    {
        if (error)
            *error = "TLV length exceeds buffer";
        return false;
    }

    info.valueStart = nextOffset;
    info.length = length;
    info.nextOffset = nextOffset + length;
    return true;
}

inline void berAppendLength(std::vector<uint8_t>& out, std::size_t len)
{
    if (len < 128U)
    {
        out.push_back(static_cast<uint8_t>(len));
        return;
    }

    std::size_t tmp = len;
    uint8_t buf[8] = {0};
    std::size_t count = 0U;

    while (tmp > 0U && count < sizeof(buf))
    {
        buf[count++] = static_cast<uint8_t>(tmp & 0xFFU);
        tmp >>= 8U;
    }

    out.push_back(static_cast<uint8_t>(0x80U | count));
    while (count > 0U)
    {
        out.push_back(buf[count - 1U]);
        --count;
    }
}

inline void berAppendTlv(std::vector<uint8_t>& out, uint8_t tag, const uint8_t* value, std::size_t len)
{
    out.push_back(tag);
    berAppendLength(out, len);
    if (len > 0U && value != nullptr)
    {
        out.insert(out.end(), value, value + len);
    }
}

inline void berAppendString(std::vector<uint8_t>& out, uint8_t tag, const std::string& value)
{
    berAppendTlv(out, tag, reinterpret_cast<const uint8_t*>(value.data()), value.size());
}

inline std::vector<uint8_t> encodeSerRecordsToTlv(const std::vector<SERRecord>& records)
{
    std::vector<uint8_t> content;
    content.reserve(records.size() * 64U);

    for (const auto& rec : records)
    {
        std::vector<uint8_t> recordValue;
        berAppendString(recordValue, 0x80U, rec.record_id);
        berAppendString(recordValue, 0x81U, rec.timestamp);
        berAppendString(recordValue, 0x82U, rec.status);
        berAppendString(recordValue, 0x83U, rec.description);

        std::vector<uint8_t> recordTlv;
        berAppendTlv(recordTlv, 0x30U, recordValue.data(), recordValue.size());
        content.insert(content.end(), recordTlv.begin(), recordTlv.end());
    }

    std::vector<uint8_t> payload;
    berAppendTlv(payload, 0x61U, content.data(), content.size());
    return payload;
}

inline bool decodeSerRecordsFromTlv(const uint8_t* data, std::size_t size,
                                    std::vector<SERRecord>& out, std::string* error = nullptr)
{
    out.clear();
    if (data == nullptr || size == 0U)
    {
        if (error)
            *error = "Empty TLV buffer";
        return false;
    }

    std::size_t offset = 0U;
    TlvInfo top;
    if (!readTlv(data, size, offset, top, error))
        return false;

    if (top.tag != 0x61U)
    {
        if (error)
            *error = "Unexpected TLV tag";
        return false;
    }

    offset = top.valueStart;
    std::size_t end = top.valueStart + top.length;

    while (offset < end)
    {
        TlvInfo record;
        if (!readTlv(data, size, offset, record, error))
            return false;

        if (record.tag != 0x30U)
        {
            offset = record.nextOffset;
            continue;
        }

        SERRecord rec;
        std::size_t fieldOffset = record.valueStart;
        std::size_t recordEnd = record.valueStart + record.length;

        while (fieldOffset < recordEnd)
        {
            TlvInfo field;
            if (!readTlv(data, size, fieldOffset, field, error))
                return false;

            std::string value(reinterpret_cast<const char*>(data + field.valueStart), field.length);

            if (field.tag == 0x80U)
                rec.record_id = value;
            else if (field.tag == 0x81U)
                rec.timestamp = value;
            else if (field.tag == 0x82U)
                rec.status = value;
            else if (field.tag == 0x83U)
                rec.description = value;

            fieldOffset = field.nextOffset;
        }

        out.push_back(rec);
        offset = record.nextOffset;
    }

    return true;
}
}  // namespace asn_tlv
