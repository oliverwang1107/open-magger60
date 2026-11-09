#include "km/km_stream.h"

#include <string.h>

#define FULL_MASK       ((1ull << KM_STREAM_LIGHTS) - 1ull)
#define COMMON_BYTES    3u        /* op, session, flags */
#define START_BYTES     8u        /* + version, fmt, timeout LE16, reserved */
#define FRAME_HDR_BYTES 6u        /* + frame, first, count */
#define REPLY_BYTES     22u

static void sat_inc(uint16_t *v)
{
    if (*v != 0xFFFFu) {
        (*v)++;
    }
}

static void close_session(km_stream_t *s)
{
    s->state = KM_STREAM_IDLE;
    s->session = 0;
    s->mask = 0;
    s->cur_valid = false;
    s->cur_done = false;
    s->bad_run = 0;
}

void km_stream_init(km_stream_t *s)
{
    memset(s, 0, sizeof *s);
    s->enabled = true;
    s->timeout_ms = KM_STREAM_TIMEOUT_DEFAULT_MS;
}

void km_stream_tick(km_stream_t *s, uint32_t now_ms)
{
    s->now_ms = now_ms;
    if (s->state != KM_STREAM_IDLE && (uint32_t)(now_ms - s->last_rx_ms) >= s->timeout_ms) {
        close_session(s);
    }
}

void km_stream_set_enabled(km_stream_t *s, bool enabled)
{
    s->enabled = enabled;
    if (!enabled) {
        close_session(s);
    }
}

km_stream_state_t km_stream_state(const km_stream_t *s)
{
    return (km_stream_state_t)s->state;
}

bool km_stream_active(const km_stream_t *s)
{
    return s->state == KM_STREAM_LIVE;
}

const uint8_t *km_stream_frame(const km_stream_t *s)
{
    return s->state == KM_STREAM_LIVE ? s->ready : NULL;
}

uint32_t km_stream_serial(const km_stream_t *s)
{
    return s->serial;
}

const km_stream_stats_t *km_stream_stats(const km_stream_t *s)
{
    return &s->stats;
}

/* The status reply. `limited` replies (acknowledgements of frame chunks) are dropped when the previous acknowledgement is
 * too recent; the answers to START / STOP / KEEPALIVE and to errors of those are never dropped and do not count. */
static bool send_reply(km_stream_t *s, const km_hid_req_t *req, uint8_t status, bool limited, uint8_t out[KM_HID_REPORT_LEN])
{
    if (limited && s->replied && (uint32_t)(s->now_ms - s->last_reply_ms) < KM_STREAM_ACK_MIN_MS) {
        return false;
    }
    uint8_t d[REPLY_BYTES];
    d[0]  = req->len > 0 ? req->data[0] : 0;
    d[1]  = status;
    d[2]  = s->state != KM_STREAM_IDLE ? s->session : 0;
    d[3]  = s->state;
    d[4]  = (uint8_t)s->last_seq;
    d[5]  = (uint8_t)(s->last_seq >> 8);
    d[6]  = s->ready_frame;
    d[7]  = (uint8_t)KM_STREAM_WINDOW;
    d[8]  = (uint8_t)s->stats.committed;
    d[9]  = (uint8_t)(s->stats.committed >> 8);
    d[10] = (uint8_t)s->stats.dropped;
    d[11] = (uint8_t)(s->stats.dropped >> 8);
    d[12] = (uint8_t)s->stats.lost;
    d[13] = (uint8_t)(s->stats.lost >> 8);
    d[14] = (uint8_t)s->stats.stale;
    d[15] = (uint8_t)(s->stats.stale >> 8);
    d[16] = (uint8_t)s->stats.rejected;
    d[17] = (uint8_t)(s->stats.rejected >> 8);
    d[18] = (uint8_t)KM_STREAM_PROTO_VERSION;
    d[19] = (uint8_t)((1u << KM_STREAM_FMT_RGB8) | (1u << KM_STREAM_FMT_GRAY8));
    d[20] = (uint8_t)s->timeout_ms;
    d[21] = (uint8_t)(s->timeout_ms >> 8);
    if (limited) {
        s->replied = true;
        s->last_reply_ms = s->now_ms;
    }
    return km_hid_build_response(out, KM_HID_RESP_OK, KM_CMD_STREAM, req->offset & 0xFFFFu, d, REPLY_BYTES) == KM_HID_OK;
}

