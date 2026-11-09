/* Light frame stream (command 0xC7, km_stream.[hc], STREAM_PROTOCOL.md): packet handling, sequence numbers, frame assembly,
 * time-outs, flow control, strict validation, fuzzing, and the dispatch through km_vendor_handle.
 *
 * Registration (done by the integrator, tests/km_test.h and tests/test_main.c): void test_stream(void); run("stream", test_stream);
 * Standalone (what tools/mutate_stream.py builds): -DKM_STREAM_STANDALONE adds a main() and the two counters.
 *
 * Expected values are worked out from the specification, not copied from the code; the golden conversation in
 * tests/golden/stream_vectors.h comes from the independent model in tools/magger60_stream.py. */
#include <stdbool.h>

#include "golden/stream_vectors.h"
#include "km/km_scope.h"
#include "km/km_stream.h"
#include "km/km_vendor.h"
#include "km_test.h"

#ifdef KM_STREAM_STANDALONE
int km_test_checks;
int km_test_failures;
size_t km_hex(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (hex[0] != '\0' && hex[1] != '\0' && n < cap) {
        unsigned v;
        char tmp[3] = { hex[0], hex[1], 0 };
        sscanf(tmp, "%2x", &v);
        out[n++] = (uint8_t)v;
        hex += 2;
    }
    return n;
}
#endif

#define OP_START  KM_STREAM_OP_START
#define OP_FRAME  KM_STREAM_OP_FRAME
#define OP_STOP   KM_STREAM_OP_STOP
#define OP_KEEP   KM_STREAM_OP_KEEPALIVE
#define ACK       KM_STREAM_FLAG_ACK_REQ

/* the stream object sits between two guard areas: any write outside it is caught */
static struct {
    uint8_t pre[64];
    km_stream_t s;
    uint8_t post[64];
} g_mem;
#define S (g_mem.s)

static const km_vendor_info_t k_info = { 0x0104, "KMFW-TEST" };

static void guards_set(void)
{
    memset(g_mem.pre, 0xA5, sizeof g_mem.pre);
    memset(g_mem.post, 0x5A, sizeof g_mem.post);
}

static bool guards_ok(void)
{
    for (unsigned i = 0; i < sizeof g_mem.pre; i++) {
        if (g_mem.pre[i] != 0xA5 || g_mem.post[i] != 0x5A) {
            return false;
        }
    }
    return true;
}

static void fresh(void)
{
    guards_set();
    km_stream_init(&S);
    km_vendor_set_stream(&S);
}

/* ---- packet builders: 'U' packet, command 0xC7, sequence number in the offset field */
static void mk(uint8_t out[KM_HID_REPORT_LEN], uint8_t op, uint8_t session, uint16_t seq, uint8_t flags, const uint8_t *body, unsigned blen)
{
    uint8_t d[KM_HID_MAX_DATA];
    CHECK(blen <= KM_HID_MAX_DATA - 3u);
    if (blen > KM_HID_MAX_DATA - 3u) {
        memset(out, 0, KM_HID_REPORT_LEN);
        return;
    }
    d[0] = op;
    d[1] = session;
    d[2] = flags;
    if (blen > 0) {
        memcpy(d + 3, body, blen);
    }
    CHECK(km_hid_build(out, KM_HID_HDR_NORMAL, KM_CMD_STREAM, 0, seq, false, d, (uint8_t)(3 + blen)) == KM_HID_OK);
}

static void mk_start(uint8_t out[KM_HID_REPORT_LEN], uint8_t session, uint16_t seq, uint8_t fmt, uint16_t timeout)
{
    const uint8_t b[5] = { KM_STREAM_PROTO_VERSION, fmt, (uint8_t)timeout, (uint8_t)(timeout >> 8), 0 };
    mk(out, OP_START, session, seq, 0, b, 5);
}

/* the colour of light i, component c, of frame f: unique per frame number, so a frame made of two frames is noticed */
static uint8_t px(unsigned f, unsigned i, unsigned c)
{
    return (uint8_t)(f * 31u + i * 7u + c * 3u + 5u);
}

static void mk_chunk(uint8_t out[KM_HID_REPORT_LEN], uint8_t session, uint16_t seq, uint8_t frame, unsigned first, unsigned count, bool gray, uint8_t flags)
{
    uint8_t b[KM_HID_MAX_DATA];
    unsigned n = 0;
    b[n++] = frame;
    b[n++] = (uint8_t)first;
    b[n++] = (uint8_t)count;
    for (unsigned i = 0; i < count; i++) {
        b[n++] = px(frame, first + i, 0);
        if (!gray) {
            b[n++] = px(frame, first + i, 1);
            b[n++] = px(frame, first + i, 2);
        }
    }
    mk(out, OP_FRAME, session, seq, flags, b, n);
}

static const unsigned k_rgb_first[4] = { 0, 16, 32, 48 };
static const unsigned k_rgb_count[4] = { 16, 16, 16, 13 };

static void mk_rgb_chunk(uint8_t out[KM_HID_REPORT_LEN], uint8_t session, uint16_t seq, uint8_t frame, unsigned chunk, uint8_t flags)
{
    mk_chunk(out, session, seq, frame, k_rgb_first[chunk], k_rgb_count[chunk], false, flags);
}

static bool tx(const uint8_t in[KM_HID_REPORT_LEN], uint8_t out[KM_HID_REPORT_LEN])
{
    memset(out, 0x77, KM_HID_REPORT_LEN);
    return km_vendor_handle(&k_info, in, out);
}

/* one whole frame of 4 RGB chunks, sequence numbers seq+1..seq+4, the last one asks for an acknowledgement; returns the last reply status */
static unsigned send_rgb_frame(uint8_t session, uint16_t seq, uint8_t frame, bool ack, uint8_t rep[KM_HID_REPORT_LEN])
{
    uint8_t in[KM_HID_REPORT_LEN];
    bool got = false;
    for (unsigned c = 0; c < 4; c++) {
        mk_rgb_chunk(in, session, (uint16_t)(seq + 1 + c), frame, c, (ack && c == 3) ? ACK : 0);
        got = tx(in, rep);
    }
    return got ? rep[9] : 0xFFu;
}

static bool frame_is(const uint8_t *f, unsigned fno)
{
    if (f == NULL) {
        return false;
    }
    for (unsigned i = 0; i < KM_STREAM_LIGHTS; i++) {
        for (unsigned c = 0; c < 3; c++) {
            if (f[3 * i + c] != px(fno, i, c)) {
                return false;
            }
        }
    }
    return true;
}

