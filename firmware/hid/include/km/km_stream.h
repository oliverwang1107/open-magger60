/*
 * km_stream.h - light frame stream: a PC tool sends complete frames of 61 light colours to the keyboard in real time
 * (vendor command 0xC7, M6 work package P7a). Hardware independent; the byte-level format, the error codes and worked
 * examples are in STREAM_PROTOCOL.md, this header is the C side of the same specification.
 *
 * What this module guarantees:
 *   - It only ever writes its own RAM (about 450 bytes) and the reply buffer. No flash, no register, no other module.
 *   - Every request is bounded work: at most one 64 byte report parsed, at most 48 bytes copied, no loop over anything
 *     the host controls. It never waits for anything, so it can neither block the key path nor the USB endpoint.
 *   - A frame becomes visible only when all 61 lights of one frame number have arrived ("ready" is assembled in a
 *     separate buffer and copied in one go): the lights never show half a frame. Lost, repeated, late and reordered
 *     packets are counted and cost at most the frame they belong to.
 *   - The stream ends by itself: no valid packet of the session for `timeout` ms (default 500) and the state falls back
 *     to IDLE, which tells the light core to show its own effect again. A host that crashes, is unplugged or sleeps
 *     therefore never leaves the lights stuck. A flood of malformed packets (32 in a row) ends the session too.
 *   - It does not know about brightness: whatever frame it hands over goes through the light core's three hard limits
 *     (km_led.h) like every other effect. The stream cannot raise any of them.
 *   - The light core can veto streams: km_stream_set_enabled(s, false) (light fault, lights off) closes the session and
 *     refuses a new one until it is set back to true.
 *
 * The part the light core (P6c) consumes, all of it called from the main loop:
 *     km_stream_tick(s, now_ms);                     once per pass, before the USB poll if possible (clock + timeout)
 *     if (km_stream_active(s)) {                     LIVE: a complete frame is on show
 *         const uint8_t *rgb = km_stream_frame(s);   KM_STREAM_FRAME_BYTES = 61 * 3 bytes, light 0..60, r g b
 *         ... copy it in ONE go (about 60 cycles), do not keep the pointer across calls ...
 *         km_stream_serial(s)                        changes whenever a new frame was committed
 *     } else { ...the normal effect... }
 * Light i is light i of km_led.h (km_led_x8 / km_led_row order). Gray frames (one byte per light) are expanded to
 * r = g = b by this module; the colour order is r, g, b as the lights are meant to look, the wire order (GRB) is the
 * light core's business.
 *
 * Time: the handler takes the time of the last km_stream_tick(). The consumer calls tick every pass (it has now_ms for
 * its own frame pacing anyway). If the main loop stops calling tick, the clock stands still and nothing expires,
 * exactly as nothing else in the firmware moves either.
 */
#ifndef KM_STREAM_H
#define KM_STREAM_H

#include <stdbool.h>
#include <stdint.h>

#include "km/km_hid.h"

#define KM_STREAM_LIGHTS            61u    /* = KM_LED_COUNT (km_led.h); P6c asserts the equality */
#define KM_STREAM_FRAME_BYTES       (KM_STREAM_LIGHTS * 3u)
#define KM_STREAM_PROTO_VERSION     1u

#define KM_STREAM_TIMEOUT_DEFAULT_MS 500u
#define KM_STREAM_TIMEOUT_MIN_MS     100u
#define KM_STREAM_TIMEOUT_MAX_MS     5000u
#define KM_STREAM_WINDOW             16u   /* most packets the host may have sent without an acknowledgement (advertised in every reply) */
#define KM_STREAM_ACK_EVERY          8u    /* the host asks for an acknowledgement on every 8th packet (advice, in STREAM_PROTOCOL.md) */
#define KM_STREAM_ACK_MIN_MS         2u    /* an ACK_REQ is not answered if the previous acknowledgement was sent less than this long ago */
#define KM_STREAM_BAD_RUN_MAX        32u   /* this many malformed packets in a row end the session */

/* Pixel formats of a session (START byte). */
#define KM_STREAM_FMT_RGB8          0u     /* 3 bytes per light: r g b            */
#define KM_STREAM_FMT_GRAY8         1u     /* 1 byte per light, shown as r = g = b */

