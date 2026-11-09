/* Raw sample capture (M6 P12a): km_capture on its own, the capture ops of the scope channel against golden packets from the
 * independent Python model, backward compatibility with every existing scope packet, and the safety properties. */
#include <stdlib.h>

#include "km/km_capture.h"
#include "km/km_capture_scope.h"
#include "km/km_vendor.h"
#include "km_test.h"
#include "golden/capture_vectors.h"
#include "golden/scope_vectors.h"

static const km_vendor_info_t k_info = { 0x0104, "Jan  1 2026,00:00:00KMFW-NOISE-TEST" };

/* ---- helpers: fake DMA words ---- */

/* slot s lives in word s/2: even slots in the low half, odd slots in the high half */
static void put_slot(uint32_t words[38], unsigned slot, uint32_t value)
{
    uint32_t *w = &words[slot >> 1];
    if ((slot & 1u) != 0u) {
        *w = (*w & 0x0000FFFFu) | (value << 16);
    } else {
        *w = (*w & 0xFFFF0000u) | (value & 0xFFFFu);
    }
}

static void push(km_capture_t *c, const uint32_t words[38], uint32_t now, bool cond)
{
    (void)km_capture_push_frame(c, words, &words[36], now, cond);
}

/* ---- km_capture alone ---- */

