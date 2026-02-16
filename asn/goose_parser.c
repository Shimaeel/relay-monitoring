// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/* goose_parser.c */

#include "goose_parser.h"

/* ------------ internal helpers ------------ */

static GooseFieldId goose_field_from_context_tagnum(uint32_t n)
{
    if (n <= 11U)
    {
        return (GooseFieldId)n;
    }
    return GOOSE_FID_UNKNOWN;
}

static GooseValueType goose_expected_type(GooseFieldId id)
{
    switch (id)
    {
        case GOOSE_FID_GOCBREF:
        case GOOSE_FID_DATSET:
        case GOOSE_FID_GOID:
            return GOOSE_VTYPE_VISIBLE_STRING;

        case GOOSE_FID_TIME_ALLOWED_TO_LIVE:
        case GOOSE_FID_STNUM:
        case GOOSE_FID_SQNUM:
        case GOOSE_FID_CONFREV:
        case GOOSE_FID_NUM_DATASET_ENTRIES:
            return GOOSE_VTYPE_UINT;

        case GOOSE_FID_TEST:
        case GOOSE_FID_NDSCOM:
            return GOOSE_VTYPE_BOOLEAN;

        case GOOSE_FID_T:
            return GOOSE_VTYPE_UTCTIME;

        case GOOSE_FID_ALLDATA:
            return GOOSE_VTYPE_MMS_DATA_SEQ;

        default:
            return GOOSE_VTYPE_UNKNOWN;
    }
}

static MmsDataType mms_type_from_tag_octet(uint8_t tag0)
{
    /* IEC 61850 MMS Data commonly uses context-specific tags in 0x81.. etc. */
    switch (tag0)
    {
        case 0x81: return MMS_TYPE_ARRAY;
        case 0x82: return MMS_TYPE_STRUCTURE;
        case 0x83: return MMS_TYPE_BOOLEAN;
        case 0x84: return MMS_TYPE_BIT_STRING;
        case 0x85: return MMS_TYPE_INTEGER;
        case 0x86: return MMS_TYPE_UNSIGNED;
        case 0x87: return MMS_TYPE_FLOATING_POINT;
        case 0x89: return MMS_TYPE_OCTET_STRING;
        case 0x8A: return MMS_TYPE_VISIBLE_STRING;
        case 0x8C: return MMS_TYPE_TIME_OF_DAY;
        case 0x8D: return MMS_TYPE_BCD;
        case 0x8E: return MMS_TYPE_BOOLEAN_ARRAY;
        case 0x91: return MMS_TYPE_UTC_TIME;
        default:   return MMS_TYPE_UNKNOWN;
    }
}

/**
 * @brief Recursively parse MMS Data TLVs inside allData.
 *
 * @param buf        Pointer to first byte of the sequence.
 * @param len        Length of sequence.
 * @param mms        Output array of MmsDataRef.
 * @param p_mms_cnt  In: capacity, Out: used count.
 * @param depth      Current nesting depth (0 = top-level).
 * @param parent_idx Parent index or -1 for root.
 *
 * @return GOOSE_OK or error code.
 *
 * \dot
 * digraph ParseMmsSequence {
 *     rankdir=LR;
 *     node [shape=box, style="rounded,filled", fillcolor="#eef5ff"];
 *     start [label="parse_mms_sequence(depth, parent)", URL="\ref parse_mms_sequence", tooltip="Recursive helper"];
 *     cap [label="remaining slots?", tooltip="Uses *p_mms_cnt as reverse index"];
 *     caperr [label="GOOSE_ERR_TOO_MANY_MMS", shape=octagon, fillcolor="#ffecec"];
 *     next [label="ber_next_tlv", URL="\ref ber_next_tlv", tooltip="Iterate child TLVs"];
 *     emit [label="Write MmsDataRef entry", tooltip="Store index/depth/parent/tag"];
 *     depthnode [label="Depth++", tooltip="child inherits parent index"];
 *     check [label="tlv.tag.constructed?", shape=diamond, fillcolor="#fff8d9"];
 *     recurse [label="parse_mms_sequence(value, len)", URL="\ref parse_mms_sequence", tooltip="DFS child sequence"];
 *     bubble [label="Propagate status", shape=parallelogram, tooltip="Return first error"];
 *     advance [label="off += tlv.tlv_len", tooltip="Advance window"];
 *     done [label="Return GOOSE_OK"];
 *     start -> cap;
 *     cap -> caperr [label="No"];
 *     cap -> next [label="Yes"];
 *     next -> emit -> depthnode -> check;
 *     check -> recurse [label="Yes"];
 *     recurse -> bubble [label="Error", style=dotted];
 *     recurse -> advance [label="OK"];
 *     bubble -> done [style=dotted];
 *     check -> advance [label="No"];
 *     advance -> cap [label="more TLVs", style=dashed];
 *     next -> done [label="none", style=dashed];
 * }
 * \enddot
 */
