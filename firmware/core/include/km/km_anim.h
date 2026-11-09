/*
 * km_anim.h - decoder for a compact, low-resolution animation on the 61 key lights (M6 package P7b).
 * Original work, pure logic: no hardware, no allocation, no waiting; time is passed in as milliseconds.
 *
 * What it is: a "screen" of 61 pixels (the lights, in light order = km_led index 0..60, the serpentine over the five key
 * rows) that plays a clip from a constant byte array. The clip is made by tools/badapple_convert.py; docs/BADAPPLE_NOTES.md
 * (zh-TW) explains the whole chain. This file only decodes: it produces one frame of 61 grey levels (0..255) per call, which the
 * lights code (M6 package P6c) feeds into its "external frame" layer. The grey level is a PRE-limit brightness: the brightness
 * scale and the three hard limits of km_led (per channel, per light, total current) are applied afterwards and are never relaxed.
 *
 * ---- Data format "KMAN" version 1 (all multi-byte fields little endian) ----
 *   0   4  magic "KMAN"
 *   4   1  version = 1
 *   5   1  lights = 61
 *   6   1  bits per sample: 1, 2 or 4          (L = 1 << bits grey steps)
 *   7   1  fps 1..60                           (nominal source frame rate)
 *   8   2  frames n, 1..65535
 *   10  1  key interval K, 1..255              (frame i is a key frame when i % K == 0; groups of K frames)
 *   11  1  flags: bit0 = "loop" hint for tools (play() takes its own flag); other bits must be 0
 *   12  1  kc 0..4   Exp-Golomb order of the change count
 *   13  1  kg 0..4   Exp-Golomb order of the gaps
 *   14  2  groups m = ceil(n / K), at most KM_ANIM_MAX_GROUPS
 *   16  2  data_len, bytes of the frame data (0 < data_len <= 65535)
 *   18  2  reserved, 0
 *   20     palette: L bytes, strictly increasing grey level of every sample value (0..255)
 *          index:   m x u16, byte offset of each group inside the frame data; index[0] = 0, strictly increasing, < data_len
 *          data:    data_len bytes of bit stream (MSB first)
 *          crc32:   u32, CRC-32/IEEE (as zlib.crc32) of everything before it
 *   The total length must be exactly 20 + L + 2m + data_len + 4.
 *
 * ---- Bit stream of one group (starts on a byte boundary, padded with 0 bits to the next boundary) ----
 *   the group is `K` frames (the last group may be shorter): one key frame, then difference frames
 *   key frame:   61 samples of `bits` bits each, in light order
 *   diff frame:  EG(kc) = c, the number of cells that change (0..61, 0 = the picture is unchanged: costs k+1 bits only);
 *                then c times:  EG(kg) = gap (cells skipped since the end of the previous change, or since cell 0),
 *                               the new value of the cell: nothing when bits == 1 (the sample toggles), else `bits` bits;
 *                               the new value must differ from the old one (the encoding is canonical)
 *   EG(k) = Exp-Golomb of order k: q zero bits, a one bit, then q + k bits r; the value is ((2^q - 1) << k) + r (q <= 7).
 *   Everything is checked while decoding: no read past the end of the group, no cell past 61, no no-op change, the group
 *   must end exactly at the next index entry with zero padding bits. Any violation stops the player (state ERROR).
 *
 * ---- Time ----
 *   Frame f is shown from t = f * 1000 / fps ms (computed exactly in 1/1000 frame units, no drift). With the BLEND flag the
 *   output between two frames is the linear mix of their grey levels in 16 steps (needs the next frame decoded one step ahead).
 *   The decoder is a cursor that only moves forward one frame at a time (and jumps to a key frame when it must go backwards,
 *   far ahead, or when the clip starts), at most KM_ANIM_MAX_STEPS frames per call, so every call costs a bounded number of
 *   cells (km_anim_last_work() <= KM_ANIM_WORK_MAX). After a stall the output simply lags a few calls behind the clock.
 */
#ifndef KM_ANIM_H
#define KM_ANIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KM_ANIM_LIGHTS        61u
#define KM_ANIM_VERSION       1u
#define KM_ANIM_HEADER_BYTES  20u
#define KM_ANIM_MAX_GROUPS    1024u
#define KM_ANIM_MAX_DATA      65535u
#define KM_ANIM_MAX_STEPS     8u      /* frames decoded per km_anim_tick() at most */
#define KM_ANIM_WORK_MAX      640u    /* cells touched by one km_anim_tick(): one key frame + the steps + building the output */
#define KM_ANIM_ALPHA_STEPS   16u     /* blend resolution */

#define KM_ANIM_PLAY_LOOP     0x01u   /* start again from frame 0 after the last one */
#define KM_ANIM_PLAY_BLEND    0x02u   /* mix neighbouring frames (time interpolation) */
#define KM_ANIM_PLAY_MASK     0x03u

typedef enum {
    KM_ANIM_OK = 0,
    KM_ANIM_E_ARG,        /* null pointer, or no data */
    KM_ANIM_E_SIZE,       /* shorter than the header, or the total length does not match the header */
    KM_ANIM_E_MAGIC,
    KM_ANIM_E_VERSION,
    KM_ANIM_E_HEADER,     /* a header field is out of range, reserved bits set, counts inconsistent */
    KM_ANIM_E_PALETTE,    /* not strictly increasing */
    KM_ANIM_E_INDEX,      /* group offsets not increasing / outside the data */
    KM_ANIM_E_CRC,        /* checksum mismatch (only reported by the verifier, km_anim_parse() does not compute it) */
    KM_ANIM_E_STREAM,     /* bit stream ended early, bad code, cell index past the last light */
    KM_ANIM_E_NOOP,       /* a listed change that does not change the value */
    KM_ANIM_E_PAD         /* a group does not end exactly where the index says, or its padding bits are not zero */
} km_anim_err_t;

