/* goose_parser.h */
/**
 * @file goose_parser.h
 * @brief GOOSE ASN.1/BER parser for embedded systems.
 *
 * This module builds on top of asn1_ber.[ch] to:
 * - Parse a GOOSE PDU (Application class tag 0x61).
 * - Extract top-level GOOSE fields as an array of pointers to values.
 * - Extract MMS Data elements inside the allData field as another array.
 *
 * Design goals:
 * - Pure C (C99 or later).
 * - No dynamic allocation (no malloc/realloc).
 * - Zero-copy of payload: all references are pointers into the input buffer.
 *
 * @section goose_pipeline Graphviz Flow
 * \dot
 * digraph GoosePipeline {
 *     rankdir=LR;
 *     node [shape=box, style="rounded,filled", fillcolor="#eef5ff"];
 *     subgraph cluster_ingress {
 *         label="Ingress";
 *         style="rounded,dashed";
 *         detect [label="goose_find_pdu\n(scan frame)", URL="\ref goose_find_pdu", tooltip="Locate APPLICATION 1 TLV inside raw buffer"];
 *     }
 *     goose [label="goose_parse_pdu\niterate fields", URL="\ref goose_parse_pdu", tooltip="Top-level BER walker"];
 *     verify [label="Check 0x61 + constructed", tooltip="Reject non-GOOSE PDUs"];
 *     loop [label="for each context tag", tooltip="Map [0..11] to GooseFieldRef"];
 *     capacity [label="Capacity guard", tooltip="GOOSE_ERR_TOO_MANY_*" shape=parallelogram];
 *     refs [label="GooseFieldRef[]\nid/vtype/value", tooltip="Zero-copy references"];
 *     branch [label="Field == allData?", shape=diamond, fillcolor="#fff8d9", tooltip="Only [11] triggers MMS recursion"];
 *     mms [label="parse_mms_sequence\n(recurse)", URL="\ref parse_mms_sequence", tooltip="Depth-first MMS decode"];
 *     emitmms [label="MmsDataRef[]", tooltip="Flat tree w/ depth+parent"];
 *     decode [label="goose_decode_* helpers", tooltip="Optional value helpers", URL="\ref goose_decode_unsigned"];
 *     detect -> goose;
 *     goose -> verify -> loop -> capacity -> refs -> branch;
 *     branch -> mms [label="Yes"];
 *     branch -> decode [label="No", style=dashed];
 *     mms -> emitmms -> decode;
 * }
 * \enddot
 */

#ifndef GOOSE_PARSER_H
#define GOOSE_PARSER_H

#include <stdint.h>
#include <stddef.h>
#include "asn1_ber.h"

/**
 * @brief Status codes returned by GOOSE parsing functions.
 */
typedef enum
{
    GOOSE_OK = 0,              /**< Success. */
    GOOSE_ERR_BER = -1,        /**< BER parse error (malformed or truncated). */
    GOOSE_ERR_NOT_GOOSE = -2,  /**< Buffer does not contain a GOOSE PDU (tag != 0x61). */
    GOOSE_ERR_TOO_MANY_FIELDS = -3, /**< Provided field array is too small. */
    GOOSE_ERR_TOO_MANY_MMS = -4     /**< Provided MMS array is too small. */
} GooseStatus;

/**
 * @brief Identifiers for GOOSE top-level fields (context-specific tags [0]..[11]).
 */
typedef enum
{
    GOOSE_FID_GOCBREF = 0,
    GOOSE_FID_TIME_ALLOWED_TO_LIVE = 1,
    GOOSE_FID_DATSET = 2,
    GOOSE_FID_GOID = 3,
    GOOSE_FID_T = 4,
    GOOSE_FID_STNUM = 5,
    GOOSE_FID_SQNUM = 6,
    GOOSE_FID_TEST = 7,
    GOOSE_FID_CONFREV = 8,
    GOOSE_FID_NDSCOM = 9,
    GOOSE_FID_NUM_DATASET_ENTRIES = 10,
    GOOSE_FID_ALLDATA = 11,
    GOOSE_FID_UNKNOWN = 255
} GooseFieldId;

/**
 * @brief Expected value type for a GOOSE field.
 *
 * This is a logical type hint; the raw value is always bytes.
 */