static void test_basics(void)
{
    km_capture_t *c = (km_capture_t *)calloc(1, sizeof *c);
    uint32_t w[38] = { 0 };
    uint16_t st[KM_CAPTURE_STATUS_WORDS];
    uint16_t out[64];

    km_capture_init(c, 240);
    CHECK_EQ(c->state, KM_CAPTURE_IDLE);
    CHECK_EQ(km_capture_value_count(c), 0);
    CHECK_EQ(km_capture_capacity_frames(c), 0);
    CHECK(!km_capture_control(c, KM_CAPTURE_CTRL_ONESHOT));           /* nothing selected */
    CHECK(!km_capture_control(c, 7));
    CHECK(!km_capture_push_frame(c, w, &w[36], 1, false));            /* not running: nothing recorded */
    CHECK_EQ(c->frames, 0);

    /* selection rules */
    CHECK(!km_capture_select(c, 4, 5));
    CHECK(!km_capture_select(c, 0, 76));
    CHECK(!km_capture_select(c, 0, 200));
    CHECK(km_capture_select(c, 0, 75));
    CHECK(km_capture_select(c, 1, 0));
    CHECK_EQ(c->n_sel, 2);
    CHECK(km_capture_select(c, 1, KM_CAPTURE_NO_SLOT));                /* the list ends at the first "no slot" */
    CHECK_EQ(c->n_sel, 1);
    CHECK(km_capture_select(c, 3, 9));                                 /* a slot behind a gap does not count */
    CHECK_EQ(c->n_sel, 1);
    CHECK_EQ(km_capture_capacity_frames(c), 16384);
    const uint8_t four[4] = { 1, 2, 3, 4 };
    CHECK(km_capture_set_slots(c, four, 4));
    CHECK_EQ(c->n_sel, 4);
    CHECK_EQ(km_capture_capacity_frames(c), 4096);
    CHECK(!km_capture_set_slots(c, four, 5));
    const uint8_t bad[2] = { 1, 76 };
    CHECK(!km_capture_set_slots(c, bad, 2));
    CHECK_EQ(c->n_sel, 4);                                             /* a refused call changes nothing */
    CHECK(!km_capture_set_slots(c, NULL, 2));
    CHECK(km_capture_set_slots(c, NULL, 0));
    CHECK_EQ(c->n_sel, 0);

    /* one slot of each kind: even low half, odd high half, the two injected words */
    const uint8_t sl[4] = { 4, 5, 72, 75 };
    CHECK(km_capture_set_slots(c, sl, 4));
    CHECK(km_capture_control(c, KM_CAPTURE_CTRL_ONESHOT));
    CHECK_EQ(c->state, KM_CAPTURE_RUNNING);
    CHECK_EQ(c->run, 1);
    CHECK(!km_capture_select(c, 0, 9));                                /* no selection change while running */
    const uint8_t two[2] = { 1, 2 };
    CHECK(!km_capture_set_slots(c, two, 2));
    CHECK_EQ(c->n_sel, 4);
    CHECK_EQ(km_capture_value_count(c), 0);                            /* a running capture cannot be read */
    CHECK_EQ(km_capture_read(c, 0, 4, out), 0);
    for (unsigned f = 0; f < 3; f++) {
        put_slot(w, 4, 1000u + f);
        put_slot(w, 5, 2000u + f);
        put_slot(w, 72, 3000u + f);
        put_slot(w, 75, 0x1FFFu + f);                                  /* more than 12 bits: only the low 12 are kept */
        push(c, w, 5000u + 100u * f, f == 1);
    }
    CHECK_EQ(c->frames, 3);
    CHECK_EQ(km_capture_value_count(c), 0);                            /* still running: frames are in, but nothing is readable */
    CHECK_EQ(km_capture_read(c, 0, 4, out), 0);
    CHECK(km_capture_control(c, KM_CAPTURE_CTRL_STOP));
    CHECK_EQ(c->state, KM_CAPTURE_DONE);
    CHECK_EQ(km_capture_value_count(c), 12);
    CHECK_EQ(km_capture_read(c, 0, 64, out), 12);
    for (unsigned f = 0; f < 3; f++) {
        const unsigned flag = f == 1 ? KM_CAPTURE_FLAG : 0u;
        CHECK_EQ(out[f * 4 + 0], (1000u + f) | flag);
        CHECK_EQ(out[f * 4 + 1], (2000u + f) | flag);
        CHECK_EQ(out[f * 4 + 2], (3000u + f) | flag);
        CHECK_EQ(out[f * 4 + 3], ((0x1FFFu + f) & 0xFFFu) | flag);
    }
    /* partial reads */
    CHECK_EQ(km_capture_read(c, 5, 3, out), 3);
    CHECK_EQ(out[0], 2001u | KM_CAPTURE_FLAG);
    CHECK_EQ(km_capture_read(c, 11, 5, out), 1);
    CHECK_EQ(km_capture_read(c, 12, 5, out), 0);
    CHECK_EQ(km_capture_read(c, 0, 4, NULL), 0);

    km_capture_status(c, st);
    CHECK_EQ(st[KM_CAP_ST_VERSION], 1);
    CHECK_EQ(st[KM_CAP_ST_STATE], KM_CAPTURE_DONE);
    CHECK_EQ(st[KM_CAP_ST_NSEL], 4);
    CHECK_EQ(st[KM_CAP_ST_SLOT0 + 2], 72);
    CHECK_EQ(st[KM_CAP_ST_RUN], 1);
    CHECK_EQ(st[KM_CAP_ST_CAP_FRAMES], 4096);
    CHECK_EQ(st[KM_CAP_ST_FRAMES_LO], 3);
    CHECK_EQ(st[KM_CAP_ST_AVAIL], 3);
    CHECK_EQ(st[KM_CAP_ST_T_FIRST_LO], 5000);
    CHECK_EQ(st[KM_CAP_ST_T_LAST_LO], 5200);
    CHECK_EQ(st[KM_CAP_ST_COND_LO], 1);
    CHECK_EQ(st[KM_CAP_ST_RING], 0);
    CHECK_EQ(st[KM_CAP_ST_CPU_MHZ], 240);

    /* stop when not running changes nothing; stop of an armed run without frames goes back to idle */
    CHECK(km_capture_control(c, KM_CAPTURE_CTRL_STOP));
    CHECK_EQ(c->state, KM_CAPTURE_DONE);
    CHECK(km_capture_control(c, KM_CAPTURE_CTRL_ONESHOT));
    CHECK_EQ(c->run, 2);
    CHECK_EQ(c->frames, 0);                                            /* the old recording is gone */
    CHECK(km_capture_control(c, KM_CAPTURE_CTRL_STOP));
    CHECK_EQ(c->state, KM_CAPTURE_IDLE);
    CHECK_EQ(km_capture_value_count(c), 0);

    /* a new selection makes a finished recording unreadable */
    CHECK(km_capture_control(c, KM_CAPTURE_CTRL_ONESHOT));
    push(c, w, 1, false);
    CHECK(km_capture_control(c, KM_CAPTURE_CTRL_STOP));
    CHECK_EQ(c->state, KM_CAPTURE_DONE);
    CHECK(km_capture_select(c, 0, 8));
    CHECK_EQ(c->state, KM_CAPTURE_IDLE);
    CHECK_EQ(km_capture_value_count(c), 0);
    free(c);
}