typedef enum {
    KM_ANIM_IDLE = 0,     /* not started, or stopped */
    KM_ANIM_PLAYING,
    KM_ANIM_DONE,         /* reached the end of a clip that does not loop */
    KM_ANIM_ERROR         /* the data is damaged (km_anim_error() says how); the output is no longer updated */
} km_anim_state_t;

/* A parsed, checked header. The pointers point into the caller's data, which must stay valid and unchanged while it is used. */
typedef struct {
    const uint8_t *base;       /* start of the whole file */
    const uint8_t *palette;    /* 1 << bits entries */
    const uint8_t *index;      /* groups * 2 bytes */
    const uint8_t *data;       /* data_len bytes */
    uint32_t total_len;
    uint32_t data_len;
    uint32_t crc;              /* the stored checksum */
    uint16_t frames;
    uint16_t groups;
    uint8_t  bits;
    uint8_t  fps;
    uint8_t  key_interval;
    uint8_t  flags;
    uint8_t  kc;
    uint8_t  kg;
} km_anim_info_t;

/* Check the header, the palette and the index (cost: a few hundred cycles, never more than ~1100 index entries). The frame data
 * is NOT looked at and the checksum is NOT computed; that is km_anim_verify_*(). Returns KM_ANIM_OK or the first problem found. */
km_anim_err_t km_anim_parse(const uint8_t *data, size_t len, km_anim_info_t *info);

typedef struct {
    km_anim_info_t info;
    uint8_t  state;
    uint8_t  err;
    uint8_t  flags;            /* KM_ANIM_PLAY_* */
    uint8_t  has_out;
    uint8_t  cur_valid;
    uint8_t  last_alpha;
    uint16_t dec_idx;          /* frame held in cur[] */
    uint16_t last_idx;         /* frame / alpha of the last output */
    uint16_t shown;            /* frame the clock points at */
    uint16_t work;
    uint32_t bitpos;           /* position in the data after frame dec_idx */
    uint32_t start_ms;
    int32_t  offset_ms;
    uint8_t  cur[KM_ANIM_LIGHTS];     /* sample values (not levels) of frame dec_idx */
    uint8_t  prev[KM_ANIM_LIGHTS];    /* sample values of the frame before it (blend), or the same */
} km_anim_t;

/* Bind a parsed clip. The player is IDLE. */
void km_anim_init(km_anim_t *a, const km_anim_info_t *info);

/* Start (or re-sync) playback: at `now_ms` the clip is at position offset_ms (negative: that long before the start, frame 0 is
 * shown while waiting). `flags` = KM_ANIM_PLAY_*. Calling it while playing moves the clock and keeps the decoder where it is. */
void km_anim_start(km_anim_t *a, uint32_t now_ms, int32_t offset_ms, uint8_t flags);

void km_anim_stop(km_anim_t *a);

/* Advance to now_ms. Returns true when `out` was written with a new frame (61 grey levels, light order); false when nothing
 * changed since the last true, or the player is not PLAYING (then `out` is untouched; look at km_anim_state()). A non-looping
 * clip ends with state DONE on the call at/after its duration. Call it as often as you like (every main-loop pass or once per
 * lights frame); the cost per call is bounded (KM_ANIM_WORK_MAX cells), and it is almost nothing when the frame did not change. */
bool km_anim_tick(km_anim_t *a, uint32_t now_ms, uint8_t out[KM_ANIM_LIGHTS]);

km_anim_state_t km_anim_state(const km_anim_t *a);
km_anim_err_t km_anim_error(const km_anim_t *a);          /* why the state is ERROR (KM_ANIM_OK otherwise) */
unsigned km_anim_frame(const km_anim_t *a);               /* frame the clock pointed at in the last tick */
unsigned km_anim_last_work(const km_anim_t *a);           /* cells touched by the last tick (diagnostics, bounded) */
uint32_t km_anim_duration_ms(const km_anim_info_t *info); /* frames * 1000 / fps, rounded down */

/* Full check in small steps, so that it can run from the main loop: the checksum first (256 bytes per budget unit), then a decode
 * of every frame (one frame per budget unit). Typical use: begin, then call km_anim_verify_step(v, 4) once per main-loop pass until
 * it stops returning KM_ANIM_V_RUNNING. */
typedef enum { KM_ANIM_V_RUNNING = 0, KM_ANIM_V_OK, KM_ANIM_V_BAD } km_anim_verify_result_t;

typedef struct {
    km_anim_t dec;
    uint32_t  crc;
    uint32_t  pos;
    uint16_t  frame;
    uint8_t   phase;           /* 0 checksum, 1 decode, 2 finished */
    uint8_t   err;
} km_anim_verify_t;

void km_anim_verify_begin(km_anim_verify_t *v, const km_anim_info_t *info);
km_anim_verify_result_t km_anim_verify_step(km_anim_verify_t *v, unsigned budget);
km_anim_err_t km_anim_verify_error(const km_anim_verify_t *v);

/* Parse and verify in one call (not bounded in time: for host tools, tests and boot-time checks only). */
km_anim_err_t km_anim_validate(const uint8_t *data, size_t len);

#endif /* KM_ANIM_H */