/* reply field access (data starts at byte 8) */
#define R_OP(r)       ((r)[8])
#define R_STATUS(r)   ((r)[9])
#define R_SESSION(r)  ((r)[10])
#define R_STATE(r)    ((r)[11])
#define R_SEQ(r)      ((unsigned)(r)[12] | ((unsigned)(r)[13] << 8))
#define R_FRAME(r)    ((r)[14])
#define R_U16(r, o)   ((unsigned)(r)[8 + (o)] | ((unsigned)(r)[9 + (o)] << 8))
#define R_COMMITTED(r) R_U16(r, 8)
#define R_DROPPED(r)   R_U16(r, 10)
#define R_LOST(r)      R_U16(r, 12)
#define R_STALE(r)     R_U16(r, 14)
#define R_REJECTED(r)  R_U16(r, 16)

static bool reply_wellformed(const uint8_t rep[KM_HID_REPORT_LEN])
{
    const uint8_t *data;
    return km_hid_check_response(rep, &data) == KM_HID_OK && rep[1] == KM_CMD_STREAM && rep[4] == 22 && R_STATUS(rep) <= KM_STREAM_ERR_UNAVAILABLE &&
           R_STATE(rep) <= KM_STREAM_LIVE && rep[8 + 7] == KM_STREAM_WINDOW && rep[8 + 18] == KM_STREAM_PROTO_VERSION && rep[8 + 19] == 3;
}

/* ------------------------------------------------------------------------------------------------------------------ tests */

static void test_dispatch(void)
{
    uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];

    /* without a registered stream 0xC7 is just another unknown command */
    km_vendor_set_stream(NULL);
    mk(in, OP_KEEP, 1, 5, 0, NULL, 0);
    CHECK(tx(in, out));
    CHECK(out[0] == 0xAA && out[1] == 0xC7 && out[4] == 5 && memcmp(out + 8, "unknw", 5) == 0);

    fresh();
    /* cmd 3 and the scope command are untouched by a registered stream */
    CHECK(km_hid_build_read(in, KM_CMD_GET_INFO, 0, 0) == KM_HID_OK);
    CHECK(tx(in, out) && out[0] == 0xAA && out[1] == 3 && out[10] == 'K');
    CHECK(km_hid_build_read(in, KM_CMD_SCOPE, 0, 0) == KM_HID_OK);
    CHECK(tx(in, out) && out[1] == KM_CMD_SCOPE);

    /* the neighbours of 0xC7 are still unknown */
    for (unsigned cmd = 0xC8; cmd <= 0xCF; cmd++) {
        CHECK(km_hid_build(in, KM_HID_HDR_NORMAL, (uint8_t)cmd, 0, 0, false, NULL, 0) == KM_HID_OK);
        CHECK(tx(in, out) && out[4] == 5 && memcmp(out + 8, "unknw", 5) == 0);
    }

    /* a bad checksum is answered by the dispatcher (0xAB, "cscr") and never reaches the stream */
    mk_start(in, 7, 0, 0, 0);
    in[3] ^= 0x10;
    CHECK(tx(in, out) && out[0] == 0xAB && memcmp(out + 8, "cscr", 4) == 0);
    CHECK_EQ(S.state, KM_STREAM_IDLE);

    /* a raw '_' packet is never answered, never executed */
    mk_start(in, 7, 0, 0, 0);
    in[0] = KM_HID_HDR_RAW;
    CHECK(!tx(in, out));
    CHECK_EQ(S.state, KM_STREAM_IDLE);

    /* km_stream_handle on its own: wrong command, NULLs */
    km_hid_req_t req;
    memset(&req, 0, sizeof req);
    req.header = KM_HID_HDR_NORMAL;
    req.cmd = 0x03;
    CHECK(!km_stream_handle(&S, &req, out));
    req.cmd = KM_CMD_STREAM;
    CHECK(!km_stream_handle(NULL, &req, out));
    CHECK(!km_stream_handle(&S, NULL, out));
    CHECK(!km_stream_handle(&S, &req, NULL));
    CHECK(!km_stream_handle(&S, &req, out));          /* len 0: nothing to answer */
    CHECK(guards_ok());
    km_vendor_set_stream(NULL);
}

static void test_reply_layout(void)
{
    uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];
    fresh();
    km_stream_tick(&S, 100);
    mk_start(in, 7, 0x1234, KM_STREAM_FMT_GRAY8, 1000);
    CHECK(tx(in, out));
    /* worked out by hand: aa c7 00 | checksum | len 22 | offset 34 12 00 | data: op 01, status 00, session 07, state OPEN 01,
     * last seq 34 12, ready frame 00, window 16, five zero counters (10 bytes), version 1, formats 0b11, time-out 1000 = e8 03 */
    uint8_t w[KM_HID_REPORT_LEN] = { 0 };
    static const uint8_t d[22] = { 0x01, 0x00, 0x07, 0x01, 0x34, 0x12, 0x00, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x03, 0xE8, 0x03 };
    unsigned sum = 22u + 0x34u + 0x12u;
    for (unsigned k = 0; k < 22; k++) {
        w[8 + k] = d[k];
        sum += d[k];
    }
    w[0] = 0xAA;
    w[1] = 0xC7;
    w[3] = (uint8_t)sum;
    w[4] = 22;
    w[5] = 0x34;
    w[6] = 0x12;
    CHECK(memcmp(out, w, KM_HID_REPORT_LEN) == 0);
    CHECK(reply_wellformed(out));
    CHECK_EQ(R_SESSION(out), 7);
    CHECK_EQ(S.timeout_ms, 1000);
    CHECK_EQ(S.last_seq, 0x1234);

    /* default time-out when the field is 0, and the extremes of the allowed range */
    mk_start(in, 7, 0, 0, 0);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_OK && S.timeout_ms == 500);
    mk_start(in, 7, 0, 0, 100);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_OK && S.timeout_ms == 100);
    mk_start(in, 7, 0, 0, 5000);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_OK && S.timeout_ms == 5000);
    mk_start(in, 7, 0, 0, 99);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_BAD_ARG && S.timeout_ms == 5000);   /* refused: nothing changed */
    mk_start(in, 7, 0, 0, 5001);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_BAD_ARG && S.timeout_ms == 5000);
    CHECK(guards_ok());
    km_vendor_set_stream(NULL);
}