/* Operations (data byte 0 of a request; echoed in the reply). */
typedef enum {
    KM_STREAM_OP_START     = 0x01,   /* open (or restart) a session                          */
    KM_STREAM_OP_FRAME     = 0x02,   /* one chunk of a frame                                 */
    KM_STREAM_OP_STOP      = 0x03,   /* close the session now                                */
    KM_STREAM_OP_KEEPALIVE = 0x04    /* keeps the session alive and asks for the status      */
    /* 0x10..0x1F are reserved for the animation player (start with a music offset, stop, status; P7b/P10) */
} km_stream_op_t;

#define KM_STREAM_FLAG_ACK_REQ      0x01u  /* data byte 2 of every request: please answer this packet */

/* Status byte of every reply (data byte 1). */
typedef enum {
    KM_STREAM_OK = 0,
    KM_STREAM_ERR_UNKNOWN_OP,   /* 1 operation not defined                                               */
    KM_STREAM_ERR_BAD_LENGTH,   /* 2 data length is not the one this operation needs                      */
    KM_STREAM_ERR_BAD_ARG,      /* 3 a field is out of range or a reserved field is not zero              */
    KM_STREAM_ERR_SESSION,      /* 4 no such session (never started, timed out, stopped, or another one)  */
    KM_STREAM_ERR_STALE,        /* 5 sequence number repeated or older than one already accepted: ignored */
    KM_STREAM_ERR_UNAVAILABLE   /* 6 the light core does not accept a stream right now                    */
} km_stream_status_t;

typedef enum {
    KM_STREAM_IDLE = 0,   /* nothing: the normal effect is shown                         */
    KM_STREAM_OPEN = 1,   /* session started, no complete frame yet: still the normal effect */
    KM_STREAM_LIVE = 2    /* a complete frame is available and the session is alive      */
} km_stream_state_t;

typedef struct {
    uint16_t committed;   /* complete frames made visible (wraps)                                     */
    uint16_t dropped;     /* frames abandoned incomplete because a newer one began                    */
    uint16_t lost;        /* packets missing in the sequence (gaps), saturating                       */
    uint16_t stale;       /* repeated, late or reordered packets ignored, saturating                  */
    uint16_t rejected;    /* malformed or wrong-session packets, saturating                           */
} km_stream_stats_t;

typedef struct {
    uint32_t now_ms;           /* time of the last km_stream_tick() */
    uint32_t last_rx_ms;       /* time of the last valid packet of the session */
    uint32_t last_reply_ms;    /* time of the last acknowledgement of a frame chunk (rate limit) */
    uint32_t serial;           /* counts committed frames, starts at 0 */
    uint64_t mask;             /* lights of the frame under assembly that have arrived */
    uint16_t timeout_ms;
    uint16_t last_seq;         /* newest accepted sequence number (START's counts as the base) */
    uint8_t  state;            /* km_stream_state_t */
    uint8_t  session;
    uint8_t  fmt;
    uint8_t  cur_frame;        /* frame number under assembly */
    uint8_t  ready_frame;      /* frame number of the visible frame */
    uint8_t  bad_run;          /* malformed packets in a row */
    bool     enabled;
    bool     cur_valid;        /* a frame number has been seen since START */
    bool     cur_done;         /* the frame under assembly has been committed already */
    bool     replied;          /* last_reply_ms is valid */
    km_stream_stats_t stats;
    uint8_t  staging[KM_STREAM_FRAME_BYTES];
    uint8_t  ready[KM_STREAM_FRAME_BYTES];
} km_stream_t;

/* All state to IDLE, enabled, statistics zero. */
void km_stream_init(km_stream_t *s);

/* Main loop, once per pass: set the clock and end the session when its time ran out. */
void km_stream_tick(km_stream_t *s, uint32_t now_ms);

/* Allow or refuse streams. false closes a running session at once (and counts nothing). */
void km_stream_set_enabled(km_stream_t *s, bool enabled);

km_stream_state_t km_stream_state(const km_stream_t *s);
/* True while a complete frame of a live session is available (state LIVE). */
bool km_stream_active(const km_stream_t *s);
/* The visible frame (KM_STREAM_FRAME_BYTES, r g b per light) or NULL unless active. Valid until the next call into
 * this module; copy it at once. */
const uint8_t *km_stream_frame(const km_stream_t *s);
uint32_t km_stream_serial(const km_stream_t *s);
const km_stream_stats_t *km_stream_stats(const km_stream_t *s);

/* Answer a decoded KM_CMD_STREAM request (km_vendor_handle does the dispatch). Returns true and fills `out` when a
 * reply has to be sent; most frame packets get none. */
bool km_stream_handle(km_stream_t *s, const km_hid_req_t *req, uint8_t out[KM_HID_REPORT_LEN]);

#endif