static void test_oneshot_and_ring(void)
{
    km_capture_t *c = (km_capture_t *)calloc(1, sizeof *c);
    uint32_t w[38] = { 0 };
    uint16_t out[KM_CAPTURE_SAMPLES];
    km_capture_init(c, 240);
    const uint8_t s4[4] = { 10, 11, 12, 13 };
    CHECK(km_capture_set_slots(c, s4, 4));

    /* one-shot: stops by itself after exactly capacity frames and ignores later frames */
    CHECK(km_capture_control(c, KM_CAPTURE_CTRL_ONESHOT));
    for (unsigned f = 0; f < 4096u + 10u; f++) {
        put_slot(w, 10, f & 0xFFFu);
        put_slot(w, 11, (f * 3u) & 0xFFFu);
        put_slot(w, 12, 4095u - (f & 0xFFFu));
        put_slot(w, 13, f % 7u);
        const bool took = km_capture_push_frame(c, w, &w[36], f, false);
        CHECK(took == (f < 4096u));
    }
    CHECK_EQ(c->state, KM_CAPTURE_DONE);
    CHECK_EQ(c->frames, 4096);
    CHECK_EQ(km_capture_value_count(c), KM_CAPTURE_SAMPLES);
    CHECK_EQ(km_capture_read(c, 0, KM_CAPTURE_SAMPLES, out), KM_CAPTURE_SAMPLES);
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[4095u * 4u], 4095u & 0xFFFu);
    CHECK_EQ(out[4095u * 4u + 2u], 4095u - (4095u & 0xFFFu));
    CHECK_EQ(out[100u * 4u + 1u], 300);

    /* ring: 1 slot, 16384 frames, run 20000 frames; the readout starts at the oldest frame */
    const uint8_t s1[1] = { 3 };
    CHECK(km_capture_set_slots(c, s1, 1));
    CHECK(km_capture_control(c, KM_CAPTURE_CTRL_RING));
    CHECK_EQ(c->run, 2);
    for (unsigned f = 0; f < 20000u; f++) {
        put_slot(w, 3, f & 0xFFFu);
        CHECK(km_capture_push_frame(c, w, &w[36], f, (f & 1u) != 0u));
    }
    CHECK_EQ(c->state, KM_CAPTURE_RUNNING);                            /* a ring run never stops by itself */
    CHECK(km_capture_control(c, KM_CAPTURE_CTRL_STOP));
    CHECK_EQ(c->state, KM_CAPTURE_DONE);
    CHECK_EQ(km_capture_value_count(c), 16384);
    CHECK_EQ(km_capture_read(c, 0, KM_CAPTURE_SAMPLES, out), KM_CAPTURE_SAMPLES);
    for (unsigned i = 0; i < KM_CAPTURE_SAMPLES; i += 997u) {          /* frame 20000 - 16384 + i, flag = odd frame number */
        const unsigned fr = 20000u - 16384u + i;
        CHECK_EQ(out[i], (fr & 0xFFFu) | ((fr & 1u) != 0u ? KM_CAPTURE_FLAG : 0u));
    }
    CHECK_EQ(out[16383], (19999u & 0xFFFu) | KM_CAPTURE_FLAG);
    uint16_t st[KM_CAPTURE_STATUS_WORDS];
    km_capture_status(c, st);
    CHECK_EQ(st[KM_CAP_ST_FRAMES_LO], 20000);
    CHECK_EQ(st[KM_CAP_ST_AVAIL], 16384);
    CHECK_EQ(st[KM_CAP_ST_RING], 1);
    CHECK_EQ(st[KM_CAP_ST_COND_LO], 10000);

    /* a ring run that has not wrapped yet starts at frame 0 */
    CHECK(km_capture_control(c, KM_CAPTURE_CTRL_RING));
    for (unsigned f = 0; f < 5u; f++) {
        put_slot(w, 3, 100u + f);
        push(c, w, f, false);
    }
    CHECK(km_capture_control(c, KM_CAPTURE_CTRL_STOP));
    CHECK_EQ(km_capture_read(c, 0, 10, out), 5);
    CHECK_EQ(out[0], 100);
    CHECK_EQ(out[4], 104);

    /* run counter: 0 is "never armed", it wraps 0xFFFF -> 1 */
    c->run = 0xFFFFu;
    CHECK(km_capture_control(c, KM_CAPTURE_CTRL_RING));
    CHECK_EQ(c->run, 1);
    free(c);
}

