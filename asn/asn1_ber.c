/* asn1_ber.c */

#include "asn1_ber.h"

/* Internal helper: decode tag. See header for documentation. */
int ber_read_tag(const uint8_t *buf, size_t len, size_t *p_off, BerTag *out)
{
    size_t off;
    uint8_t b0;
    uint32_t tagnum;
    size_t start;
    int seen;

    if (buf == 0 || p_off == 0 || out == 0)
    {
        return 0;
    }

    off = *p_off;
    if (off >= len)
    {
        return 0;
    }

    b0 = buf[off];
    out->first_octet = b0;
    out->cls = (Asn1Class)((b0 >> 6) & 0x03);
    out->constructed = (uint8_t)((b0 & 0x20U) ? 1U : 0U);

    tagnum = (uint32_t)(b0 & 0x1FU);
    start = off;
    off++;

    if (tagnum != 0x1FU)
    {
        out->number = tagnum;
        out->encoded_len = (uint8_t)(off - start);
        *p_off = off;
        return 1;
    }

    /* High-tag-number form */
    tagnum = 0U;
    seen = 0;
    while (off < len)
    {
        uint8_t b = buf[off++];
        seen++;
        if (seen > 5)
        {
            return 0; /* avoid absurdly large tag numbers */
        }
        tagnum = (tagnum << 7) | (uint32_t)(b & 0x7FU);
        if ((b & 0x80U) == 0U)
        {
            out->number = tagnum;
            out->encoded_len = (uint8_t)(off - start);
            *p_off = off;
            return 1;
        }
    }

    return 0;
}

/* Internal helper: decode length. See header for documentation. */
int ber_read_length(const uint8_t *buf, size_t len, size_t *p_off, size_t *outlen)
{
    size_t off;
    uint8_t b0;

    if (buf == 0 || p_off == 0 || outlen == 0)
    {
        return 0;
    }

    off = *p_off;
    if (off >= len)
    {
        return 0;
    }

    b0 = buf[off++];

    if ((b0 & 0x80U) == 0U)
    {
        /* Short form. */
        *outlen = (size_t)b0;
        *p_off = off;
        return 1;
    }
    else
    {
        /* Long form. */
        uint8_t nbytes = (uint8_t)(b0 & 0x7FU);
        size_t v = 0U;
        uint8_t i;

        if (nbytes == 0U)
        {
            return 0; /* indefinite length not supported */
        }
        if (nbytes > (uint8_t)sizeof(size_t))
        {
            return 0; /* larger than platform size_t */
        }
        if (off + (size_t)nbytes > len)
        {
            return 0;
        }

        for (i = 0U; i < nbytes; i++)
        {
            v = (v << 8) | (size_t)buf[off++];
        }

        *outlen = v;
        *p_off = off;
        return 1;
    }
}

/* TLV iterator. See header for documentation. */
int ber_next_tlv(const uint8_t *buf, size_t len, size_t *p_off, BerTlv *out)
{
    size_t start;
    BerTag tag;
    size_t vlen;
    size_t off;

    if (buf == 0 || p_off == 0 || out == 0)
    {
        return 0;
    }

    off = *p_off;
    start = off;

    if (!ber_read_tag(buf, len, &off, &tag))
    {
        return 0;
    }

    if (!ber_read_length(buf, len, &off, &vlen))
    {
        return 0;
    }

    if (off + vlen > len)
    {
        return 0;
    }

    out->tag = tag;
    out->tlv = buf + start;
    out->value = buf + off;
    out->value_len = vlen;
    out->tlv_len = (off - start) + vlen;

    off += vlen;
    *p_off = off;
    return 1;
}