static void test_frames(void)
{
    uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];
    fresh();
    km_stream_tick(&S, 1000);
    mk_start(in, 5, 100, KM_STREAM_FMT_RGB8, 0);
    CHECK(tx(in, out) && R_STATE(out) == KM_STREAM_OPEN);
    CHECK(!km_stream_active(&S) && km_stream_frame(&S) == NULL);

    /* chunks without ACK_REQ get no reply; the frame appears only with its last chunk */
    for (unsigned c = 0; c < 3; c++) {
        mk_rgb_chunk(in, 5, (uint16_t)(101 + c), 1, c, 0);
        CHECK(!tx(in, out));
        CHECK(!km_stream_active(&S) && km_stream_frame(&S) == NULL && S.serial == 0);
    }
    mk_rgb_chunk(in, 5, 104, 1, 3, ACK);
    km_stream_tick(&S, 1010);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_OK && R_STATE(out) == KM_STREAM_LIVE && R_COMMITTED(out) == 1 && R_SEQ(out) == 104 && R_FRAME(out) == 1);
    CHECK(reply_wellformed(out));
    CHECK(km_stream_active(&S) && km_stream_state(&S) == KM_STREAM_LIVE);
    CHECK(frame_is(km_stream_frame(&S), 1));
    CHECK_EQ(km_stream_serial(&S), 1);

    /* the next frame replaces it only when complete: until then the old one stays */
    mk_rgb_chunk(in, 5, 105, 2, 0, 0);
    CHECK(!tx(in, out));
    CHECK(frame_is(km_stream_frame(&S), 1) && km_stream_serial(&S) == 1);
    for (unsigned c = 1; c < 4; c++) {
        mk_rgb_chunk(in, 5, (uint16_t)(105 + c), 2, c, 0);
        CHECK(!tx(in, out));
    }
    CHECK(frame_is(km_stream_frame(&S), 2) && km_stream_serial(&S) == 2);

    /* chunks in any order inside a frame */
    static const unsigned order[4] = { 3, 1, 0, 2 };
    for (unsigned k = 0; k < 4; k++) {
        mk_rgb_chunk(in, 5, (uint16_t)(109 + k), 3, order[k], 0);
        tx(in, out);
    }
    CHECK(frame_is(km_stream_frame(&S), 3) && km_stream_serial(&S) == 3);

    /* a chunk of the frame that is on show, again, with a new sequence number: ignored, no second commit */
    mk_rgb_chunk(in, 5, 113, 3, 0, ACK);
    km_stream_tick(&S, 1100);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_STALE && km_stream_serial(&S) == 3);

    /* the frame number wraps (254, 255, 0, 1 are in order); a jump back by more than 127 counts as "older" */
    fresh();
    km_stream_tick(&S, 2000);
    mk_start(in, 5, 0, KM_STREAM_FMT_RGB8, 0);
    tx(in, out);
    uint16_t sq = 0;
    static const uint8_t wrap[4] = { 254, 255, 0, 1 };
    for (unsigned k = 0; k < 4; k++) {
        km_stream_tick(&S, 2000u + 10u * k);
        CHECK(send_rgb_frame(5, sq, wrap[k], true, out) == KM_STREAM_OK && frame_is(km_stream_frame(&S), wrap[k]) && km_stream_serial(&S) == k + 1);
        sq = (uint16_t)(sq + 4);
    }
    km_stream_tick(&S, 2100);
    mk_rgb_chunk(in, 5, (uint16_t)(sq + 1), 255, 0, ACK);            /* a late chunk of frame 255, newer sequence number */
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_STALE && frame_is(km_stream_frame(&S), 1) && km_stream_serial(&S) == 4);
    CHECK_EQ(S.stats.committed, 4);
    CHECK_EQ(S.stats.stale, 1);
    CHECK(guards_ok());

    /* frame numbers: 127 ahead is newer, 128 ahead is older (the boundary of the 8 bit comparison) */
    fresh();
    km_stream_tick(&S, 3000);
    mk_start(in, 5, 0, KM_STREAM_FMT_RGB8, 0);
    tx(in, out);
    CHECK(send_rgb_frame(5, 0, 10, false, out) == 0xFF && S.serial == 1);
    km_stream_tick(&S, 3010);
    CHECK(send_rgb_frame(5, 4, (uint8_t)(10 + 127), false, out) == 0xFF && S.serial == 2 && frame_is(km_stream_frame(&S), 137));
    km_stream_tick(&S, 3020);
    send_rgb_frame(5, 8, (uint8_t)(137 + 128), false, out);           /* 128 ahead: an older frame, ignored */
    CHECK(S.serial == 2 && frame_is(km_stream_frame(&S), 137) && S.stats.stale == 4);

    /* gray frames: one byte per light, shown as r = g = b */
    fresh();
    mk_start(in, 5, 0, KM_STREAM_FMT_GRAY8, 0);
    tx(in, out);
    mk_chunk(in, 5, 1, 9, 0, 50, true, 0);
    CHECK(!tx(in, out));
    mk_chunk(in, 5, 2, 9, 50, 11, true, ACK);
    km_stream_tick(&S, 50);
    CHECK(tx(in, out) && R_STATE(out) == KM_STREAM_LIVE);
    const uint8_t *f = km_stream_frame(&S);
    CHECK(f != NULL);
    for (unsigned i = 0; f != NULL && i < KM_STREAM_LIGHTS; i++) {
        CHECK(f[3 * i] == px(9, i, 0) && f[3 * i + 1] == f[3 * i] && f[3 * i + 2] == f[3 * i]);
    }
    /* chunks may overlap and may be small: single lights */
    fresh();
    mk_start(in, 5, 0, KM_STREAM_FMT_RGB8, 0);
    tx(in, out);
    for (unsigned i = 0; i < KM_STREAM_LIGHTS; i++) {
        mk_chunk(in, 5, (uint16_t)(1 + i), 4, i, 1, false, 0);
        tx(in, out);
    }
    CHECK(frame_is(km_stream_frame(&S), 4) && S.serial == 1);
    CHECK(guards_ok());
    km_vendor_set_stream(NULL);
}