/* A packet that failed validation: counted, never applied. Too many in a row end the session. */
static bool reject(km_stream_t *s, const km_hid_req_t *req, uint8_t status, bool control, bool want, uint8_t out[KM_HID_REPORT_LEN])
{
    sat_inc(&s->stats.rejected);
    if (s->bad_run < 0xFFu) {
        s->bad_run++;
    }
    if (s->state != KM_STREAM_IDLE && s->bad_run >= KM_STREAM_BAD_RUN_MAX) {
        close_session(s);
    }
    return want && send_reply(s, req, status, !control, out);
}

/* Sequence check shared by FRAME and KEEPALIVE. Returns the gap (0 = in order) or -1 for a repeated or older number. */
static int seq_advance(const km_stream_t *s, uint16_t seq)
{
    const uint16_t diff = (uint16_t)(seq - s->last_seq);
    if (diff == 0u || diff >= 0x8000u) {
        return -1;
    }
    return (int)(diff - 1u);
}

static void seq_accept(km_stream_t *s, uint16_t seq, int gap)
{
    const uint32_t lost = (uint32_t)s->stats.lost + (uint32_t)gap;
    s->stats.lost = lost > 0xFFFFu ? 0xFFFFu : (uint16_t)lost;
    s->last_seq = seq;
    s->bad_run = 0;
    s->last_rx_ms = s->now_ms;
}

static void commit(km_stream_t *s)
{
    memcpy(s->ready, s->staging, sizeof s->ready);
    s->ready_frame = s->cur_frame;
    s->serial++;
    s->stats.committed++;
    s->cur_done = true;
    s->state = KM_STREAM_LIVE;
}

static bool op_start(km_stream_t *s, const km_hid_req_t *req, uint8_t out[KM_HID_REPORT_LEN])
{
    const uint8_t *d = req->data;
    const uint16_t timeout = (uint16_t)(d[5] | (d[6] << 8));
    if (req->len != START_BYTES) {
        return reject(s, req, KM_STREAM_ERR_BAD_LENGTH, true, true, out);
    }
    if (d[1] == 0 || d[3] != KM_STREAM_PROTO_VERSION || d[4] > KM_STREAM_FMT_GRAY8 || d[7] != 0 ||
        (timeout != 0 && (timeout < KM_STREAM_TIMEOUT_MIN_MS || timeout > KM_STREAM_TIMEOUT_MAX_MS))) {
        return reject(s, req, KM_STREAM_ERR_BAD_ARG, true, true, out);
    }
    if (!s->enabled) {
        return send_reply(s, req, KM_STREAM_ERR_UNAVAILABLE, false, out);
    }
    /* a new session: sequence base, empty frame state, fresh statistics */
    memset(&s->stats, 0, sizeof s->stats);
    s->session = d[1];
    s->fmt = d[4];
    s->timeout_ms = timeout != 0 ? timeout : KM_STREAM_TIMEOUT_DEFAULT_MS;
    s->state = KM_STREAM_OPEN;
    s->mask = 0;
    s->cur_valid = false;
    s->cur_done = false;
    s->last_seq = (uint16_t)req->offset;
    s->bad_run = 0;
    s->last_rx_ms = s->now_ms;
    return send_reply(s, req, KM_STREAM_OK, false, out);
}