typedef enum
{
    GOOSE_VTYPE_UNKNOWN = 0,
    GOOSE_VTYPE_VISIBLE_STRING,
    GOOSE_VTYPE_UINT,
    GOOSE_VTYPE_INT,
    GOOSE_VTYPE_BOOLEAN,
    GOOSE_VTYPE_OCTET_STRING,
    GOOSE_VTYPE_UTCTIME,
    GOOSE_VTYPE_MMS_DATA_SEQ    /**< allData: MMS Data sequence. */
} GooseValueType;

/**
 * @brief MMS Data type (subset of types used in IEC 61850 allData).
 */
typedef enum
{
    MMS_TYPE_UNKNOWN = 0,
    MMS_TYPE_ARRAY,
    MMS_TYPE_STRUCTURE,
    MMS_TYPE_BOOLEAN,
    MMS_TYPE_BIT_STRING,
    MMS_TYPE_INTEGER,
    MMS_TYPE_UNSIGNED,
    MMS_TYPE_FLOATING_POINT,
    MMS_TYPE_OCTET_STRING,
    MMS_TYPE_VISIBLE_STRING,
    MMS_TYPE_TIME_OF_DAY,
    MMS_TYPE_BCD,
    MMS_TYPE_BOOLEAN_ARRAY,
    MMS_TYPE_UTC_TIME
} MmsDataType;

/**
 * @brief Reference to one top-level GOOSE field.
 *
 * All pointers refer into the original GOOSE PDU buffer.
 */
typedef struct
{
    GooseFieldId    id;        /**< Logical field id (gocbRef, stNum, allData, etc.). */
    GooseValueType  vtype;     /**< Expected logical value type. */
    uint8_t         tag_octet; /**< Raw first tag octet (usually 0x80..0x8B or 0xAB). */
    const uint8_t  *value;     /**< Pointer to BER value bytes of this field. */
    size_t          value_len; /**< Length of value in bytes. */
} GooseFieldRef;

/**
 * @brief Reference to one MMS Data element inside allData.
 *
 * All pointers refer into the original GOOSE PDU buffer.
 */
typedef struct
{
    size_t          index;        /**< Sequential index in the flat MMS array. */
    int             depth;        /**< Nesting depth: 0 = top-level in allData. */
    int             parent_index; /**< Parent element index, or -1 for root. */
    MmsDataType     type;         /**< MMS Data type (Boolean, Unsigned, etc.). */
    uint8_t         tag_octet;    /**< First tag octet for this MMS element. */
    const uint8_t  *value;        /**< Pointer to MMS Data value bytes. */
    size_t          value_len;    /**< Length in bytes. */
} MmsDataRef;

/**
 * @brief Return human-readable name for a GOOSE field id.
 *
 * @param id Field identifier.
 * @return Constant string for diagnostic/logging use.
 */
const char *goose_field_name(GooseFieldId id);

/**
 * @brief Return human-readable name for an MMS Data type.
 *
 * @param t MMS Data type.
 * @return Constant string for diagnostic/logging use.
 */
const char *mms_type_name(MmsDataType t);

/**
 * @brief Find a goosePdu (tag 0x61) inside a larger buffer (e.g. after APPID).
 *
 * This is optional convenience. It scans the buffer for a byte 0x61 and then
 * tries to interpret it as a BER TLV with tag=APPLICATION, constructed,
 * number=1. If successful, p_pdu points to 0x61 and *p_pdu_len is the full
 * TLV length (tag+length+value).
 *
 * @param buf        Input buffer.
 * @param len        Buffer length.
 * @param p_pdu      Output pointer to goosePdu TLV (tag octet).
 * @param p_pdu_len  Output total length of goosePdu TLV.
 * @return GOOSE_OK if found, GOOSE_ERR_NOT_GOOSE if no suitable TLV,
 *         GOOSE_ERR_BER if malformed.
 */
GooseStatus goose_find_pdu(const uint8_t *buf, size_t len,
                           const uint8_t **p_pdu, size_t *p_pdu_len);