static void test_sequence(void)
{
    uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];

    /* gaps are counted, the packet after a gap is accepted */
    fresh();
    mk_start(in, 5, 10, 0, 0);
    tx(in, out);
    mk_rgb_chunk(in, 5, 15, 1, 0, ACK);
    km_stream_tick(&S, 10);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_OK && R_LOST(out) == 4 && R_SEQ(out) == 15);

    /* duplicates and older sequence numbers: counted as stale, no effect, never counted as lost */
    mk_rgb_chunk(in, 5, 15, 1, 1, ACK);
    km_stream_tick(&S, 20);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_STALE && R_STALE(out) == 1 && R_SEQ(out) == 15);
    mk_rgb_chunk(in, 5, 12, 1, 1, ACK);
    km_stream_tick(&S, 30);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_STALE && R_STALE(out) == 2 && R_LOST(out) == 4);
    CHECK_EQ(S.mask, 0x000000000000FFFFull);                      /* only the first chunk was applied */
    CHECK(guards_ok());

    /* 16 bit wrap: 0xFFFE, 0xFFFF, 0, 1 are in order; 0x8000 ahead is "older" */
    fresh();
    mk_start(in, 5, 0xFFFE, 0, 0);
    tx(in, out);
    for (unsigned k = 0; k < 4; k++) {
        mk_rgb_chunk(in, 5, (uint16_t)(0xFFFF + k), 1, k, k == 3 ? ACK : 0);
        km_stream_tick(&S, 40);
        const bool got = tx(in, out);
        CHECK(got == (k == 3));
    }
    CHECK(R_STATUS(out) == KM_STREAM_OK && R_LOST(out) == 0 && R_SEQ(out) == 2 && frame_is(km_stream_frame(&S), 1));
    mk_rgb_chunk(in, 5, (uint16_t)(2 + 0x8000), 2, 0, ACK);        /* exactly half the space ahead: treated as old */
    km_stream_tick(&S, 80);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_STALE);
    mk_rgb_chunk(in, 5, (uint16_t)(2 + 0x7FFF), 2, 0, ACK);        /* just under half: ahead, a huge gap, saturating counter */
    km_stream_tick(&S, 90);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_OK && R_LOST(out) == 0x7FFE);
    mk_rgb_chunk(in, 5, (uint16_t)(2 + 2 * 0x7FFF), 2, 1, ACK);
    km_stream_tick(&S, 100);
    CHECK(tx(in, out) && R_LOST(out) == 0xFFFC);                       /* 0x7FFE + 0x7FFE */
    mk_rgb_chunk(in, 5, (uint16_t)(2 + 3 * 0x7FFF), 2, 2, ACK);
    km_stream_tick(&S, 110);
    CHECK(tx(in, out) && R_LOST(out) == 0xFFFF);                       /* saturates, does not wrap */
    CHECK(guards_ok());

    /* an incomplete frame is abandoned when a newer one begins; reordering across frames is harmless */
    fresh();
    mk_start(in, 5, 0, 0, 0);
    tx(in, out);
    mk_rgb_chunk(in, 5, 1, 1, 0, 0);
    tx(in, out);
    mk_rgb_chunk(in, 5, 2, 2, 0, 0);
    tx(in, out);
    CHECK_EQ(S.stats.dropped, 1);
    mk_rgb_chunk(in, 5, 3, 1, 1, 0);                                /* a late chunk of frame 1, newer sequence number */
    tx(in, out);
    CHECK_EQ(S.stats.stale, 1);
    CHECK_EQ(S.mask, 0xFFFFull);
    for (unsigned c = 1; c < 4; c++) {
        mk_rgb_chunk(in, 5, (uint16_t)(3 + c), 2, c, 0);
        tx(in, out);
    }
    CHECK(frame_is(km_stream_frame(&S), 2) && S.stats.committed == 1 && S.stats.dropped == 1);

    /* a frame that never completes is not counted twice, a complete one is never counted as dropped */
    mk_rgb_chunk(in, 5, 8, 3, 0, 0);
    tx(in, out);
    CHECK_EQ(S.stats.dropped, 1);
    mk_rgb_chunk(in, 5, 9, 4, 0, 0);
    tx(in, out);
    CHECK_EQ(S.stats.dropped, 2);
    CHECK(guards_ok());
    km_vendor_set_stream(NULL);
}

