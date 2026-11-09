/* Scope channel (M3-T2): golden packets, paging, torn reads, publish edge cases, and proof that nothing else changed. */
#include "km/km_scope.h"
#include "km/km_vendor.h"
#include "km_test.h"
#include "golden/scope_vectors.h"

static const km_vendor_info_t k_info = { 0x0104, "Jan  1 2026,00:00:00KMFW-TEST" };

static uint16_t snap_value(int snap, unsigned i)
{
    return (uint16_t)(snap == 0 ? i * 1337u + 11u : i * 0x0201u + 0x7FF0u);
}

static void publish_snap(int snap)
{
    uint16_t v[KM_SCOPE_MAX_VALUES];
    km_scope_reset();
    if (snap == 2) {
        return;
    }
    const unsigned n = snap == 0 ? 76u : 128u;
    for (unsigned i = 0; i < n; i++) {
        v[i] = snap_value(snap, i);
    }
    km_scope_publish(snap == 0 ? 2u : 3u, v, (uint16_t)n);
}

/* A scope request: the wire offset is start | tag << 8, len is the number of reply data bytes the host accepts. */
static void make_req(uint8_t in[KM_HID_REPORT_LEN], unsigned start, unsigned len, unsigned tag)
{
    const uint32_t off = (uint32_t)start | ((uint32_t)tag << 8);
    CHECK(km_hid_build(in, KM_HID_HDR_NORMAL, KM_CMD_SCOPE, 0, off, true, NULL, 0) == KM_HID_OK);
    in[4] = (uint8_t)len;
    in[3] = km_hid_checksum((uint8_t)len, off, NULL);
}

typedef struct { unsigned tag, seq, count, start, n; uint16_t v[KM_SCOPE_PAGE_VALUES]; } page_t;

static bool ask(unsigned start, unsigned len, unsigned tag, page_t *pg)
{
    uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];
    make_req(in, start, len, tag);
    memset(out, 0x77, sizeof out);
    if (!km_vendor_handle(&k_info, in, out)) {
        return false;
    }
    CHECK(out[0] == 0xAA && out[1] == KM_CMD_SCOPE && out[2] == 0);
    CHECK(out[4] >= 6 && (out[4] & 1) == 0 && out[4] <= KM_HID_MAX_DATA);
    CHECK_EQ(out[3], km_hid_checksum(out[4], (uint32_t)out[5] | ((uint32_t)out[6] << 8) | ((uint32_t)out[7] << 16), out + 8));
    for (unsigned i = 8u + out[4]; i < KM_HID_REPORT_LEN; i++) {
        CHECK(out[i] == 0);
    }
    pg->tag = out[8] | (unsigned)(out[9] << 8);
    pg->seq = out[10] | (unsigned)(out[11] << 8);
    pg->count = out[12] | (unsigned)(out[13] << 8);
    pg->start = out[5] | (unsigned)(out[6] << 8) | (unsigned)(out[7] << 16);
    pg->n = (out[4] - 6u) / 2u;
    for (unsigned i = 0; i < pg->n; i++) {
        pg->v[i] = (uint16_t)(out[14 + 2 * i] | (out[15 + 2 * i] << 8));
    }
    return true;
}

/* What a host does: read every page and demand the same tag and seq in all of them. Returns the page count, or 0 on a torn read. */
static unsigned read_all(uint16_t *dst, unsigned *count_out, unsigned *seq_out)
{
    page_t pg;
    unsigned pages = 0, got = 0, seq = 0, count = 0;
    do {
        CHECK(ask(got, 56, 0, &pg));
        if (pages == 0) {
            seq = pg.seq;
            count = pg.count;
        } else if (pg.seq != seq) {
            return 0;
        }
        for (unsigned i = 0; i < pg.n; i++) {
            dst[got + i] = pg.v[i];
        }
        got += pg.n;
        pages++;
    } while (got < count && pg.n != 0);
    *count_out = count;
    *seq_out = seq;
    return pages;
}