static bool op_frame(km_stream_t *s, const km_hid_req_t *req, bool want, uint8_t out[KM_HID_REPORT_LEN])
{
    const uint8_t *d = req->data;
    if (req->len < FRAME_HDR_BYTES) {
        return reject(s, req, KM_STREAM_ERR_BAD_LENGTH, false, want, out);
    }
    if (s->state == KM_STREAM_IDLE || d[1] != s->session) {
        return reject(s, req, KM_STREAM_ERR_SESSION, false, want, out);
    }
    const uint8_t frame = d[3];
    const unsigned first = d[4];
    const unsigned count = d[5];
    const unsigned bpp = s->fmt == KM_STREAM_FMT_GRAY8 ? 1u : 3u;
    if (count == 0 || first >= KM_STREAM_LIGHTS || count > KM_STREAM_LIGHTS - first) {
        return reject(s, req, KM_STREAM_ERR_BAD_ARG, false, want, out);
    }
    if (req->len != FRAME_HDR_BYTES + count * bpp) {
        return reject(s, req, KM_STREAM_ERR_BAD_LENGTH, false, want, out);
    }

    const int gap = seq_advance(s, (uint16_t)req->offset);
    if (gap < 0) {
        sat_inc(&s->stats.stale);
        return want && send_reply(s, req, KM_STREAM_ERR_STALE, true, out);
    }
    seq_accept(s, (uint16_t)req->offset, gap);

    const uint8_t fdiff = (uint8_t)(frame - s->cur_frame);
    if (!s->cur_valid || (fdiff != 0 && fdiff < 0x80u)) {
        if (s->cur_valid && s->mask != 0 && !s->cur_done) {
            sat_inc(&s->stats.dropped);                 /* the older frame never completed */
        }
        s->cur_frame = frame;
        s->cur_valid = true;
        s->mask = 0;
        s->cur_done = false;
    } else if (fdiff != 0) {                             /* a chunk of an older frame, arrived late */
        sat_inc(&s->stats.stale);
        return want && send_reply(s, req, KM_STREAM_ERR_STALE, true, out);
    }
    if (s->cur_done) {                                   /* a chunk of the frame that is on show already */
        sat_inc(&s->stats.stale);
        return want && send_reply(s, req, KM_STREAM_ERR_STALE, true, out);
    }

    uint8_t *dst = s->staging + first * 3u;
    const uint8_t *src = d + FRAME_HDR_BYTES;
    if (bpp == 3u) {
        memcpy(dst, src, count * 3u);
    } else {
        for (unsigned i = 0; i < count; i++) {
            dst[3u * i] = dst[3u * i + 1u] = dst[3u * i + 2u] = src[i];
        }
    }
    s->mask |= ((1ull << count) - 1ull) << first;
    if (s->mask == FULL_MASK) {
        commit(s);
    }
    return want && send_reply(s, req, KM_STREAM_OK, true, out);
}

static bool op_stop(km_stream_t *s, const km_hid_req_t *req, uint8_t out[KM_HID_REPORT_LEN])
{
    if (req->len != COMMON_BYTES) {
        return reject(s, req, KM_STREAM_ERR_BAD_LENGTH, true, true, out);
    }
    if (s->state == KM_STREAM_IDLE || req->data[1] != s->session) {
        return reject(s, req, KM_STREAM_ERR_SESSION, true, true, out);
    }
    close_session(s);
    return send_reply(s, req, KM_STREAM_OK, false, out);
}

static bool op_keepalive(km_stream_t *s, const km_hid_req_t *req, uint8_t out[KM_HID_REPORT_LEN])
{
    if (req->len != COMMON_BYTES) {
        return reject(s, req, KM_STREAM_ERR_BAD_LENGTH, true, true, out);
    }
    if (s->state == KM_STREAM_IDLE || req->data[1] != s->session) {
        return reject(s, req, KM_STREAM_ERR_SESSION, true, true, out);
    }
    const int gap = seq_advance(s, (uint16_t)req->offset);
    if (gap < 0) {
        sat_inc(&s->stats.stale);
        return send_reply(s, req, KM_STREAM_ERR_STALE, false, out);   /* answered, but it does not keep the session alive */
    }
    seq_accept(s, (uint16_t)req->offset, gap);
    return send_reply(s, req, KM_STREAM_OK, false, out);
}

bool km_stream_handle(km_stream_t *s, const km_hid_req_t *req, uint8_t out[KM_HID_REPORT_LEN])
{
    if (s == NULL || req == NULL || out == NULL || req->cmd != KM_CMD_STREAM) {
        return false;
    }
    if (req->len < COMMON_BYTES) {                       /* not even op / session / flags: nothing to answer or to name */
        sat_inc(&s->stats.rejected);
        return false;
    }
    const uint8_t op = req->data[0];
    const bool control = op != KM_STREAM_OP_FRAME;
    const bool want = control || (req->data[2] & KM_STREAM_FLAG_ACK_REQ) != 0;

    if (op != KM_STREAM_OP_START && op != KM_STREAM_OP_FRAME && op != KM_STREAM_OP_STOP && op != KM_STREAM_OP_KEEPALIVE) {
        return reject(s, req, KM_STREAM_ERR_UNKNOWN_OP, true, true, out);
    }
    if (req->header != KM_HID_HDR_NORMAL || req->xor_key != 0 || req->offset > 0xFFFFu ||
        (req->data[2] & (uint8_t)~KM_STREAM_FLAG_ACK_REQ) != 0) {
        return reject(s, req, KM_STREAM_ERR_BAD_ARG, control, want, out);
    }
    switch (op) {
    case KM_STREAM_OP_START:     return op_start(s, req, out);
    case KM_STREAM_OP_FRAME:     return op_frame(s, req, want, out);
    case KM_STREAM_OP_STOP:      return op_stop(s, req, out);
    default:                     return op_keepalive(s, req, out);
    }
}