static void test_validation(void)
{
    uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];
    fresh();

    /* no session: frame chunks are dropped silently without ACK_REQ, answered SESSION with it; control ops are answered */
    mk_rgb_chunk(in, 5, 1, 1, 0, 0);
    CHECK(!tx(in, out));
    km_stream_tick(&S, 10);
    mk_rgb_chunk(in, 0, 1, 1, 0, ACK);                               /* session 0 is "none": it does not match an idle keyboard either */
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_SESSION && S.state == KM_STREAM_IDLE && S.mask == 0);
    km_stream_tick(&S, 20);
    mk_rgb_chunk(in, 5, 2, 1, 0, ACK);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_SESSION && R_STATE(out) == KM_STREAM_IDLE && R_SESSION(out) == 0);
    mk(in, OP_KEEP, 5, 3, 0, NULL, 0);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_SESSION);
    mk(in, OP_STOP, 5, 4, 0, NULL, 0);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_SESSION);
    CHECK_EQ(S.state, KM_STREAM_IDLE);

    /* START validation: every bad argument is refused and changes nothing */
    uint8_t b[5];
    for (unsigned k = 0; k < 6; k++) {
        b[0] = KM_STREAM_PROTO_VERSION;
        b[1] = 0;
        b[2] = 0;
        b[3] = 0;
        b[4] = 0;
        unsigned session = 5;
        switch (k) {
        case 0: b[0] = 2; break;                 /* version */
        case 1: b[0] = 0; break;
        case 2: b[1] = 2; break;                 /* format */
        case 3: b[4] = 1; break;                 /* reserved byte */
        case 4: session = 0; break;              /* session 0 */
        default: b[2] = 99; break;               /* time-out 99 ms */
        }
        mk(in, OP_START, (uint8_t)session, 0, 0, b, 5);
        CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_BAD_ARG && S.state == KM_STREAM_IDLE);
    }
    mk(in, OP_START, 5, 0, 0, b, 4);                                /* too short */
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_BAD_LENGTH && S.state == KM_STREAM_IDLE);
    uint8_t b6[6] = { 1, 0, 0, 0, 0, 0 };
    mk(in, OP_START, 5, 0, 0, b6, 6);                               /* too long */
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_BAD_LENGTH && S.state == KM_STREAM_IDLE);
    mk(in, OP_START, 5, 0, 0x02, (const uint8_t[5]){ 1, 0, 0, 0, 0 }, 5);   /* reserved flag bit */
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_BAD_ARG && S.state == KM_STREAM_IDLE);
    mk(in, 0x00, 5, 0, 0, NULL, 0);                                 /* unknown ops */
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_UNKNOWN_OP);
    mk(in, 0x10, 5, 0, 0, NULL, 0);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_UNKNOWN_OP && R_OP(out) == 0x10);

    /* masked (XOR) and wide-offset requests are refused */
    {
        const uint8_t d[8] = { OP_START, 5, 0, 1, 0, 0, 0, 0 };
        CHECK(km_hid_build(in, KM_HID_HDR_NORMAL, KM_CMD_STREAM, 0x5A, 0, false, d, 8) == KM_HID_OK);
        CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_BAD_ARG && S.state == KM_STREAM_IDLE);
        CHECK(km_hid_build(in, KM_HID_HDR_NORMAL, KM_CMD_STREAM, 0, 0x10000, true, d, 8) == KM_HID_OK);
        CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_BAD_ARG && S.state == KM_STREAM_IDLE);
    }
    /* fewer than three data bytes: not answered at all */
    {
        const uint8_t d[2] = { OP_START, 5 };
        CHECK(km_hid_build(in, KM_HID_HDR_NORMAL, KM_CMD_STREAM, 0, 0, false, d, 2) == KM_HID_OK);
        CHECK(!tx(in, out));
        CHECK(km_hid_build(in, KM_HID_HDR_NORMAL, KM_CMD_STREAM, 0, 0, false, NULL, 0) == KM_HID_OK);
        CHECK(!tx(in, out));
    }

    /* frame chunk validation inside a session */
    mk_start(in, 5, 0, KM_STREAM_FMT_RGB8, 0);
    CHECK(tx(in, out));
    uint16_t seq = 0;
    const struct { unsigned first, count, blen; unsigned status; } cases[] = {
        { 0, 0, 0, KM_STREAM_ERR_BAD_ARG },                   /* count 0 */
        { 61, 1, 3, KM_STREAM_ERR_BAD_ARG },                  /* first past the end */
        { 60, 2, 6, KM_STREAM_ERR_BAD_ARG },                  /* runs over the end */
        { 255, 1, 3, KM_STREAM_ERR_BAD_ARG },
        { 0, 255, 3, KM_STREAM_ERR_BAD_ARG },
        { 0, 2, 5, KM_STREAM_ERR_BAD_LENGTH },                /* one byte short */
        { 0, 2, 7, KM_STREAM_ERR_BAD_LENGTH },                /* one byte long */
        { 0, 1, 0, KM_STREAM_ERR_BAD_LENGTH },
    };
    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        uint8_t body[KM_HID_MAX_DATA];
        body[0] = 1;
        body[1] = (uint8_t)cases[k].first;
        body[2] = (uint8_t)cases[k].count;
        memset(body + 3, 0xCC, 53);
        mk(in, OP_FRAME, 5, (uint16_t)(seq + 1), ACK, body, 3 + cases[k].blen);
        km_stream_tick(&S, (uint32_t)(100 + 10 * k));
        const bool got = tx(in, out);
        if (!got || R_STATUS(out) != cases[k].status) {
            printf("  case %u: got %d status %u, want %u\n", k, got, got ? R_STATUS(out) : 0u, cases[k].status);
        }
        CHECK(got && R_STATUS(out) == cases[k].status);
        CHECK(S.last_seq == seq && S.mask == 0);                  /* nothing of it was used, not even the sequence number */
    }
    {
        uint8_t body[3] = { 1, 0, 1 };                            /* a frame header only, 6 data bytes in all: 1 light needs 3 pixel bytes */
        mk(in, OP_FRAME, 5, 1, ACK, body, 3);
        km_stream_tick(&S, 200);
        CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_BAD_LENGTH && S.mask == 0);
        mk(in, OP_FRAME, 5, 1, ACK, NULL, 0);                     /* too short for a frame at all */
        km_stream_tick(&S, 210);
        CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_BAD_LENGTH);
        mk(in, OP_FRAME, 5, 1, ACK, b6, 1);                       /* 4 and 5 data bytes: the header of a chunk is 6 */
        km_stream_tick(&S, 212);
        CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_BAD_LENGTH);
        mk(in, OP_FRAME, 5, 1, ACK, b6, 2);
        km_stream_tick(&S, 214);
        CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_BAD_LENGTH);
        mk_rgb_chunk(in, 6, 1, 1, 0, ACK);                        /* another session's chunk */
        km_stream_tick(&S, 220);
        CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_SESSION && S.mask == 0 && S.last_seq == 0);
        mk_rgb_chunk(in, 5, 1, 1, 0, 0x02);                       /* reserved flag */
        CHECK(!tx(in, out));                                      /* no ACK_REQ (0x02 is a reserved bit): refused silently */
        CHECK(S.mask == 0);
    }
    CHECK(S.stats.rejected >= 12);

    /* the valid packet after all that works */
    mk_rgb_chunk(in, 5, 1, 1, 0, 0);
    CHECK(!tx(in, out));
    CHECK_EQ(S.mask, 0xFFFFull);
    CHECK(guards_ok());
    km_vendor_set_stream(NULL);
}

