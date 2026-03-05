// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/**
 * @file asn_tlv_codec.hpp
 * @brief ASN.1 BER/TLV codec for SER record payloads.
 *
 * @details Provides small, header-only helpers to encode and decode SER records
 * using ASN.1 Basic Encoding Rules (BER) with a Tag-Length-Value (TLV) layout.
 * The output is a compact binary payload used for WebSocket transfer.
 *
 * ## TLV Structure
 *
 * ```
 * +--------+--------+------------------+
 * |  Tag   | Length |      Value       |
 * | 1 byte | 1-5 B  |  Length bytes    |
 * +--------+--------+------------------+
 * ```
 *
 * ## Payload Format
 *
 * Top-level TLV (APPLICATION 1, constructed):
 * ```
 * Tag: 0x61
 * Value: zero or more Record TLVs
 * ```
 *
 * Record TLV (SEQUENCE, constructed):
 * ```
 * Tag: 0x30
 * Value: context-specific primitive fields
 *   0x80: record_id (string)
 *   0x81: timestamp (string)
 *   0x82: status (string)
 *   0x83: description (string)
 * ```
 *
 * ## Data Flow
 *
 * @dot
 * digraph ASNCodec {
 *     rankdir=TB;
 *     node [shape=box, style=filled, fillcolor=lightyellow];
 *
 *     Records [label="vector<SERRecord>"];
 *     Encode [label="encodeSerRecordsToTlv()"];
 *     Binary [label="vector<uint8_t>\n(TLV payload)", fillcolor=lightgreen];
 *     Decode [label="decodeSerRecordsFromTlv()"];
 *     Output [label="vector<SERRecord>"];
 *
 *     Records -> Encode [label="serialize"];
 *     Encode -> Binary;
 *     Binary -> Decode [label="deserialize"];
 *     Decode -> Output;
 * }
 * @enddot
 *
 * ## Usage Example
 *
 * @code{.cpp}
 * // Encode
 * std::vector<SERRecord> records = getRecords();
 * auto payload = asn_tlv::encodeSerRecordsToTlv(records);
 * webSocket.send(payload);
 *
 * // Decode
 * std::vector<SERRecord> decoded;
 * std::string error;
 * if (asn_tlv::decodeSerRecordsFromTlv(data, size, decoded, &error)) {
 *     processRecords(decoded);
 * }
 * @endcode
 *
 * @see SERWebSocketServer Uses this codec for WebSocket messages
 * @see SharedSerReader Uses this codec for shared memory data
 *
 * @author Telnet-SML Development Team
 * @version 1.0.0
 * @date 2026
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ser_record.hpp"

/**
 * @namespace asn_tlv
 * @brief ASN.1 BER/TLV encoding and decoding utilities.
 */
namespace asn_tlv
{

/**
 * @struct TlvInfo
 * @brief Parsed TLV header information.
 *
 * @details Contains metadata extracted from a TLV structure.
 */
struct TlvInfo
{
    uint8_t tag = 0U;            ///< Tag byte (type identifier)
    bool constructed = false;    ///< true if constructed (contains nested TLVs)
    std::size_t valueStart = 0U; ///< Offset where value bytes begin
    std::size_t length = 0U;     ///< Length of value in bytes
    std::size_t nextOffset = 0U; ///< Offset of next TLV (after this one)
};

/**
 * @brief Read BER length encoding from a buffer.
 *
 * @details Handles both short form (single byte < 128) and long form
 * (first byte indicates count of following length bytes).
 *
 * @param buffer Input buffer
 * @param size Buffer size in bytes
 * @param offset Current position in buffer
 * @param[out] length Decoded length value
 * @param[out] nextOffset Position after length bytes
 * @param[out] error Optional error message on failure
 *
 * @return true if decoding succeeded
 * @return false if encoding is invalid or out of range
 */
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

/**
 * @brief Read and parse a complete TLV structure.
 *
 * @details Extracts tag, length, and value boundaries from buffer.
 *
 * @param buffer Input buffer
 * @param size Buffer size in bytes
 * @param offset Current position in buffer
 * @param[out] info Parsed TLV information
 * @param[out] error Optional error message on failure
 *
 * @return true if parsing succeeded
 * @return false if the TLV is invalid or out of range
 */
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

/**
 * @brief Append BER length encoding to an output buffer.
 *
 * @details Uses short form for lengths < 128, long form otherwise.
 *
 * @param[out] out Output buffer to append to
 * @param len Length value to encode
 */
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

/**
 * @brief Append a complete TLV structure to an output buffer.
 *
 * @param[out] out Output buffer to append to
 * @param tag Tag byte
 * @param value Pointer to value bytes (may be nullptr if len is 0)
 * @param len Length of value
 */
inline void berAppendTlv(std::vector<uint8_t>& out, uint8_t tag, const uint8_t* value, std::size_t len)
{
    out.push_back(tag);
    berAppendLength(out, len);
    if (len > 0U && value != nullptr)
    {
        out.insert(out.end(), value, value + len);
    }
}

/**
 * @brief Append a string as a TLV structure.
 *
 * @param[out] out Output buffer to append to
 * @param tag Tag byte
 * @param value String value to encode
 */
inline void berAppendString(std::vector<uint8_t>& out, uint8_t tag, const std::string& value)
{
    berAppendTlv(out, tag, reinterpret_cast<const uint8_t*>(value.data()), value.size());
}

/**
 * @brief Encode SER records to ASN.1 BER/TLV format.
 *
 * @details Creates a top-level APPLICATION 1 TLV containing zero or more
 * SEQUENCE TLVs, each with context-specific fields for record data.
 *
 * ## Output Structure
 * ```
 * 0x61 [len] {
 *   0x30 [len] {
 *     0x80 [len] record_id
 *     0x81 [len] timestamp
 *     0x82 [len] status
 *     0x83 [len] description
 *     0x84 [len] relay_id
 *     0x85 [len] relay_name
 *   }
 *   ... (more records)
 * }
 * ```
 *
 * @param records Vector of SERRecord to encode
 * @return std::vector<uint8_t> Binary TLV payload
 */
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
        berAppendString(recordValue, 0x84U, rec.relay_id);
        berAppendString(recordValue, 0x85U, rec.relay_name);

        std::vector<uint8_t> recordTlv;
        berAppendTlv(recordTlv, 0x30U, recordValue.data(), recordValue.size());
        content.insert(content.end(), recordTlv.begin(), recordTlv.end());
    }

    std::vector<uint8_t> payload;
    berAppendTlv(payload, 0x61U, content.data(), content.size());
    return payload;
}

/**
 * @brief Decode ASN.1 BER/TLV payload to SER records.
 *
 * @details Parses a top-level APPLICATION 1 TLV and extracts all
 * SEQUENCE TLVs containing record data.
 *
 * @param data Input buffer containing TLV payload
 * @param size Size of input buffer in bytes
 * @param[out] out Vector to receive decoded records
 * @param[out] error Optional pointer to receive error message
 *
 * @return true if all records were decoded successfully
 * @return false on parse error (details in @p error if provided)
 *
 * @note Output vector is cleared before decoding
 */
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
            else if (field.tag == 0x84U)
                rec.relay_id = value;
            else if (field.tag == 0x85U)
                rec.relay_name = value;

            fieldOffset = field.nextOffset;
        }

        out.push_back(rec);
        offset = record.nextOffset;
    }

    return true;
}
}  // namespace asn_tlv