/* ---- the scope ops ---- */

static void make_req(uint8_t in[KM_HID_REPORT_LEN], const char *hex)
{
    CHECK_EQ(km_hex(hex, in, KM_HID_REPORT_LEN), KM_HID_REPORT_LEN);
}

/* A scope request as the host builds it: offset = start | tag << 8, len = reply data bytes accepted, no payload. */
static void scope_req(uint8_t in[KM_HID_REPORT_LEN], unsigned tag, unsigned start, unsigned len)
{
    const uint32_t off = (uint32_t)start | ((uint32_t)tag << 8);
    CHECK(km_hid_build(in, KM_HID_HDR_NORMAL, KM_CMD_SCOPE, 0, off, true, NULL, 0) == KM_HID_OK);
    in[4] = (uint8_t)len;
    in[3] = km_hid_checksum((uint8_t)len, off, NULL);
}

/* the capture every golden vector starts from (see firmware/tools/gen_capture_vectors.py) */
static void vector_state(km_capture_t *c)
{
    km_capture_init(c, 240);
    const uint8_t s[2] = { 5, 12 };
    CHECK(km_capture_set_slots(c, s, 2));
    for (unsigned r = 0; r < 3; r++) {                                 /* three arms: run counter 3 */
        CHECK(km_capture_control(c, KM_CAPTURE_CTRL_ONESHOT));
        if (r < 2) {
            CHECK(km_capture_control(c, KM_CAPTURE_CTRL_STOP));
        }
    }
    uint32_t w[38] = { 0 };
    for (unsigned f = 0; f < 100u; f++) {
        put_slot(w, 5, (f * 31u + 0u * 7u + 1000u) & 0xFFFu);
        put_slot(w, 12, (f * 31u + 1u * 7u + 1000u) & 0xFFFu);
        push(c, w, 1000u + f * 10300u, (f % 5u) == 0u);
    }
    CHECK(km_capture_control(c, KM_CAPTURE_CTRL_STOP));
}

