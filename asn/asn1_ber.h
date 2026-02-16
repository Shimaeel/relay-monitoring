// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/* asn1_ber.h */
/**
 * @file asn1_ber.h
 * @brief Minimal ASN.1 BER TLV reader for IEC 61850 GOOSE/MMS.
 *
 * This module provides a very small subset of ASN.1 BER decoding that is
 * sufficient for parsing GOOSE PDUs and MMS Data structures:
 * - Tag decoding (class, constructed flag, tag number).
 * - Length decoding (definite length, short/long form).
 * - TLV iteration via ber_next_tlv().
 *
 * All operations are zero-copy: no dynamic allocation and no copying of
 * payload bytes. The TLV structures only keep pointers and lengths into
 * the caller's buffer.
 *
 * @section ber_flow Graphviz Flow
 * \dot
 * digraph BERFlow {
 *     rankdir=LR;
 *     node [shape=box, style="rounded,filled", fillcolor="#eef5ff"];
 *     start [label="ber_next_tlv entry", URL="\ref ber_next_tlv", tooltip="Public iterator entry point"];
 *     tag [label="ber_read_tag", URL="\ref ber_read_tag", tooltip="Decode class/number"];
 *     taghi [label="High-tag loop", tooltip="Shift + accumulate base-128"];
 *     len [label="ber_read_length", URL="\ref ber_read_length", tooltip="Decode definite length"];
 *     shortlen [label="Short form", tooltip="Single-octet length"];
 *     longlen [label="Long form", tooltip="n-byte length with bounds"];
 *     guard [label="Has value bytes?\n(off + len <= buf)"];
 *     emit [label="Populate BerTlv\n(tag/value pointers)", URL="\ref ber_next_tlv", tooltip="Fill caller struct"];
 *     error [label="Return 0", shape=octagon, fillcolor="#ffecec", tooltip="Failure bubbles up"];
 *     start -> tag;
 *     tag -> taghi [label=">1 octet", style=dashed];
 *     tag -> len;
 *     taghi -> len;
 *     len -> shortlen [label="bit7=0"];
 *     len -> longlen [label="bit7=1"];
 *     shortlen -> guard;
 *     longlen -> guard;
 *     guard -> emit [label="Yes"];
 *     guard -> error [label="No"];
 *     taghi -> error [label=">5 octets", style=dotted];
 *     longlen -> error [label="overflow", style=dotted];
 * }
 * \enddot
 */

#ifndef ASN1_BER_H
#define ASN1_BER_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief ASN.1 class of a tag.
 */
typedef enum
{
    ASN1_CLASS_UNIVERSAL   = 0,
    ASN1_CLASS_APPLICATION = 1,
    ASN1_CLASS_CONTEXT     = 2,
    ASN1_CLASS_PRIVATE     = 3
} Asn1Class;

/**
 * @brief BER tag information.
 *
 * This represents the decoded tag octets of a TLV element.
 */
typedef struct
{
    Asn1Class cls;      /**< ASN.1 class (UNIVERSAL, APPLICATION, CONTEXT, PRIVATE). */
    uint8_t   constructed; /**< Non-zero if constructed (bit 5 of first octet). */
    uint32_t  number;   /**< Tag number (low 5 bits or high-tag-number form). */
    uint8_t   first_octet; /**< Original first tag octet. */
    uint8_t   encoded_len; /**< Number of octets used for the tag field. */
} BerTag;

/**
 * @brief BER TLV element: Tag + Length + Value.
 *
 * All pointers point directly into the caller's buffer; no copies are made.
 */
typedef struct
{
    BerTag         tag;       /**< Decoded tag. */
    const uint8_t *tlv;       /**< Pointer to first Tag octet. */
    size_t         tlv_len;   /**< Total TLV length in bytes (Tag+Length+Value). */
    const uint8_t *value;     /**< Pointer to Value bytes. */
    size_t         value_len; /**< Length of Value in bytes. */
} BerTlv;

/**
 * @brief Decode a BER tag from a buffer.
 *
 * This is a low-level function. In most cases you want ber_next_tlv().
 *
 * @param buf   Input buffer.
 * @param len   Total buffer length in bytes.
 * @param p_off In/out offset; on entry the current position, on return the
 *              offset just after the tag field.
 * @param out   Pointer to store decoded BerTag.
 * @return 1 on success, 0 on error (truncated, malformed).
 */
int ber_read_tag(const uint8_t *buf, size_t len, size_t *p_off, BerTag *out);

/**
 * @brief Decode a BER length from a buffer.
 *
 * Supports definite length (short or long form). Indefinite length is not
 * supported and is treated as an error.
 *
 * @param buf    Input buffer.
 * @param len    Total buffer length in bytes.
 * @param p_off  In/out offset; on entry the current position, on return the
 *               offset just after the length field.
 * @param outlen Pointer to store the decoded length.
 * @return 1 on success, 0 on error.
 */
int ber_read_length(const uint8_t *buf, size_t len, size_t *p_off, size_t *outlen);

/**
 * @brief Read next BER TLV (Tag + Length + Value) from a buffer.
 *
 * @param buf    Input buffer.
 * @param len    Total buffer length in bytes.
 * @param p_off  In/out offset; on entry the current position, on return the
 *               offset just after the entire TLV.
 * @param out    Pointer to BerTlv where result is stored.
 * @return 1 on success, 0 on error.
 */
int ber_next_tlv(const uint8_t *buf, size_t len, size_t *p_off, BerTlv *out);

#endif /* ASN1_BER_H */