static GooseStatus parse_mms_sequence(const uint8_t *buf, size_t len,
                                      MmsDataRef *mms, size_t *p_mms_cnt,
                                      int depth, int parent_idx)
{
    size_t off = 0U;

    while (off < len)
    {
        BerTlv tlv;
        size_t before = off;
        GooseStatus st;

        if (!ber_next_tlv(buf, len, &off, &tlv))
        {
            return GOOSE_ERR_BER;
        }

        if (*p_mms_cnt == 0U)
        {
            return GOOSE_ERR_TOO_MANY_MMS;
        }

        /* Use the last slot and then advance pointer. */
        {
            size_t idx = 0U;
            MmsDataRef *r;

            /* We treat the array as "capacity remaining" via p_mms_cnt.
             * For embedded use, you may prefer the opposite convention.
             */
            idx = (*p_mms_cnt) - 1U;
            r = &mms[idx];

            r->index = idx;
            r->depth = depth;
            r->parent_index = parent_idx;
            r->type = mms_type_from_tag_octet(tlv.tag.first_octet);
            r->tag_octet = tlv.tag.first_octet;
            r->value = tlv.value;
            r->value_len = tlv.value_len;
        }

        /* Decrement "remaining capacity". */
        (*p_mms_cnt)--;

        /* Recurse into constructed types (Array / Structure). */
        if (tlv.tag.constructed)
        {
            int this_parent = (int)((*p_mms_cnt)); /* not perfect, but consistent with "reverse filling". */

            st = parse_mms_sequence(tlv.value, tlv.value_len,
                                    mms, p_mms_cnt,
                                    depth + 1, this_parent);
            if (st != GOOSE_OK)
            {
                return st;
            }
        }

        if (off == before)
        {
            return GOOSE_ERR_BER;
        }
    }

    return GOOSE_OK;
}

/* ------------ public API ------------ */

const char *goose_field_name(GooseFieldId id)
{
    switch (id)
    {
        case GOOSE_FID_GOCBREF:             return "gocbRef";
        case GOOSE_FID_TIME_ALLOWED_TO_LIVE:return "timeAllowedToLive";
        case GOOSE_FID_DATSET:              return "datSet";
        case GOOSE_FID_GOID:                return "goID";
        case GOOSE_FID_T:                   return "t";
        case GOOSE_FID_STNUM:               return "stNum";
        case GOOSE_FID_SQNUM:               return "sqNum";
        case GOOSE_FID_TEST:                return "test";
        case GOOSE_FID_CONFREV:             return "confRev";
        case GOOSE_FID_NDSCOM:              return "ndsCom";
        case GOOSE_FID_NUM_DATASET_ENTRIES: return "numDatSetEntries";
        case GOOSE_FID_ALLDATA:             return "allData";
        default:                            return "unknown";
    }
}

const char *mms_type_name(MmsDataType t)
{
    switch (t)
    {
        case MMS_TYPE_ARRAY:          return "Array";
        case MMS_TYPE_STRUCTURE:      return "Structure";
        case MMS_TYPE_BOOLEAN:        return "Boolean";
        case MMS_TYPE_BIT_STRING:     return "BitString";
        case MMS_TYPE_INTEGER:        return "Integer";
        case MMS_TYPE_UNSIGNED:       return "Unsigned";
        case MMS_TYPE_FLOATING_POINT: return "FloatingPoint";
        case MMS_TYPE_OCTET_STRING:   return "OctetString";
        case MMS_TYPE_VISIBLE_STRING: return "VisibleString";
        case MMS_TYPE_TIME_OF_DAY:    return "TimeOfDay";
        case MMS_TYPE_BCD:            return "BCD";
        case MMS_TYPE_BOOLEAN_ARRAY:  return "BooleanArray";
        case MMS_TYPE_UTC_TIME:       return "UtcTime";
        default:                      return "Unknown";
    }
}

