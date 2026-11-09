/*
 * km_anim.c - decoder for the KMAN animation format, see km_anim.h. Integer math only, no allocation, no waiting.
 * Every read of the clip goes through the bit reader below, which can never read outside the group it was given.
 */
#include "km/km_anim.h"

#include <string.h>

#define EG_MAX_PREFIX 7u   /* longest zero prefix of an Exp-Golomb code: the biggest value we ever code is 61 */

_Static_assert(KM_ANIM_LIGHTS == 61u, "the clip format and the light layout agree on 61 lights");
_Static_assert(KM_ANIM_MAX_GROUPS * 2u + 32u + KM_ANIM_MAX_DATA + 8u < 0x7FFFFFFFu, "length arithmetic stays in 32 bits");

/* ---------------------------------------------------------------- small helpers */

static unsigned rd16(const uint8_t *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* CRC-32/IEEE, reflected, 16-entry table so that the code stays tiny (two lookups per byte). */
static uint32_t crc_update(uint32_t c, const uint8_t *p, size_t n)
{
    static const uint32_t t[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    while (n--) {
        c ^= *p++;
        c = (c >> 4) ^ t[c & 15u];
        c = (c >> 4) ^ t[c & 15u];
    }
    return c;
}

/* ---------------------------------------------------------------- bit reader (MSB first, bounded) */

typedef struct {
    const uint8_t *p;
    uint32_t pos;      /* bit position of the next bit */
    uint32_t end;      /* bit position one past the last readable bit */
    bool err;
} br_t;

static unsigned br_bits(br_t *r, unsigned n)   /* n <= 16 */
{
    unsigned v = 0;
    if (r->err || n > r->end - r->pos) {
        r->err = true;
        return 0;
    }
    while (n > 0u) {
        const unsigned avail = 8u - (r->pos & 7u);
        const unsigned take = n < avail ? n : avail;
        const unsigned byte = r->p[r->pos >> 3];
        v = (v << take) | ((byte >> (avail - take)) & ((1u << take) - 1u));
        r->pos += take;
        n -= take;
    }
    return v;
}

/* Exp-Golomb of order k. On any problem r->err is set and 0 is returned. */
static unsigned br_eg(br_t *r, unsigned k)
{
    unsigned q = 0;
    while (br_bits(r, 1u) == 0u) {
        if (r->err || ++q > EG_MAX_PREFIX) {
            r->err = true;
            return 0;
        }
    }
    if (r->err) {
        return 0;
    }
    return (((1u << q) - 1u) << k) + br_bits(r, q + k);
}

/* ---------------------------------------------------------------- header */

static unsigned groups_for(unsigned frames, unsigned key)
{
    return (frames + key - 1u) / key;
}

static unsigned key_frame_bytes(unsigned bits)
{
    return (KM_ANIM_LIGHTS * bits + 7u) / 8u;
}

km_anim_err_t km_anim_parse(const uint8_t *data, size_t len, km_anim_info_t *info)
{
    if (data == NULL || info == NULL || len == 0u) {
        return KM_ANIM_E_ARG;
    }
    if (len < KM_ANIM_HEADER_BYTES + 2u + 2u + 1u + 4u) {
        return KM_ANIM_E_SIZE;
    }
    if (memcmp(data, "KMAN", 4) != 0) {
        return KM_ANIM_E_MAGIC;
    }
    if (data[4] != KM_ANIM_VERSION) {
        return KM_ANIM_E_VERSION;
    }
    const unsigned bits = data[6], fps = data[7], frames = rd16(data + 8), key = data[10], flags = data[11];
    const unsigned kc = data[12], kg = data[13], groups = rd16(data + 14), data_len = rd16(data + 16);
    if (data[5] != KM_ANIM_LIGHTS || (bits != 1u && bits != 2u && bits != 4u) || fps < 1u || fps > 60u || frames < 1u ||
        key < 1u || (flags & ~1u) != 0u || kc > 4u || kg > 4u || groups != groups_for(frames, key) ||
        groups > KM_ANIM_MAX_GROUPS || data_len < 1u || rd16(data + 18) != 0u) {
        return KM_ANIM_E_HEADER;
    }
    const unsigned levels = 1u << bits;
    const size_t total = (size_t)KM_ANIM_HEADER_BYTES + levels + 2u * groups + data_len + 4u;
    if (len != total) {
        return KM_ANIM_E_SIZE;
    }
    const uint8_t *pal = data + KM_ANIM_HEADER_BYTES;
    for (unsigned i = 1; i < levels; i++) {
        if (pal[i] <= pal[i - 1u]) {
            return KM_ANIM_E_PALETTE;
        }
    }
    const uint8_t *idx = pal + levels;
    const unsigned min_group = key_frame_bytes(bits);
    unsigned prev = 0;
    for (unsigned g = 0; g < groups; g++) {
        const unsigned off = rd16(idx + 2u * g);
        const unsigned next = (g + 1u < groups) ? rd16(idx + 2u * (g + 1u)) : data_len;
        if ((g == 0u && off != 0u) || (g > 0u && off <= prev) || off >= data_len || next > data_len || next < off ||
            next - off < min_group) {
            return KM_ANIM_E_INDEX;
        }
        prev = off;
    }
    info->base = data;
    info->palette = pal;
    info->index = idx;
    info->data = idx + 2u * groups;
    info->total_len = (uint32_t)total;
    info->data_len = data_len;
    info->crc = rd32(data + total - 4u);
    info->frames = (uint16_t)frames;
    info->groups = (uint16_t)groups;
    info->bits = (uint8_t)bits;
    info->fps = (uint8_t)fps;
    info->key_interval = (uint8_t)key;
    info->flags = (uint8_t)flags;
    info->kc = (uint8_t)kc;
    info->kg = (uint8_t)kg;
    return KM_ANIM_OK;
}

uint32_t km_anim_duration_ms(const km_anim_info_t *info)
{
    return (uint32_t)(((uint64_t)info->frames * 1000u) / info->fps);
}

/* ---------------------------------------------------------------- cursor: decoding one frame */

static unsigned group_end_byte(const km_anim_t *a, unsigned g)
{
    return (g + 1u < a->info.groups) ? rd16(a->info.index + 2u * (g + 1u)) : a->info.data_len;
}

/* The group must end exactly where the index says and the bits after the last frame must be zero. */
static km_anim_err_t check_tail(const km_anim_t *a, unsigned g)
{
    const uint32_t pos = a->bitpos;
    if (((pos + 7u) >> 3) != group_end_byte(a, g)) {
        return KM_ANIM_E_PAD;
    }
    if ((pos & 7u) != 0u && (a->info.data[pos >> 3] & (0xFFu >> (pos & 7u))) != 0u) {
        return KM_ANIM_E_PAD;
    }
    return KM_ANIM_OK;
}

static bool last_in_group(const km_anim_t *a, unsigned frame)
{
    return (frame % a->info.key_interval) == (unsigned)a->info.key_interval - 1u || frame + 1u == a->info.frames;
}

static km_anim_err_t dec_key(km_anim_t *a, unsigned g)
{
    br_t r = { a->info.data, rd16(a->info.index + 2u * g) * 8u, group_end_byte(a, g) * 8u, false };
    for (unsigned i = 0; i < KM_ANIM_LIGHTS; i++) {
        a->cur[i] = (uint8_t)br_bits(&r, a->info.bits);
    }
    if (r.err) {
        return KM_ANIM_E_STREAM;
    }
    a->bitpos = r.pos;
    a->work = (uint16_t)(a->work + KM_ANIM_LIGHTS);
    return KM_ANIM_OK;
}

static km_anim_err_t dec_diff(km_anim_t *a, unsigned g)
{
    br_t r = { a->info.data, a->bitpos, group_end_byte(a, g) * 8u, false };
    const unsigned count = br_eg(&r, a->info.kc);
    if (r.err || count > KM_ANIM_LIGHTS) {
        return KM_ANIM_E_STREAM;
    }
    unsigned cell = 0;
    for (unsigned k = 0; k < count; k++) {
        cell += br_eg(&r, a->info.kg);
        if (r.err || cell >= KM_ANIM_LIGHTS) {
            return KM_ANIM_E_STREAM;
        }
        if (a->info.bits == 1u) {
            a->cur[cell] ^= 1u;
        } else {
            const uint8_t v = (uint8_t)br_bits(&r, a->info.bits);
            if (r.err) {
                return KM_ANIM_E_STREAM;
            }
            if (v == a->cur[cell]) {
                return KM_ANIM_E_NOOP;
            }
            a->cur[cell] = v;
        }
        cell++;
    }
    a->bitpos = r.pos;
    a->work = (uint16_t)(a->work + 1u + count);
    return KM_ANIM_OK;
}

/* Jump to the key frame that starts group g. */
static km_anim_err_t cursor_seek(km_anim_t *a, unsigned g)
{
    km_anim_err_t e = dec_key(a, g);
    if (e == KM_ANIM_OK && last_in_group(a, g * a->info.key_interval)) {
        e = check_tail(a, g);
    }
    if (e != KM_ANIM_OK) {
        return e;
    }
    memcpy(a->prev, a->cur, sizeof a->cur);
    a->dec_idx = (uint16_t)(g * a->info.key_interval);
    a->cur_valid = 1u;
    return KM_ANIM_OK;
}

/* Decode the frame after dec_idx (frame 0 after the last one: only used when the clip loops). */
static km_anim_err_t cursor_step(km_anim_t *a)
{
    unsigned next = (unsigned)a->dec_idx + 1u;
    if (next >= a->info.frames) {
        next = 0;
    }
    const unsigned g = next / a->info.key_interval;
    km_anim_err_t e;
    memcpy(a->prev, a->cur, sizeof a->cur);
    if (next % a->info.key_interval == 0u) {
        e = dec_key(a, g);
    } else {
        e = dec_diff(a, g);
    }
    if (e == KM_ANIM_OK && last_in_group(a, next)) {
        e = check_tail(a, g);
    }
    if (e != KM_ANIM_OK) {
        return e;
    }
    a->dec_idx = (uint16_t)next;
    return KM_ANIM_OK;
}

/* ---------------------------------------------------------------- player */

void km_anim_init(km_anim_t *a, const km_anim_info_t *info)
{
    memset(a, 0, sizeof *a);
    a->info = *info;
    a->state = KM_ANIM_IDLE;
}

void km_anim_start(km_anim_t *a, uint32_t now_ms, int32_t offset_ms, uint8_t flags)
{
    a->start_ms = now_ms;
    a->offset_ms = offset_ms;
    a->flags = (uint8_t)(flags & KM_ANIM_PLAY_MASK);
    a->state = KM_ANIM_PLAYING;
    a->err = KM_ANIM_OK;
    a->has_out = 0;
}

void km_anim_stop(km_anim_t *a)
{
    a->state = KM_ANIM_IDLE;
    a->cur_valid = 0;
    a->has_out = 0;
}

static void fail(km_anim_t *a, km_anim_err_t e)
{
    a->state = KM_ANIM_ERROR;
    a->err = (uint8_t)e;
    a->cur_valid = 0;
}

bool km_anim_tick(km_anim_t *a, uint32_t now_ms, uint8_t out[KM_ANIM_LIGHTS])
{
    a->work = 0;
    if (a->state != KM_ANIM_PLAYING) {
        return false;
    }
    const unsigned n = a->info.frames, key = a->info.key_interval;
    const bool loop = (a->flags & KM_ANIM_PLAY_LOOP) != 0u, blend = (a->flags & KM_ANIM_PLAY_BLEND) != 0u;

    /* clock: position in 1/1000 frame, exact */
    int64_t pos_ms = (int64_t)(uint32_t)(now_ms - a->start_ms) + a->offset_ms;
    if (pos_ms < 0) {
        pos_ms = 0;
    }
    uint64_t x = (uint64_t)pos_ms * a->info.fps;
    const uint64_t total = (uint64_t)n * 1000u;
    if (x >= total) {
        if (!loop) {
            a->state = KM_ANIM_DONE;
            return false;
        }
        x %= total;
    }
    const unsigned f = (unsigned)(x / 1000u), rem = (unsigned)(x % 1000u);
    a->shown = (uint16_t)f;

    /* which frame the decoder has to hold, and how far to mix */
    unsigned want = f, alpha = KM_ANIM_ALPHA_STEPS;
    if (blend) {
        alpha = (rem * KM_ANIM_ALPHA_STEPS) / 1000u;
        want = f + 1u;
        if (want >= n) {
            if (loop) {
                want = 0;
            } else {
                want = n - 1u;
                alpha = KM_ANIM_ALPHA_STEPS;
            }
        }
    }

    /* move the cursor: forward one frame at a time, or jump to the key frame of the wanted group when that is cheaper */
    km_anim_err_t e = KM_ANIM_OK;
    if (!a->cur_valid) {
        e = cursor_seek(a, want / key);
    } else if (a->dec_idx != want) {
        unsigned d;
        if (loop) {
            d = (want + n - a->dec_idx) % n;
        } else {
            d = want > a->dec_idx ? want - a->dec_idx : n;
        }
        if (d > 1u + want % key) {
            e = cursor_seek(a, want / key);
        }
    }
    for (unsigned steps = 0; e == KM_ANIM_OK && a->dec_idx != want && steps < KM_ANIM_MAX_STEPS; steps++) {
        e = cursor_step(a);
    }
    if (e != KM_ANIM_OK) {
        fail(a, e);
        return false;
    }
    if (a->dec_idx != want || !blend) {
        alpha = KM_ANIM_ALPHA_STEPS;      /* behind the clock (or no blending): show the decoded frame as it is */
    }

    if (a->has_out && a->last_idx == a->dec_idx && a->last_alpha == alpha) {
        return false;
    }
    const uint8_t *pal = a->info.palette;
    if (alpha >= KM_ANIM_ALPHA_STEPS) {
        for (unsigned i = 0; i < KM_ANIM_LIGHTS; i++) {
            out[i] = pal[a->cur[i]];
        }
    } else {
        for (unsigned i = 0; i < KM_ANIM_LIGHTS; i++) {
            out[i] = (uint8_t)((pal[a->prev[i]] * (KM_ANIM_ALPHA_STEPS - alpha) + pal[a->cur[i]] * alpha + KM_ANIM_ALPHA_STEPS / 2u) /
                               KM_ANIM_ALPHA_STEPS);
        }
    }
    a->work = (uint16_t)(a->work + KM_ANIM_LIGHTS);
    a->has_out = 1u;
    a->last_idx = a->dec_idx;
    a->last_alpha = (uint8_t)alpha;
    return true;
}

km_anim_state_t km_anim_state(const km_anim_t *a)
{
    return (km_anim_state_t)a->state;
}

km_anim_err_t km_anim_error(const km_anim_t *a)
{
    return (km_anim_err_t)a->err;
}

unsigned km_anim_frame(const km_anim_t *a)
{
    return a->shown;
}

unsigned km_anim_last_work(const km_anim_t *a)
{
    return a->work;
}

/* ---------------------------------------------------------------- full check, in small steps */

void km_anim_verify_begin(km_anim_verify_t *v, const km_anim_info_t *info)
{
    memset(v, 0, sizeof *v);
    km_anim_init(&v->dec, info);
    v->crc = 0xFFFFFFFFu;
}

km_anim_verify_result_t km_anim_verify_step(km_anim_verify_t *v, unsigned budget)
{
    const km_anim_info_t *info = &v->dec.info;
    if (v->phase == 2u) {
        return v->err == KM_ANIM_OK ? KM_ANIM_V_OK : KM_ANIM_V_BAD;
    }
    if (budget == 0u) {
        budget = 1u;
    }
    if (v->phase == 0u) {
        const uint32_t body = info->total_len - 4u;
        uint32_t n = body - v->pos;
        if (n > budget * 256u) {
            n = budget * 256u;
        }
        v->crc = crc_update(v->crc, info->base + v->pos, n);
        v->pos += n;
        if (v->pos == body) {
            if ((v->crc ^ 0xFFFFFFFFu) != info->crc) {
                v->err = KM_ANIM_E_CRC;
                v->phase = 2u;
                return KM_ANIM_V_BAD;
            }
            v->phase = 1u;
        }
        return KM_ANIM_V_RUNNING;
    }
    for (unsigned i = 0; i < budget && v->frame < info->frames; i++) {
        v->dec.work = 0;
        const km_anim_err_t e = (v->frame == 0u) ? cursor_seek(&v->dec, 0u) : cursor_step(&v->dec);
        if (e != KM_ANIM_OK) {
            v->err = (uint8_t)e;
            v->phase = 2u;
            return KM_ANIM_V_BAD;
        }
        v->frame++;
    }
    if (v->frame == info->frames) {
        v->phase = 2u;
        return KM_ANIM_V_OK;
    }
    return KM_ANIM_V_RUNNING;
}

km_anim_err_t km_anim_verify_error(const km_anim_verify_t *v)
{
    return (km_anim_err_t)v->err;
}

km_anim_err_t km_anim_validate(const uint8_t *data, size_t len)
{
    km_anim_info_t info;
    km_anim_err_t e = km_anim_parse(data, len, &info);
    if (e != KM_ANIM_OK) {
        return e;
    }
    km_anim_verify_t v;
    km_anim_verify_begin(&v, &info);
    km_anim_verify_result_t r;
    do {
        r = km_anim_verify_step(&v, 64u);
    } while (r == KM_ANIM_V_RUNNING);
    return r == KM_ANIM_V_OK ? KM_ANIM_OK : km_anim_verify_error(&v);
}