static void test_golden(void)
{
    for (unsigned k = 0; k < SCOPE_VECTOR_COUNT; k++) {
        const scope_vector_t *v = &scope_vectors[k];
        uint8_t req[KM_HID_REPORT_LEN], want[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];
        CHECK_EQ(km_hex(v->req_hex, req, sizeof req), KM_HID_REPORT_LEN);
        CHECK_EQ(km_hex(v->reply_hex, want, sizeof want), KM_HID_REPORT_LEN);
        publish_snap(v->snap);
        memset(out, 0x77, sizeof out);
        CHECK(km_vendor_handle(&k_info, req, out));
        if (memcmp(out, want, sizeof want) != 0) {
            printf("  golden mismatch: %s\n    got  %02x%02x%02x%02x%02x%02x%02x%02x...\n", v->name,
                   out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7]);
        }
        CHECK(memcmp(out, want, sizeof want) == 0);
    }
}

static void test_paging(void)
{
    uint16_t got[KM_SCOPE_MAX_VALUES];
    unsigned count, seq;

    publish_snap(0);                                   /* 76 values: 25 + 25 + 25 + 1 */
    CHECK_EQ(read_all(got, &count, &seq), 4);
    CHECK_EQ(count, 76);
    CHECK_EQ(seq, 1);
    for (unsigned i = 0; i < 76; i++) {
        CHECK_EQ(got[i], snap_value(0, i));
    }
    publish_snap(1);                                   /* 128 values: 5 * 25 + 3 */
    CHECK_EQ(read_all(got, &count, &seq), 6);
    CHECK_EQ(count, 128);
    for (unsigned i = 0; i < 128; i++) {
        CHECK_EQ(got[i], snap_value(1, i));
    }

    /* the number of values per reply follows len: (len - 6) / 2, at most 25, never past the end */
    publish_snap(0);
    for (unsigned len = 0; len <= KM_HID_MAX_DATA; len++) {
        page_t pg;
        CHECK(ask(10, len, 0, &pg));
        unsigned want = len >= 6 ? (len - 6) / 2 : 0;
        if (want > 25) {
            want = 25;
        }
        CHECK_EQ(pg.n, want);
        CHECK_EQ(pg.start, 10);
        for (unsigned i = 0; i < pg.n; i++) {
            CHECK_EQ(pg.v[i], snap_value(0, 10 + i));
        }
    }
    /* every start 0..255: values only where start < count, header always correct */
    for (unsigned start = 0; start < 256; start++) {
        page_t pg;
        CHECK(ask(start, 56, 0, &pg));
        unsigned want = start >= 76 ? 0 : 76 - start;
        if (want > 25) {
            want = 25;
        }
        CHECK_EQ(pg.n, want);
        CHECK_EQ(pg.count, 76);
        CHECK_EQ(pg.tag, 2);
    }
    /* a 57-byte length is not a valid 'U' packet: not answered (the parser refuses it) */
    {
        uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];
        make_req(in, 0, 57, 0);
        CHECK(!km_vendor_handle(&k_info, in, out));
        make_req(in, 0, 255, 0);
        CHECK(!km_vendor_handle(&k_info, in, out));
    }
}

static void test_torn_read(void)
{
    page_t a, b;
    uint16_t v[76];
    publish_snap(0);
    CHECK(ask(0, 56, 0, &a));
    for (unsigned i = 0; i < 76; i++) {
        v[i] = (uint16_t)(snap_value(0, i) + 1u);
    }
    km_scope_publish(2, v, 76);                        /* the producer ran between two pages */
    CHECK(ask(25, 56, 0, &b));
    CHECK(b.seq != a.seq);                             /* the host can see the page belongs to another snapshot */
    CHECK_EQ(b.seq, a.seq + 1);
    CHECK_EQ(b.v[0], snap_value(0, 25) + 1u);

    uint16_t got[KM_SCOPE_MAX_VALUES];
    unsigned count, seq;
    CHECK_EQ(read_all(got, &count, &seq), 4);          /* a read with no publish in between is accepted */
    CHECK_EQ(seq, 2);

    /* seq counts every publish, skips 0 on wrap */
    km_scope_reset();
    for (unsigned i = 0; i < 65535; i++) {
        km_scope_publish(5, v, 1);
    }
    CHECK(ask(0, 0, 0, &a));
    CHECK_EQ(a.seq, 0xFFFF);
    km_scope_publish(5, v, 1);
    CHECK(ask(0, 0, 0, &a));
    CHECK_EQ(a.seq, 1);                                /* never 0 once something is published */
    km_scope_publish(5, v, 1);
    CHECK(ask(0, 0, 0, &a));
    CHECK_EQ(a.seq, 2);
}

