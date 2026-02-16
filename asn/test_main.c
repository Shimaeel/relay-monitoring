// COPYRIGHT (C) 2026 EUREKA POWER SOLUTIONS (www.PowerEureka.com)

/* test_main.c */
/**
 * @file test_main.c
 * @brief Simple test harness for the GOOSE ASN.1/BER parser.
 *
 * Build on PC:
 *   gcc -std=c99 -Wall -Wextra -o goose_test test_main.c asn1_ber.c goose_parser.c
 *
 * \dot
 * digraph TestHarness {
 *     rankdir=LR;
 *     node [shape=box, style="rounded,filled", fillcolor="#eef5ff"];
 *     main_node [label="main"];
 *     t1 [label="test_goose_minimal"];
 *     parse [label="goose_parse_pdu +\nprint results"];
 *     t2 [label="test_goose_find_pdu"];
 *     report [label="Console report"];
 *     main_node -> t1 -> parse -> report;
 *     main_node -> t2 -> report;
 * }
 * \enddot
 */

#include <stdio.h>
#include <string.h>
#include "asn1_ber.h"
#include "goose_parser.h"

/* Maximum elements for this test build / typical embedded use. */
#define MAX_GOOSE_FIELDS  32U
#define MAX_MMS_ELEMENTS  64U

/* Helper: print bytes as hex, truncated for debug. */
static void print_hex(const uint8_t *p, size_t n, size_t maxn)
{
    size_t i;
    size_t limit = (n > maxn) ? maxn : n;

    for (i = 0U; i < limit; i++)
    {
        printf("%02X", p[i]);
        if (i + 1U < limit)
        {
            printf(" ");
        }
    }
    if (n > maxn)
    {
        printf(" ...");
    }
}

/* Test 1: minimal artificial GOOSE PDU with allData = [ Boolean, Unsigned ]. */
static int test_goose_minimal(void)
{
    /* goosePdu (APPLICATION 1, tag 0x61):
     *   0x61 0x18
     *     [0] gocbRef VisibleString "TEST"
     *     [1] timeAllowedToLive Unsigned 1000
     *     [10] numDatSetEntries 2
     *     [11] allData
     *          Boolean TRUE
     *          Unsigned 42
     */
    static const uint8_t sample[] = {
        0x61, 0x18,
          0x80, 0x04, 'T','E','S','T',
          0x81, 0x02, 0x03, 0xE8,
          0x8A, 0x01, 0x02,
          0xAB, 0x09,
            0x83, 0x01, 0x01,
            0x86, 0x04, 0x00,0x00,0x00,0x2A
    };

    GooseFieldRef fields[MAX_GOOSE_FIELDS];
    MmsDataRef    mms[MAX_MMS_ELEMENTS];
    size_t field_count = MAX_GOOSE_FIELDS;
    size_t mms_capacity = MAX_MMS_ELEMENTS;
    GooseStatus st;
    size_t i;

    /* Initialize output arrays (optional but recommended). */
    memset(fields, 0, sizeof(fields));
    memset(mms, 0, sizeof(mms));

    st = goose_parse_pdu(sample, sizeof(sample),
                         fields, &field_count,
                         mms, &mms_capacity);
    if (st != GOOSE_OK)
    {
        printf("test_goose_minimal: goose_parse_pdu failed with status %d\n", (int)st);
        return 0;
    }

    printf("test_goose_minimal: parsed %u GOOSE fields\n", (unsigned)field_count);

    /* Print fields. */
    for (i = 0U; i < field_count; i++)
    {
        const GooseFieldRef *f = &fields[i];
        printf("  Field %u: %-18s tag0=0x%02X len=%u val=",
               (unsigned)i,
               goose_field_name(f->id),
               (unsigned)f->tag_octet,
               (unsigned)f->value_len);
        print_hex(f->value, f->value_len, 16U);
        printf("\n");

        if (f->vtype == GOOSE_VTYPE_UINT)
        {
            uint64_t x = 0U;
            if (goose_decode_unsigned(f->value, f->value_len, &x))
            {
                printf("    -> UINT = %llu\n", (unsigned long long)x);
            }
        }
        else if (f->vtype == GOOSE_VTYPE_BOOLEAN)
        {
            int b = 0;
            if (goose_decode_boolean(f->value, f->value_len, &b))
            {
                printf("    -> BOOL = %s\n", b ? "true" : "false");
            }
        }
        else if (f->vtype == GOOSE_VTYPE_VISIBLE_STRING)
        {
            printf("    -> STR  = \"%.*s\"\n",
                   (int)f->value_len, (const char *)f->value);
        }
    }

    /* mms_capacity was decremented by parse_mms_sequence() to indicate how much was used.
     * For clarity, convert it to a count here.
     */
    {
        size_t mms_used = MAX_MMS_ELEMENTS - mms_capacity;
        printf("\nMMS (allData) elements: %u\n", (unsigned)mms_used);

        for (i = 0U; i < mms_used; i++)
        {
            const MmsDataRef *e = &mms[i];
            printf("  [%u] depth=%d type=%-12s tag0=0x%02X len=%u val=",
                   (unsigned)i,
                   e->depth,
                   mms_type_name(e->type),
                   (unsigned)e->tag_octet,
                   (unsigned)e->value_len);
            print_hex(e->value, e->value_len, 16U);
            printf("\n");
        }
    }

    return 1;
}

/* Test 2: use goose_find_pdu on a "frame" with leading bytes and the same GOOSE PDU. */
static int test_goose_find_pdu(void)
{
    static const uint8_t frame[] = {
        0x00, 0x01, 0x02, 0x03, /* some fake header bytes */
        0x61, 0x02, 0x80, 0x00  /* minimal goosePdu with empty gocbRef */
    };

    const uint8_t *pdu = 0;
    size_t pdu_len = 0;
    GooseStatus st;

    st = goose_find_pdu(frame, sizeof(frame), &pdu, &pdu_len);
    if (st != GOOSE_OK)
    {
        printf("test_goose_find_pdu: failed to find goosePdu (status %d)\n", (int)st);
        return 0;
    }

    printf("test_goose_find_pdu: found goosePdu at offset %ld, length %u\n",
           (long)(pdu - frame), (unsigned)pdu_len);

    return 1;
}

int main(void)
{
    int ok = 1;

    printf("Running GOOSE parser tests...\n\n");

    if (!test_goose_minimal())
    {
        ok = 0;
    }
    printf("\n");

    if (!test_goose_find_pdu())
    {
        ok = 0;
    }
    printf("\n");

    if (ok)
    {
        printf("All tests PASSED.\n");
        return 0;
    }
    else
    {
        printf("Some tests FAILED.\n");
        return 1;
    }
}