static void test_golden(void)
{
    km_capture_t *c = (km_capture_t *)calloc(1, sizeof *c);
    CHECK(CAPTURE_VECTOR_COUNT >= 20);
    for (unsigned k = 0; k < CAPTURE_VECTOR_COUNT; k++) {
        const capture_vector_t *v = &capture_vectors[k];
        uint8_t req[KM_HID_REPORT_LEN], want[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];
        make_req(req, v->req_hex);
        make_req(want, v->reply_hex);
        vector_state(c);
        km_scope_reset();
        km_scope_set_capture(&km_capture_scope_ops, c);
        memset(out, 0x77, sizeof out);
        CHECK(km_vendor_handle(&k_info, req, out));
        if (memcmp(out, want, sizeof want) != 0) {
            printf("  golden mismatch: %s\n    got  ", v->name);
            for (unsigned i = 0; i < 24; i++) {
                printf("%02x", out[i]);
            }
            printf("\n    want ");
            for (unsigned i = 0; i < 24; i++) {
                printf("%02x", want[i]);
            }
            printf("\n");
        }
        CHECK(memcmp(out, want, sizeof want) == 0);
    }
    km_scope_set_capture(NULL, NULL);
    free(c);
}

/* A host reads a whole recording through pages, like tools/magger60_noise.py does. */
static void test_read_through_pages(void)
{
    km_capture_t *c = (km_capture_t *)calloc(1, sizeof *c);
    vector_state(c);
    km_scope_reset();
    km_scope_set_capture(&km_capture_scope_ops, c);
    unsigned got = 0, page = 0;
    uint16_t all[300];
    for (;;) {
        uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];
        scope_req(in, 0xC300u | (page >> 8), page & 0xFFu, 56);
        CHECK(km_vendor_handle(&k_info, in, out));
        const unsigned n = (out[4] - 6u) / 2u;
        CHECK_EQ(out[10] | (out[11] << 8), 3);                         /* run counter */
        CHECK_EQ(out[12] | (out[13] << 8), 200);                       /* 100 frames x 2 columns */
        if (n == 0) {
            break;
        }
        for (unsigned i = 0; i < n; i++) {
            all[got + i] = (uint16_t)(out[14 + 2 * i] | (out[15 + 2 * i] << 8));
        }
        got += n;
        page++;
    }
    CHECK_EQ(got, 200);
    CHECK_EQ(page, 8);
    for (unsigned f = 0; f < 100u; f++) {
        for (unsigned col = 0; col < 2u; col++) {
            const unsigned want = ((f * 31u + col * 7u + 1000u) & 0xFFFu) | ((f % 5u) == 0u ? KM_CAPTURE_FLAG : 0u);
            CHECK_EQ(all[f * 2u + col], want);
        }
    }
    km_scope_set_capture(NULL, NULL);
    free(c);
}

/* Without a registered capture the capture tags are just tags that are not published: the answer is what the code before this
 * change gave. With one registered, every packet of the old scope golden set is answered byte for byte as before. */
static void test_backward_compatible(void)
{
    km_capture_t *c = (km_capture_t *)calloc(1, sizeof *c);
    vector_state(c);
    for (unsigned reg = 0; reg < 2; reg++) {
        km_scope_set_capture(reg ? &km_capture_scope_ops : NULL, c);
        for (unsigned k = 0; k < SCOPE_VECTOR_COUNT; k++) {
            const scope_vector_t *v = &scope_vectors[k];
            uint8_t req[KM_HID_REPORT_LEN], want[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];
            make_req(req, v->req_hex);
            make_req(want, v->reply_hex);
            uint16_t vals[KM_SCOPE_MAX_VALUES];
            km_scope_reset();
            if (v->snap != 2) {
                const unsigned n = v->snap == 0 ? 76u : 128u;
                for (unsigned i = 0; i < n; i++) {
                    vals[i] = (uint16_t)(v->snap == 0 ? i * 1337u + 11u : i * 0x0201u + 0x7FF0u);
                }
                km_scope_publish(v->snap == 0 ? 2u : 3u, vals, (uint16_t)n);
            }
            CHECK(km_vendor_handle(&k_info, req, out));
            CHECK(memcmp(out, want, sizeof want) == 0);
        }
    }
    /* unregistered: every capture op gets the plain "this tag is not published" header (tag/seq/count of the snapshot) */
    km_scope_set_capture(NULL, NULL);
    km_scope_reset();
    uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];
    for (unsigned i = 0; i < 4; i++) {
        scope_req(in, 0xC000u | (i << 8), i == 1 ? 0x0Au : 0x00u, 56);
        CHECK(km_vendor_handle(&k_info, in, out));
        CHECK_EQ(out[4], 6);
        for (unsigned b = 8; b < 14; b++) {
            CHECK(out[b] == 0);
        }
    }
    /* a published snapshot + a capture op without a capture: the header names the published tag, no values */
    uint16_t v3[3] = { 1, 2, 3 };
    km_scope_publish(0x0201u, v3, 3);
    scope_req(in, 0xC000u, 0, 56);
    CHECK(km_vendor_handle(&k_info, in, out));
    CHECK_EQ(out[4], 6);
    CHECK_EQ(out[8] | (out[9] << 8), 0x0201);
    km_scope_set_capture(NULL, NULL);
    free(c);
}