static void test_publish_edges(void)
{
    page_t pg;
    uint16_t v[1000];
    for (unsigned i = 0; i < 1000; i++) {
        v[i] = (uint16_t)(0xA000u + i);
    }

    /* nothing published yet */
    km_scope_reset();
    CHECK(ask(0, 56, 0, &pg));
    CHECK(pg.tag == 0 && pg.seq == 0 && pg.count == 0 && pg.n == 0);

    /* tag 0 is reserved, NULL data with a count is ignored: snapshot and seq unchanged */
    publish_snap(0);
    km_scope_publish(0, v, 10);
    km_scope_publish(7, NULL, 10);
    CHECK(ask(0, 56, 0, &pg));
    CHECK(pg.tag == 2 && pg.seq == 1 && pg.count == 76 && pg.v[0] == snap_value(0, 0));

    /* count 0 is a valid empty snapshot (NULL allowed) and bumps seq */
    km_scope_publish(9, NULL, 0);
    CHECK(ask(0, 56, 0, &pg));
    CHECK(pg.tag == 9 && pg.seq == 2 && pg.count == 0 && pg.n == 0);

    /* 128 exactly, then 129 and 1000 clamp to 128 and copy only the first 128 values */
    km_scope_publish(0x0100, v, 128);
    CHECK(ask(125, 56, 0, &pg));
    CHECK(pg.count == 128 && pg.n == 3 && pg.v[2] == 0xA000u + 127u);
    km_scope_reset();
    km_scope_publish(0x0100, v, 129);
    CHECK(ask(125, 56, 0, &pg));
    CHECK(pg.count == 128 && pg.n == 3 && pg.v[2] == 0xA000u + 127u);
    CHECK(ask(128, 56, 0, &pg));
    CHECK(pg.count == 128 && pg.n == 0);
    km_scope_publish(0x0100, v, 1000);
    CHECK(ask(125, 56, 0, &pg));
    CHECK(pg.count == 128 && pg.n == 3);
    CHECK(ask(128, 56, 0, &pg));
    CHECK(pg.n == 0);
    km_scope_publish(0x0100, v, 65535);
    CHECK(ask(0, 56, 0, &pg));
    CHECK(pg.count == 128);

    /* a smaller snapshot hides the older, longer one completely */
    km_scope_publish(0x0100, v, 128);
    km_scope_publish(0x0101, v, 5);
    CHECK(ask(0, 56, 0, &pg));
    CHECK(pg.tag == 0x0101 && pg.count == 5 && pg.n == 5);
    CHECK(ask(5, 56, 0, &pg));
    CHECK(pg.n == 0);
    CHECK(ask(100, 56, 0, &pg));
    CHECK(pg.n == 0);

    /* the snapshot is a copy: changing the caller's array afterwards changes nothing */
    v[0] = 0x1111;
    km_scope_publish(0x0102, v, 2);
    v[0] = 0x2222;
    CHECK(ask(0, 56, 0, &pg));
    CHECK(pg.v[0] == 0x1111);

    /* extreme values survive unchanged, both bytes */
    v[0] = 0x0000;
    v[1] = 0xFFFF;
    v[2] = 0x00FF;
    v[3] = 0xFF00;
    km_scope_publish(0xFFFF, v, 4);
    CHECK(ask(0, 56, 0, &pg));
    CHECK(pg.tag == 0xFFFF && pg.v[0] == 0 && pg.v[1] == 0xFFFF && pg.v[2] == 0x00FF && pg.v[3] == 0xFF00);
}