/**
 * @brief Parse a GOOSE PDU (BER-encoded goosePdu starting at tag 0x61).
 *
 * This function:
 * - Verifies that the first TLV is an APPLICATION class, constructed, tag#1 (0x61).
 * - Iterates all top-level TLVs inside the goosePdu value (context tags [0]..[11]).
 * - Fills an array of GooseFieldRef for each field.
 * - If allData is present, parses its MMS Data elements and fills an array of MmsDataRef.
 *
 * The caller provides arrays and capacity for fields and MMS elements, so no
 * dynamic allocation is performed.
 *
 * @param buf             Pointer to goosePdu TLV (starting at 0x61).
 * @param len             Total length of that TLV (tag+length+value).
 * @param fields          Array of GooseFieldRef for output.
 * @param p_field_count   In: capacity of @p fields; Out: used count.
 * @param mms             Array of MmsDataRef for output (allData).
 * @param p_mms_count     In: capacity of @p mms; Out: used count.
 * @return GOOSE_OK on success, or a negative GooseStatus error code.
 *
 * \dot
 * digraph GooseParsePdu {
 *     rankdir=LR;
 *     node [shape=box, style="rounded,filled", fillcolor="#eef5ff"];
 *     start [label="Entry (buf/len)", URL="\ref goose_parse_pdu", tooltip="Public API"];
 *     wrap [label="ber_next_tlv outer", URL="\ref ber_next_tlv", tooltip="Read goosePdu envelope"];
 *     validate [label="APPLICATION 1?", tooltip="cls=APPLICATION, constructed=1, number=1"];
 *     reject [label="GOOSE_ERR_NOT_GOOSE", shape=octagon, fillcolor="#ffecec"];
 *     innerloop [label="while inner TLVs", tooltip="Iterate context-specific children"];
 *     read [label="ber_next_tlv inner", URL="\ref ber_next_tlv", tooltip="Read context TLV"];
 *     cap [label="field_used < capacity?", tooltip="Otherwise GOOSE_ERR_TOO_MANY_FIELDS"];
 *     caperr [label="GOOSE_ERR_TOO_MANY_FIELDS", shape=octagon, fillcolor="#ffecec"];
 *     build [label="Fill GooseFieldRef slot", tooltip="Store tag/value pointers"];
 *     classify [label="Map to GooseFieldId", tooltip="goose_field_from_context_tagnum"];
 *     branch [label="id == GOOSE_FID_ALLDATA?", shape=diamond, fillcolor="#fff8d9"];
 *     recurse [label="parse_mms_sequence", URL="\ref parse_mms_sequence", tooltip="Depth-first MMS parsing"];
 *     reserr [label="Propagate MMS error", shape=octagon, fillcolor="#ffecec"];
 *     advance [label="slide inner window", tooltip="inner += tlv.tlv_len"];
 *     done [label="Return GOOSE_OK"];
 *     start -> wrap -> validate;
 *     validate -> reject [label="No"];
 *     validate -> innerloop [label="Yes"];
 *     innerloop -> read -> cap;
 *     cap -> caperr [label="No"];
 *     cap -> build [label="Yes"];
 *     build -> classify -> branch;
 *     branch -> recurse [label="Yes"];
 *     recurse -> reserr [label="Error", style=dotted];
 *     recurse -> advance [label="OK"];
 *     branch -> advance [label="No"];
 *     advance -> innerloop [label="More TLVs", style=dashed];
 *     innerloop -> done [label="none left"];
 * }
 * \enddot
 */
GooseStatus goose_parse_pdu(const uint8_t *buf, size_t len,
                            GooseFieldRef *fields, size_t *p_field_count,
                            MmsDataRef *mms, size_t *p_mms_count);

/**
 * @brief Decode an unsigned integer from BER value bytes.
 *
 * This is a helper for fields like stNum, sqNum, confRev, etc. up to 8 bytes.
 *
 * @param value    Pointer to BER integer bytes (2's complement, big-endian).
 * @param value_len Length in bytes (1..8 recommended).
 * @param p_out    Output unsigned integer.
 * @return 1 on success, 0 on error (too long or NULL).
 */
int goose_decode_unsigned(const uint8_t *value, size_t value_len, uint64_t *p_out);

/**
 * @brief Decode a boolean from BER value bytes.
 *
 * IEC 61850 uses BOOLEAN as 1 byte (0x00 = FALSE, non-zero = TRUE).
 *
 * @param value     Pointer to BER boolean byte.
 * @param value_len Length in bytes (should be 1).
 * @param p_out     Output: 0 for FALSE, non-zero for TRUE.
 * @return 1 on success, 0 on error.
 */
int goose_decode_boolean(const uint8_t *value, size_t value_len, int *p_out);

#endif /* GOOSE_PARSER_H */