static void test_session_end(void)
{
    uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];

    /* the time-out: alive at 499 ms, over at 500 (default), counted from the last VALID packet */
    fresh();
    km_stream_tick(&S, 1000);
    mk_start(in, 5, 0, 0, 0);
    tx(in, out);
    CHECK(send_rgb_frame(5, 0, 1, false, out) != 0 && km_stream_active(&S));
    km_stream_tick(&S, 1000 + 499);
    CHECK(km_stream_active(&S) && km_stream_frame(&S) != NULL);
    km_stream_tick(&S, 1000 + 500);
    CHECK(!km_stream_active(&S) && km_stream_frame(&S) == NULL && S.state == KM_STREAM_IDLE && S.session == 0);
    /* after the end the old session is gone: its frames and keep-alives are refused */
    km_stream_tick(&S, 1600);
    mk(in, OP_KEEP, 5, 10, 0, NULL, 0);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_SESSION && R_STATE(out) == KM_STREAM_IDLE);
    CHECK(S.state == KM_STREAM_IDLE);

    /* every valid chunk keeps it alive; a keep-alive does too; a refused or stale packet does not */
    fresh();
    km_stream_tick(&S, 0);
    mk_start(in, 5, 0, 0, 200);
    tx(in, out);
    mk_rgb_chunk(in, 5, 1, 1, 0, 0);
    km_stream_tick(&S, 150);
    tx(in, out);                                                   /* valid chunk at 150 ms */
    km_stream_tick(&S, 349);
    CHECK(S.state == KM_STREAM_OPEN);
    km_stream_tick(&S, 350);
    CHECK(S.state == KM_STREAM_IDLE);                              /* 200 ms after the chunk at 150 */

    fresh();
    km_stream_tick(&S, 0);
    mk_start(in, 5, 0, 0, 200);
    tx(in, out);
    mk(in, OP_KEEP, 5, 1, 0, NULL, 0);
    km_stream_tick(&S, 150);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_OK);
    mk(in, OP_KEEP, 5, 1, 0, NULL, 0);                              /* the same keep-alive again: answered STALE, no refresh */
    km_stream_tick(&S, 300);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_STALE);
    km_stream_tick(&S, 349);
    CHECK(S.state == KM_STREAM_OPEN);
    km_stream_tick(&S, 350);
    CHECK(S.state == KM_STREAM_IDLE);

    fresh();
    km_stream_tick(&S, 0);
    mk_start(in, 5, 0, 0, 200);
    tx(in, out);
    mk_rgb_chunk(in, 6, 1, 1, 0, 0);                                /* wrong session: rejected, no refresh */
    km_stream_tick(&S, 150);
    tx(in, out);
    mk_chunk(in, 5, 1, 1, 99, 1, false, 0);                        /* malformed: no refresh */
    km_stream_tick(&S, 190);
    tx(in, out);
    km_stream_tick(&S, 200);
    CHECK(S.state == KM_STREAM_IDLE);

    /* a keep-alive of another session neither keeps this one alive nor is accepted */
    fresh();
    km_stream_tick(&S, 0);
    mk_start(in, 5, 0, 0, 200);
    tx(in, out);
    mk(in, OP_KEEP, 6, 1, 0, NULL, 0);
    km_stream_tick(&S, 150);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_SESSION && R_SESSION(out) == 5 && S.last_seq == 0);
    km_stream_tick(&S, 200);
    CHECK(S.state == KM_STREAM_IDLE);

    /* a restart forgets the frame numbers: the first frame of the new session may be numbered "older" than the last of the old one */
    fresh();
    km_stream_tick(&S, 0);
    mk_start(in, 5, 0, 0, 0);
    tx(in, out);
    send_rgb_frame(5, 0, 3, false, out);
    CHECK(frame_is(km_stream_frame(&S), 3));
    km_stream_tick(&S, 10);
    mk_start(in, 5, 10, 0, 0);
    tx(in, out);
    send_rgb_frame(5, 10, 200, false, out);
    CHECK(frame_is(km_stream_frame(&S), 200) && S.stats.committed == 1);

    /* STOP ends it at once; the counters stay readable; a second STOP is refused */
    fresh();
    km_stream_tick(&S, 0);
    mk_start(in, 5, 0, 0, 0);
    tx(in, out);
    send_rgb_frame(5, 0, 1, false, out);
    CHECK(km_stream_active(&S));
    mk(in, OP_STOP, 4, 5, 0, NULL, 0);                              /* the wrong session cannot stop it */
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_SESSION && km_stream_active(&S));
    mk(in, OP_STOP, 5, 5, 0, NULL, 0);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_OK && R_STATE(out) == KM_STREAM_IDLE && R_COMMITTED(out) == 1);
    CHECK(!km_stream_active(&S) && km_stream_frame(&S) == NULL);
    mk(in, OP_STOP, 5, 6, 0, NULL, 0);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_SESSION);

    /* a new START restarts everything: statistics, frame state, no old frame on show */
    mk_start(in, 8, 500, KM_STREAM_FMT_RGB8, 0);
    CHECK(tx(in, out) && R_COMMITTED(out) == 0 && R_STATE(out) == KM_STREAM_OPEN && !km_stream_active(&S));
    mk_start(in, 9, 7, KM_STREAM_FMT_RGB8, 0);                      /* START again while open: replaces the session */
    CHECK(tx(in, out) && R_SESSION(out) == 9 && S.last_seq == 7);
    mk_rgb_chunk(in, 8, 8, 1, 0, ACK);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_SESSION);   /* the old session id no longer works */

    /* a START in mid-stream drops the frame on show (the light core goes back to its effect until the first new frame) */
    fresh();
    mk_start(in, 5, 0, 0, 0);
    tx(in, out);
    send_rgb_frame(5, 0, 1, false, out);
    CHECK(km_stream_active(&S));
    mk_start(in, 5, 10, 0, 0);
    tx(in, out);
    CHECK(!km_stream_active(&S) && km_stream_frame(&S) == NULL);

    /* 32 malformed packets in a row end the session; 31 do not; a valid one in between restarts the count */
    fresh();
    mk_start(in, 5, 0, 0, 0);
    tx(in, out);
    for (unsigned i = 0; i < KM_STREAM_BAD_RUN_MAX - 1; i++) {
        mk_chunk(in, 5, (uint16_t)(1 + i), 1, 99, 1, false, 0);
        CHECK(!tx(in, out));
    }
    CHECK_EQ(S.state, KM_STREAM_OPEN);
    mk_rgb_chunk(in, 5, 40, 1, 0, 0);                               /* valid: resets the run */
    tx(in, out);
    for (unsigned i = 0; i < KM_STREAM_BAD_RUN_MAX - 1; i++) {
        mk_chunk(in, 5, (uint16_t)(41 + i), 1, 99, 1, false, 0);
        tx(in, out);
    }
    CHECK_EQ(S.state, KM_STREAM_OPEN);
    mk_chunk(in, 5, 80, 1, 99, 1, false, 0);
    tx(in, out);
    CHECK(S.state == KM_STREAM_IDLE && S.session == 0);
    mk_start(in, 5, 0, 0, 0);                                       /* a fresh session works at once */
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_OK && S.state == KM_STREAM_OPEN);

    /* the light core can veto streams */
    fresh();
    mk_start(in, 5, 0, 0, 0);
    tx(in, out);
    send_rgb_frame(5, 0, 1, false, out);
    CHECK(km_stream_active(&S));
    km_stream_set_enabled(&S, false);
    CHECK(!km_stream_active(&S) && S.state == KM_STREAM_IDLE);
    mk_start(in, 5, 0, 0, 0);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_ERR_UNAVAILABLE && S.state == KM_STREAM_IDLE);
    km_stream_set_enabled(&S, true);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_OK && S.state == KM_STREAM_OPEN);
    CHECK(guards_ok());
    km_vendor_set_stream(NULL);
}