static void test_tag_select(void)
{
    page_t pg;
    publish_snap(0);
    CHECK(ask(0, 56, 2, &pg));                         /* wanted tag = published tag: data */
    CHECK(pg.tag == 2 && pg.n == 25);
    CHECK(ask(0, 56, 0, &pg));                         /* wanted tag 0 = whatever */
    CHECK(pg.n == 25);
    for (unsigned want = 1; want < 0x10000u; want += (want < 8 ? 1u : 257u)) {
        if (want == 2) {
            continue;
        }
        CHECK(ask(0, 56, want, &pg));                  /* another tag: only the header, naming the tag that is there */
        CHECK(pg.tag == 2 && pg.seq == 1 && pg.count == 76 && pg.n == 0);
    }
    CHECK(ask(0, 56, 0x0102, &pg));                    /* wanted tag differing only in the high byte */
    CHECK(pg.n == 0);
    CHECK(ask(0, 56, 0x0200, &pg));                    /* ... or only in the low byte */
    CHECK(pg.n == 0);
}

static void test_robustness(void)
{
    uint8_t in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];

    publish_snap(0);

    /* bad checksum: the stock 0xAB "cscr" answer, with the scope command id, and no scope data */
    make_req(in, 0, 56, 0);
    in[3] ^= 0x01;
    CHECK(km_vendor_handle(&k_info, in, out));
    CHECK(out[0] == 0xAB && out[1] == KM_CMD_SCOPE && out[4] == 4 && memcmp(out + 8, "cscr", 4) == 0);

    /* a raw '_' packet with the scope command is neither answered nor executed */
    CHECK(km_hid_build(in, KM_HID_HDR_RAW, KM_CMD_SCOPE, 0, 0, false, NULL, 0) == KM_HID_OK);
    CHECK(!km_vendor_handle(&k_info, in, out));

    /* an XOR-masked request is decoded like any other 'U' packet and gets the plain reply */
    {
        uint8_t plain[KM_HID_REPORT_LEN];
        make_req(in, 25, 56, 2);
        CHECK(km_vendor_handle(&k_info, in, plain));
        /* km_hid_build only masks the bytes it wrote (len 0), so build the masked packet with len 56 by hand */
        uint8_t raw[KM_HID_REPORT_LEN] = { 0 };
        const uint32_t off = 25u | (2u << 8);
        raw[0] = KM_HID_HDR_NORMAL;
        raw[1] = KM_CMD_SCOPE;
        raw[2] = 0x5A;
        raw[3] = km_hid_checksum(56, off, NULL);
        raw[4] = 56;
        raw[5] = (uint8_t)off;
        raw[6] = (uint8_t)(off >> 8);
        raw[7] = (uint8_t)(off >> 16);
        for (unsigned i = 3; i <= 7u + 56u; i++) {
            raw[i] ^= 0x5A;
        }
        CHECK(km_vendor_handle(&k_info, raw, out));
        CHECK(memcmp(out, plain, KM_HID_REPORT_LEN) == 0);
    }

    /* a request that (wrongly) carries payload bytes is just a request with that length; nothing is stored or echoed */
    {
        uint8_t junk[KM_HID_MAX_DATA];
        memset(junk, 0xEE, sizeof junk);
        CHECK(km_hid_build(in, KM_HID_HDR_NORMAL, KM_CMD_SCOPE, 0, 0, true, junk, 56) == KM_HID_OK);
        CHECK(km_vendor_handle(&k_info, in, out));
        CHECK(out[4] == 56 && out[8] == 2 && out[10] == 1);
        for (unsigned i = 14; i < 64; i += 2) {
            CHECK(!(out[i] == 0xEE && out[i + 1] == 0xEE));
        }
    }

    /* read-only: hundreds of requests of every shape leave the snapshot exactly as published */
    for (unsigned start = 0; start < 256; start += 5) {
        for (unsigned len = 0; len <= 56; len += 7) {
            page_t pg;
            CHECK(ask(start, len, (start & 1u) ? 0u : 2u, &pg));
        }
    }
    {
        uint16_t got[KM_SCOPE_MAX_VALUES];
        unsigned count, seq;
        CHECK_EQ(read_all(got, &count, &seq), 4);
        CHECK(count == 76 && seq == 1);
        for (unsigned i = 0; i < 76; i++) {
            CHECK_EQ(got[i], snap_value(0, i));
        }
    }
}