/* Safety: the capture ops cannot change anything but the capture's own state, and bad arguments are refused. */
static void test_ops_are_contained(void)
{
    km_capture_t *c = (km_capture_t *)calloc(1, sizeof *c);
    vector_state(c);
    km_scope_reset();
    km_scope_set_capture(&km_capture_scope_ops, c);
    uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];
    uint16_t pub[2] = { 7, 8 };
    km_scope_publish(0x0100u, pub, 2);
    /* a spread of page numbers (all of 0..65535 would take long): every answer is well formed and in range */
    for (unsigned page = 0; page < 0x10000u; page += 251u) {
        scope_req(in, 0xC300u | (page >> 8), page & 0xFFu, 56);
        CHECK(km_vendor_handle(&k_info, in, out));
        CHECK_EQ(out[0], 0xAA);
        CHECK(out[4] <= 56 && (out[4] & 1u) == 0u && out[4] >= 6);
    }
    /* every control byte and every select argument: only the capture changes, the published snapshot never */
    for (unsigned b = 0; b < 256u; b++) {
        scope_req(in, 0xC200u, b, 56);
        CHECK(km_vendor_handle(&k_info, in, out));
        CHECK(c->state != KM_CAPTURE_RUNNING || b == 1u || b == 2u);
        (void)km_capture_control(c, KM_CAPTURE_CTRL_STOP);
        scope_req(in, 0xC100u | (b & 7u), b, 56);
        CHECK(km_vendor_handle(&k_info, in, out));
    }
    scope_req(in, 0, 0, 56);
    CHECK(km_vendor_handle(&k_info, in, out));
    CHECK_EQ(out[8] | (out[9] << 8), 0x0100);
    CHECK_EQ(out[10] | (out[11] << 8), 1);
    CHECK_EQ(out[14] | (out[15] << 8), 7);
    CHECK_EQ(out[16] | (out[17] << 8), 8);
    /* selecting while a run is going is refused and the reply tells (the slots in the status block did not change) */
    CHECK(km_capture_set_slots(c, (const uint8_t[]){ 5, 12 }, 2));
    CHECK(km_capture_control(c, KM_CAPTURE_CTRL_ONESHOT));
    scope_req(in, 0xC100u, 20, 56);
    CHECK(km_vendor_handle(&k_info, in, out));
    CHECK_EQ(out[14 + 2 * KM_CAP_ST_STATE], KM_CAPTURE_RUNNING);
    CHECK_EQ(out[14 + 2 * KM_CAP_ST_SLOT0], 5);
    km_scope_set_capture(NULL, NULL);
    free(c);
}

void test_capture(void)
{
    test_basics();
    test_oneshot_and_ring();
    test_golden();
    test_read_through_pages();
    test_backward_compatible();
    test_ops_are_contained();
}