static void test_clock(void)
{
    uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];

    /* the 32 bit millisecond clock wraps: only differences count */
    fresh();
    km_stream_tick(&S, 0xFFFFFF00u);
    mk_start(in, 5, 0, 0, 0);
    tx(in, out);
    km_stream_tick(&S, 0xFFFFFFFFu);
    CHECK(S.state == KM_STREAM_OPEN);
    km_stream_tick(&S, 0xFFFFFF00u + 499u);                          /* wrapped past zero by then */
    CHECK(S.state == KM_STREAM_OPEN);
    km_stream_tick(&S, 0xFFFFFF00u + 500u);
    CHECK(S.state == KM_STREAM_IDLE);

    /* a clock that jumps backwards is the safe direction: the stream ends, it does not live forever */
    fresh();
    km_stream_tick(&S, 5000);
    mk_start(in, 5, 0, 0, 0);
    tx(in, out);
    km_stream_tick(&S, 10);
    CHECK(S.state == KM_STREAM_IDLE);

    /* acknowledgements: not more often than every KM_STREAM_ACK_MIN_MS (counted from the previous acknowledgement); the answers
     * to control packets are never held back and do not count */
    fresh();
    km_stream_tick(&S, 100);
    mk_start(in, 5, 0, 0, 0);
    CHECK(tx(in, out));                                              /* control reply */
    mk_rgb_chunk(in, 5, 1, 1, 0, ACK);
    CHECK(tx(in, out));                                              /* ack at 100: the control reply did not count */
    mk_rgb_chunk(in, 5, 2, 1, 1, ACK);
    km_stream_tick(&S, 101);
    CHECK(!tx(in, out));                                             /* 1 ms after the ack: held back */
    mk(in, OP_KEEP, 5, 3, 0, NULL, 0);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_OK);             /* control: answered at once */
    mk_rgb_chunk(in, 5, 4, 1, 2, ACK);
    km_stream_tick(&S, 102);
    CHECK(tx(in, out) && R_STATUS(out) == KM_STREAM_OK);             /* 2 ms after the last ack (a held-back one does not count) */
    mk_rgb_chunk(in, 5, 5, 1, 3, ACK);
    km_stream_tick(&S, 103);
    CHECK(!tx(in, out));
    mk_rgb_chunk(in, 5, 6, 2, 0, ACK);
    km_stream_tick(&S, 104);
    CHECK(tx(in, out));
    CHECK(guards_ok());
    km_vendor_set_stream(NULL);
}

/* ------------------------------------------------------------------------------------------------------------ lossy sender */

typedef struct {
    uint32_t state;
} rng_t;

static uint32_t rnd(rng_t *r)
{
    r->state = r->state * 1664525u + 1013904223u;
    return r->state >> 8;
}

/* A sender that numbers its packets in order but delivers them with loss, duplicates and delays (reordering). Whatever the
 * keyboard shows must always be one whole frame, made of chunks that really were delivered; with no loss every frame is shown. */
static void lossy_run(uint32_t seed, unsigned loss_pct, unsigned dup_pct, unsigned delay_pct, unsigned frames, bool expect_all)
{
    uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN], held[4][KM_HID_REPORT_LEN];
    unsigned held_n = 0;
    uint8_t delivered[256];
    rng_t r = { seed };
    memset(delivered, 0, sizeof delivered);
    fresh();
    uint32_t now = 10;
    km_stream_tick(&S, now);
    uint16_t seq = (uint16_t)rnd(&r);
    mk_start(in, 3, seq, KM_STREAM_FMT_RGB8, 0);
    CHECK(tx(in, out));
    uint32_t last_serial = 0;
    for (unsigned f = 0; f < frames; f++) {
        const uint8_t fno = (uint8_t)(f + 1);
        delivered[fno] = 0;
        for (unsigned c = 0; c < 4; c++) {
            seq++;
            mk_rgb_chunk(in, 3, seq, fno, c, (rnd(&r) % 7 == 0) ? ACK : 0);
            const unsigned roll = rnd(&r) % 100;
            if (roll < loss_pct) {
                continue;
            }
            if (roll < loss_pct + delay_pct && held_n < 4) {
                memcpy(held[held_n++], in, KM_HID_REPORT_LEN);
                continue;
            }
            unsigned copies = (rnd(&r) % 100 < dup_pct) ? 2u : 1u;
            for (unsigned k = 0; k < copies; k++) {
                now += 3;
                km_stream_tick(&S, now);
                tx(in, out);
                delivered[fno] |= (uint8_t)(1u << c);
            }
        }
        if (held_n > 0 && rnd(&r) % 2 == 0) {                       /* release a delayed packet, long after its time */
            memcpy(in, held[--held_n], KM_HID_REPORT_LEN);
            now += 3;
            km_stream_tick(&S, now);
            tx(in, out);
            /* which frame/chunk was it? Recover from the bytes: data[3] = frame, data[4] = first light */
            const unsigned hf = in[8 + 3], hc = in[8 + 4] / 16u;
            delivered[hf] |= (uint8_t)(1u << hc);
        }
        if (S.serial != last_serial) {                              /* a frame was committed: it must be whole and fully delivered */
            CHECK(frame_is(km_stream_frame(&S), S.ready_frame));
            CHECK(delivered[S.ready_frame] == 0x0F);
            last_serial = S.serial;
        }
        CHECK(guards_ok());
    }
    if (expect_all) {
        CHECK_EQ(S.stats.committed, frames);
        CHECK_EQ(S.stats.dropped, 0);
    }
    CHECK(S.stats.committed <= frames);
}

static void test_lossy(void)
{
    lossy_run(1, 0, 0, 0, 200, true);                                 /* perfect link: every frame is shown */
    lossy_run(2, 0, 30, 0, 200, true);                                /* duplicates only: still every frame */
    for (uint32_t seed = 10; seed < 60; seed++) {
        lossy_run(seed, 10, 10, 10, 120, false);
        lossy_run(seed + 1000, 40, 20, 30, 120, false);
        lossy_run(seed + 2000, 0, 0, 40, 120, false);
    }
    km_vendor_set_stream(NULL);
}

/* ---------------------------------------------------------------------------------------------------------------- fuzzing */