/* The handler must stay safe even when it is called with a request the parser would never produce (len above 56). */
static void test_handler_direct(void)
{
    km_hid_req_t req;
    uint8_t out[KM_HID_REPORT_LEN];

    publish_snap(1);
    memset(&req, 0, sizeof req);
    req.header = KM_HID_HDR_NORMAL;
    req.cmd = KM_CMD_SCOPE;
    static const uint8_t lens[] = { 56, 57, 58, 59, 60, 255 };
    for (unsigned k = 0; k < sizeof lens; k++) {
        req.len = lens[k];
        req.offset = 0;
        memset(out, 0x77, sizeof out);
        CHECK(km_scope_handle(&req, out));
        CHECK(out[0] == 0xAA && out[4] == 56);               /* 6 header bytes + 25 values, never more */
        CHECK(out[8 + 6 + 49] == (uint8_t)(snap_value(1, 24) >> 8));
    }
    req.offset = 0xFFFFFFu;                                  /* start 255, wanted tag 0xFFFF */
    req.len = 56;
    CHECK(km_scope_handle(&req, out));
    CHECK(out[4] == 6 && out[5] == 0xFF);
    req.offset = 255;                                        /* start 255, any tag */
    CHECK(km_scope_handle(&req, out));
    CHECK(out[4] == 6 && out[5] == 0xFF);
}

/* Nothing else changed: cmd 3 still gives the hand-computed info reply (see test_vendor.c), every other command
 * still answers "unknw", even while a snapshot is published. */
static void test_others_unchanged(void)
{
    static const char stock_reply[] = "aa0300b00b000000" "0201" "4b4d46572d54455354";
    static const km_vendor_info_t stock = { 0x0102, "KMFW-TEST" };
    uint8_t want[KM_HID_REPORT_LEN], in[KM_HID_REPORT_LEN], out[KM_HID_REPORT_LEN];

    publish_snap(1);
    memset(want, 0, sizeof want);
    CHECK_EQ(km_hex(stock_reply, want, sizeof want), 19);
    CHECK(km_hid_build_read(in, KM_CMD_GET_INFO, 0, 31) == KM_HID_OK);
    CHECK(km_vendor_handle(&stock, in, out));
    CHECK(memcmp(out, want, sizeof want) == 0);

    for (unsigned cmd = 0; cmd < 256; cmd++) {
        if (cmd == KM_CMD_GET_INFO || cmd == KM_CMD_SCOPE) {
            continue;
        }
        uint8_t data[KM_HID_MAX_DATA];
        memset(data, 0xA5, sizeof data);
        CHECK(km_hid_build(in, KM_HID_HDR_NORMAL, (uint8_t)cmd, 0, 0x0102, false, data, 56) == KM_HID_OK);
        CHECK(km_vendor_handle(&stock, in, out));
        CHECK(out[0] == 0xAA && out[1] == cmd && out[4] == 5 && memcmp(out + 8, "unknw", 5) == 0);
        CHECK_EQ(out[5], 0x02);
        CHECK_EQ(out[6], 0x01);
        for (unsigned i = 13; i < KM_HID_REPORT_LEN; i++) {
            CHECK(out[i] == 0);
        }
    }

    /* neighbours of the new id, and the other ids the stock firmware uses, are not the scope command */
    static const uint8_t others[] = { 0xC4, 0xC6, 0xB0, 0xB1, 0xC0, 0xCF, 0x45, 0xE0, 0xE1, 0x06 };
    for (unsigned i = 0; i < sizeof others; i++) {
        make_req(in, 0, 56, 0);
        in[1] = others[i];
        CHECK(km_vendor_handle(&k_info, in, out));
        CHECK(out[4] == 5 && memcmp(out + 8, "unknw", 5) == 0);
    }
    CHECK_EQ(KM_CMD_SCOPE, 0xC5);
}

void test_scope(void)
{
    test_golden();
    test_paging();
    test_torn_read();
    test_publish_edges();
    test_tag_select();
    test_robustness();
    test_handler_direct();
    test_others_unchanged();
}