GooseStatus goose_find_pdu(const uint8_t *buf, size_t len,
                           const uint8_t **p_pdu, size_t *p_pdu_len)
{
    size_t i;

    if (buf == 0 || p_pdu == 0 || p_pdu_len == 0)
    {
        return GOOSE_ERR_BER;
    }

    for (i = 0U; i + 2U < len; i++)
    {
        if (buf[i] != 0x61U)
        {
            continue;
        }

        /* Try to read a TLV from this position. */
        {
            size_t off = i;
            BerTlv tlv;

            if (!ber_next_tlv(buf, len, &off, &tlv))
            {
                continue;
            }

            if (tlv.tag.cls == ASN1_CLASS_APPLICATION &&
                tlv.tag.constructed != 0U &&
                tlv.tag.number == 1U)
            {
                *p_pdu = buf + i;
                *p_pdu_len = tlv.tlv_len;
                return GOOSE_OK;
            }
        }
    }

    return GOOSE_ERR_NOT_GOOSE;
}

GooseStatus goose_parse_pdu(const uint8_t *buf, size_t len,
                            GooseFieldRef *fields, size_t *p_field_count,
                            MmsDataRef *mms, size_t *p_mms_count)
{
    size_t off = 0U;
    BerTlv outer;
    const uint8_t *inner;
    size_t inner_len;
    size_t field_capacity;
    size_t field_used = 0U;
    int have_allData = 0;
    GooseStatus st;

    if (buf == 0 || fields == 0 || p_field_count == 0 || mms == 0 || p_mms_count == 0)
    {
        return GOOSE_ERR_BER;
    }

    field_capacity = *p_field_count;

    if (!ber_next_tlv(buf, len, &off, &outer))
    {
        return GOOSE_ERR_BER;
    }

    if (!(outer.tag.cls == ASN1_CLASS_APPLICATION &&
          outer.tag.constructed != 0U &&
          outer.tag.number == 1U))
    {
        return GOOSE_ERR_NOT_GOOSE;
    }

    inner = outer.value;
    inner_len = outer.value_len;

    while (1)
    {
        BerTlv tlv;
        GooseFieldRef *fr;

        if (off >= len && inner_len == 0U)
        {
            break;
        }

        if (inner_len == 0U)
        {
            break;
        }

        {
            size_t inner_off = 0U;
            if (!ber_next_tlv(inner, inner_len, &inner_off, &tlv))
            {
                break;
            }
            /* Move "window" forward. */
            inner += tlv.tlv_len;
            inner_len -= tlv.tlv_len;
        }

        if (field_used >= field_capacity)
        {
            return GOOSE_ERR_TOO_MANY_FIELDS;
        }

        fr = &fields[field_used];
        field_used++;

        fr->tag_octet = tlv.tag.first_octet;
        fr->value = tlv.value;
        fr->value_len = tlv.value_len;
        fr->id = GOOSE_FID_UNKNOWN;
        fr->vtype = GOOSE_VTYPE_UNKNOWN;

        if (tlv.tag.cls == ASN1_CLASS_CONTEXT)
        {
            GooseFieldId id = goose_field_from_context_tagnum(tlv.tag.number);
            fr->id = id;
            fr->vtype = goose_expected_type(id);
        }

        if (fr->id == GOOSE_FID_ALLDATA)
        {
            have_allData = 1;
            /* For MMS parsing, p_mms_count is treated as remaining capacity. */
            st = parse_mms_sequence(tlv.value, tlv.value_len,
                                    mms, p_mms_count,
                                    0, -1);
            if (st != GOOSE_OK)
            {
                return st;
            }
        }
    }

    *p_field_count = field_used;

    (void)have_allData; /* You can check this flag if needed. */

    return GOOSE_OK;
}

int goose_decode_unsigned(const uint8_t *value, size_t value_len, uint64_t *p_out)
{
    size_t i;
    uint64_t x = 0U;

    if (value == 0 || p_out == 0 || value_len == 0U || value_len > 8U)
    {
        return 0;
    }

    for (i = 0U; i < value_len; i++)
    {
        x = (x << 8) | (uint64_t)value[i];
    }

    *p_out = x;
    return 1;
}

int goose_decode_boolean(const uint8_t *value, size_t value_len, int *p_out)
{
    if (value == 0 || p_out == 0 || value_len != 1U)
    {
        return 0;
    }

    *p_out = (value[0] != 0U) ? 1 : 0;
    return 1;
}