static void fuzz_one(rng_t *r, uint8_t in[KM_HID_REPORT_LEN])
{
    for (unsigned i = 0; i < KM_HID_REPORT_LEN; i++) {
        in[i] = (uint8_t)rnd(r);
    }
    const unsigned shape = rnd(r) % 10;
    if (rnd(r) % 3 == 0) {                                            /* a third of the packets are well-formed and aimed at the live session */
        const unsigned kind = rnd(r) % 40;
        const bool gray = S.fmt == KM_STREAM_FMT_GRAY8;
        const uint16_t seq = (uint16_t)(S.last_seq + (rnd(r) % 4 == 0 ? rnd(r) % 5 : 1u));
        const uint8_t session = (S.session != 0 && rnd(r) % 8 != 0) ? S.session : (uint8_t)(1 + rnd(r) % 3);
        if (kind == 0) {
            mk_start(in, session, seq, (uint8_t)(rnd(r) % 2), (uint16_t)(rnd(r) % 2 ? 0 : 100 + rnd(r) % 400));
        } else if (kind == 1) {
            mk(in, OP_KEEP, session, seq, 0, NULL, 0);
        } else if (kind == 2) {
            mk(in, OP_STOP, session, seq, 0, NULL, 0);
        } else {
            const unsigned per = gray ? 50u : 16u;
            const unsigned chunk = rnd(r) % ((KM_STREAM_LIGHTS + per - 1) / per);
            const unsigned first = chunk * per;
            const unsigned count = (KM_STREAM_LIGHTS - first) < per ? (KM_STREAM_LIGHTS - first) : per;
            mk_chunk(in, session, seq, (uint8_t)(1 + rnd(r) % 3), first, count, gray, (uint8_t)(rnd(r) % 4 == 0 ? ACK : 0));
        }
        return;
    }
    if (shape < 8) {
        in[0] = KM_HID_HDR_NORMAL;
        in[1] = KM_CMD_STREAM;
    }
    if (shape < 7) {
        in[2] = 0;
        in[7] = 0;
        const unsigned lenroll = rnd(r) % 4;
        in[4] = (uint8_t)(lenroll == 0 ? rnd(r) % 4 : lenroll == 1 ? 6 + rnd(r) % 52 : lenroll == 2 ? 8 : 3);
        in[8] = (uint8_t)(1 + rnd(r) % 5);
        in[9] = (uint8_t)(rnd(r) % 3);
        in[10] = (uint8_t)(rnd(r) % 4);
        if (in[8] == OP_FRAME && (rnd(r) % 2) == 0 && in[4] >= 6) {
            in[12] = (uint8_t)(rnd(r) % 64);                          /* first */
            in[13] = (uint8_t)(1 + rnd(r) % 20);                      /* count: sometimes right, mostly not */
        }
        if (in[8] == OP_START && (rnd(r) % 2) == 0) {
            in[11] = KM_STREAM_PROTO_VERSION;
            in[12] = (uint8_t)(rnd(r) % 3);
            in[13] = 0;
            in[14] = 0;
            in[15] = 0;
        }
        if (in[4] <= KM_HID_MAX_DATA && (rnd(r) % 8) != 0) {
            in[3] = km_hid_checksum(in[4], (uint32_t)in[5] | ((uint32_t)in[6] << 8) | ((uint32_t)in[7] << 16), in + 8);
        }
    }
}

static void test_fuzz(void)
{
    rng_t r = { 12345 };
    uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];
    fresh();
    uint32_t now = 0;
    unsigned replies = 0, live = 0;
    for (unsigned i = 0; i < 3000000u; i++) {
        fuzz_one(&r, in);
        now += rnd(&r) % 40 == 0 ? 400u : 1u;                           /* sometimes long pauses: time-outs happen */
        km_stream_tick(&S, now);
        const bool got = tx(in, out);
        if (got) {
            replies++;
            if (out[1] == KM_CMD_STREAM) {
                const bool cscr = out[0] == 0xAB && memcmp(out + 8, "cscr", 4) == 0;   /* the dispatcher's own answer to a bad checksum */
                if (!cscr && (out[0] != 0xAA || !reply_wellformed(out))) {
                    CHECK(false);                                         /* one failure is enough to report */
                    break;
                }
            }
        }
        const bool active = km_stream_active(&S);
        live += active ? 1u : 0u;
        if (S.state > KM_STREAM_LIVE || (km_stream_frame(&S) != NULL) != active || (active && S.state != KM_STREAM_LIVE) ||
            (S.state == KM_STREAM_IDLE && S.session != 0) || (S.state != KM_STREAM_IDLE && S.session == 0)) {
            CHECK(false);
            break;
        }
        if (!guards_ok()) {
            CHECK(false);
            break;
        }
    }
    CHECK(replies > 100000 && live > 100000);                              /* the fuzzer did reach the interesting states */
    CHECK(guards_ok());
    km_vendor_set_stream(NULL);
}

/* ----------------------------------------------------------------------------------------------------------- golden replay */

static void test_golden(void)
{
    fresh();
    for (unsigned i = 0; i < KM_STREAM_VECTOR_COUNT; i++) {
        const km_stream_vector_t *v = &k_stream_vectors[i];
        uint8_t in[KM_HID_REPORT_LEN], want[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];
        km_stream_tick(&S, v->now_ms);
        if (v->request[0] != '\0') {
            CHECK_EQ(km_hex(v->request, in, sizeof in), KM_HID_REPORT_LEN);
            const bool got = tx(in, out);
            if (v->reply[0] == '\0') {
                if (got) {
                    printf("  vector %u (%s): unexpected reply\n", i, v->name);
                }
                CHECK(!got);
            } else {
                CHECK_EQ(km_hex(v->reply, want, sizeof want), KM_HID_REPORT_LEN);
                const bool same = got && memcmp(out, want, KM_HID_REPORT_LEN) == 0;
                if (!same) {
                    printf("  vector %u (%s): reply differs\n", i, v->name);
                }
                CHECK(same);
            }
        }
        uint32_t h = 0;
        if (km_stream_frame(&S) != NULL) {
            h = 0x811C9DC5u;
            for (unsigned k = 0; k < KM_STREAM_FRAME_BYTES; k++) {
                h = (h ^ km_stream_frame(&S)[k]) * 0x01000193u;
            }
        }
        if ((unsigned)km_stream_state(&S) != v->state || km_stream_serial(&S) != v->serial || h != v->frame_hash) {
            printf("  vector %u (%s): state %u serial %u hash %08x, want %u %u %08x\n", i, v->name, (unsigned)km_stream_state(&S),
                   (unsigned)km_stream_serial(&S), (unsigned)h, v->state, v->serial, v->frame_hash);
        }
        CHECK_EQ(km_stream_state(&S), v->state);
        CHECK_EQ(km_stream_serial(&S), v->serial);
        CHECK(h == v->frame_hash);
    }
    CHECK(KM_STREAM_VECTOR_COUNT > 30);
    CHECK(guards_ok());
    km_vendor_set_stream(NULL);
}

void test_stream(void)
{
    test_dispatch();
    test_reply_layout();
    test_frames();
    test_sequence();
    test_validation();
    test_session_end();
    test_clock();
    test_lossy();
    test_golden();
    test_fuzz();
}

#ifdef KM_STREAM_STANDALONE
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_stream();
    printf("%d checks, %d failures\n", km_test_checks, km_test_failures);
    return km_test_failures == 0 ? 0 : 1;
}
#endif
