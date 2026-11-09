#include "km/km_store.h"

#include <string.h>

#define CHUNK 64u    /* read-back / CRC chunk, on the stack */

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* a is newer than b (sequence numbers wrap) */
static bool seq_newer(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

void km_store_init(km_store_t *st, const km_store_ops_t *ops, uint32_t sector_a, uint32_t sector_b)
{
    st->ops = *ops;
    st->sector[0] = sector_a;
    st->sector[1] = sector_b;
}

bool km_store_check_sector(const km_store_t *st, unsigned idx, km_store_info_t *info)
{
    uint8_t h[KM_STORE_HEADER_BYTES];
    if (idx > 1u || !st->ops.read(st->ops.ctx, st->sector[idx], h, sizeof h)) {
        return false;
    }
    const uint32_t len = get32(h + 12);
    if (get32(h) != KM_STORE_MAGIC || len == 0 || len > KM_STORE_MAX_PAYLOAD) {
        return false;
    }
    uint32_t crc = km_crc32(0, h + 8, 8);
    uint8_t buf[CHUNK];
    for (uint32_t off = 0; off < len; off += CHUNK) {
        const uint32_t n = (len - off < CHUNK) ? len - off : CHUNK;
        if (!st->ops.read(st->ops.ctx, st->sector[idx] + KM_STORE_HEADER_BYTES + off, buf, n)) {
            return false;
        }
        crc = km_crc32(crc, buf, n);
    }
    if (crc != get32(h + 4)) {
        return false;
    }
    if (info != NULL) {
        info->valid = true;
        info->index = (uint8_t)idx;
        info->seq = get32(h + 8);
        info->len = len;
    }
    return true;
}

bool km_store_find(const km_store_t *st, km_store_info_t *info)
{
    km_store_info_t a, b;
    const bool va = km_store_check_sector(st, 0, &a);
    const bool vb = km_store_check_sector(st, 1, &b);
    if (!va && !vb) {
        return false;
    }
    if (info != NULL) {
        *info = (va && (!vb || !seq_newer(b.seq, a.seq))) ? a : b;    /* a tie goes to sector 0 */
    }
    return true;
}

bool km_store_read(const km_store_t *st, uint8_t *buf, uint32_t cap, uint32_t *len)
{
    km_store_info_t cur;
    if (buf == NULL || !km_store_find(st, &cur) || cur.len > cap) {
        return false;
    }
    if (!st->ops.read(st->ops.ctx, st->sector[cur.index] + KM_STORE_HEADER_BYTES, buf, cur.len)) {
        return false;
    }
    if (len != NULL) {
        *len = cur.len;
    }
    return true;
}

/* The payload of record sector idx equals data[0..len)? */
static bool payload_equals(const km_store_t *st, unsigned idx, const uint8_t *data, uint32_t len)
{
    uint8_t buf[CHUNK];
    for (uint32_t off = 0; off < len; off += CHUNK) {
        const uint32_t n = (len - off < CHUNK) ? len - off : CHUNK;
        if (!st->ops.read(st->ops.ctx, st->sector[idx] + KM_STORE_HEADER_BYTES + off, buf, n) ||
            memcmp(buf, data + off, n) != 0) {
            return false;
        }
    }
    return true;
}

km_store_result_t km_store_write(const km_store_t *st, const uint8_t *data, uint32_t len)
{
    if (st == NULL || data == NULL || len == 0 || len > KM_STORE_MAX_PAYLOAD) {
        return KM_STORE_ERR_ARG;
    }
    km_store_info_t cur;
    const bool have = km_store_find(st, &cur);
    if (have && cur.len == len && payload_equals(st, cur.index, data, len)) {
        return KM_STORE_UNCHANGED;
    }
    const unsigned target = have ? 1u - cur.index : 0u;
    const uint32_t base = st->sector[target];
    const uint32_t seq = have ? cur.seq + 1u : 1u;

    uint8_t head[KM_STORE_HEADER_BYTES];
    put32(head + 8, seq);
    put32(head + 12, len);
    put32(head, KM_STORE_MAGIC);
    put32(head + 4, km_crc32(km_crc32(0, head + 8, 8), data, len));

    if (!st->ops.erase(st->ops.ctx, base)) {
        return KM_STORE_ERR_ERASE;
    }
    /* Every unit is written in ascending address order: the flash controller reports a programming error when a unit is
     * written below one that is already programmed in the same 16-byte line (seen on the hardware). The record only counts
     * as valid once the CRC over sequence, length and payload matches, so a power cut at any point leaves either the old
     * record or an invalid sector, whatever the order. 1. payload; 2. magic + CRC unit; 3. sequence + length unit LAST. */
    for (uint32_t off = 0; off < len; off += KM_STORE_UNIT) {
        uint8_t unit[KM_STORE_UNIT];
        memset(unit, 0xFF, sizeof unit);
        memcpy(unit, data + off, (len - off < KM_STORE_UNIT) ? len - off : KM_STORE_UNIT);
        static const uint8_t blank[KM_STORE_UNIT] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        if (memcmp(unit, blank, sizeof unit) == 0) {
            /* Programming 0xFF changes no bits, but a blank LOWER unit still has to be programmed when the upper unit of its
             * 16-byte line carries data: the controller refuses an upper unit whose lower unit was never programmed (seen on
             * the hardware, in the calibration log). A blank upper unit, or a line that is blank as a whole, is skipped. */
            const bool upper = ((off / KM_STORE_UNIT) & 1u) != 0u;
            bool next_blank = true;
            if (!upper && off + KM_STORE_UNIT < len) {
                for (uint32_t i = off + KM_STORE_UNIT; i < off + 2u * KM_STORE_UNIT && i < len; i++) {
                    next_blank = next_blank && data[i] == 0xFFu;
                }
            }
            if (upper || next_blank) {
                continue;
            }
        }
        if (!st->ops.program(st->ops.ctx, base + KM_STORE_HEADER_BYTES + off, unit)) {
            return KM_STORE_ERR_PROGRAM;
        }
    }
    if (!st->ops.program(st->ops.ctx, base, head)) {
        return KM_STORE_ERR_PROGRAM;
    }
    if (!st->ops.program(st->ops.ctx, base + 8u, head + 8)) {
        return KM_STORE_ERR_PROGRAM;
    }

    /* verify by reading everything back */
    uint8_t rb[KM_STORE_HEADER_BYTES];
    if (!st->ops.read(st->ops.ctx, base, rb, sizeof rb)) {
        return KM_STORE_ERR_READ;
    }
    if (memcmp(rb, head, sizeof rb) != 0 || !payload_equals(st, target, data, len)) {
        return KM_STORE_ERR_VERIFY;
    }
    km_store_info_t now;
    if (!km_store_check_sector(st, target, &now) || now.seq != seq || now.len != len) {
        return KM_STORE_ERR_VERIFY;
    }
    return KM_STORE_OK;
}

/* ---- settings ---- */

bool km_store_load_settings(const km_store_t *st, km_settings_t *s, uint8_t *scratch, unsigned cap)
{
    uint32_t len = 0;
    if (st == NULL || s == NULL || scratch == NULL || cap < KM_SETTINGS_ENCODED_SIZE) {
        return false;
    }
    return km_store_read(st, scratch, cap, &len) && km_settings_decode(s, scratch, len);
}

km_store_result_t km_store_save_settings(const km_store_t *st, const km_settings_t *s, uint8_t *scratch, unsigned cap)
{
    if (st == NULL || s == NULL || scratch == NULL) {
        return KM_STORE_ERR_ARG;
    }
    const unsigned n = km_settings_encode(s, scratch, cap);
    return n == 0 ? KM_STORE_ERR_ARG : km_store_write(st, scratch, n);
}

/* ---- argument-less API ---- */

static const km_store_t *g_bound;
static uint8_t g_scratch[KM_SETTINGS_ENCODED_SIZE];
static km_store_result_t g_last_result;

void km_store_bind(const km_store_t *st)
{
    g_bound = st;
}

bool km_store_load(km_settings_t *s)
{
    return km_store_load_settings(g_bound, s, g_scratch, sizeof g_scratch);
}

bool km_store_save(const km_settings_t *s)
{
    const km_store_result_t r = km_store_save_settings(g_bound, s, g_scratch, sizeof g_scratch);
    g_last_result = r;
    return r == KM_STORE_OK || r == KM_STORE_UNCHANGED;
}

km_store_result_t km_store_last_result(void)
{
    return g_last_result;
}
