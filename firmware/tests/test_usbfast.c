/* 8 kHz work: the slow-work deferral bound, the endpoint busy-time buckets, and the report-sync latency counters. */
#include <stdint.h>
#include <string.h>

#include "km/km_report_sync.h"
#include "km/km_usb.h"
#include "km/km_usbfast.h"
#include "km_test.h"

static void test_defer(void)
{
    km_defer_t d;
    km_defer_init(&d);
    CHECK(km_defer_slow_ok(&d, false, 0u, 1000u));           /* no event: runs */
    CHECK(!km_defer_slow_ok(&d, true, 100u, 1000u));         /* event: skipped */
    CHECK(!km_defer_slow_ok(&d, true, 600u, 1000u));
    CHECK_EQ(d.skipped, 2);
    CHECK(km_defer_slow_ok(&d, true, 1100u, 1000u));         /* window of 1000 over: forced through */
    CHECK_EQ(d.forced, 1);
    CHECK(!km_defer_slow_ok(&d, true, 1200u, 1000u));        /* restarted */
    CHECK(km_defer_slow_ok(&d, false, 1300u, 1000u));        /* quiet pass ends the window */
    CHECK(!km_defer_slow_ok(&d, true, 1400u, 1000u));        /* a new window starts at 1400 */
    CHECK(!km_defer_slow_ok(&d, true, 2399u, 1000u));
    CHECK(km_defer_slow_ok(&d, true, 2400u, 1000u));

    /* starvation bound: events on every pass (wrapping counter), the work still runs once per max_cycles */
    km_defer_init(&d);
    unsigned runs = 0;
    uint32_t t = 0xFFFFF000u;
    for (unsigned i = 0; i < 10000u; i++, t += 100u) {
        if (km_defer_slow_ok(&d, true, t, 240000u)) {
            runs++;
        }
    }
    CHECK(runs >= 4 && runs <= 5);                           /* 10000 * 100 = 1e6 cycles / 240000 */
}

static void test_ustat(void)
{
    km_ustat_t u;
    km_ustat_init(&u);
    km_ustat_observe(&u, 10u, true, 0u);                         /* nothing sent: nothing measured */
    CHECK_EQ(u.count, 0);
    km_ustat_sent(&u, 1000u, 100u);
    km_ustat_observe(&u, 2000u, false, 100u);                      /* still busy */
    CHECK_EQ(u.count, 0);
    km_ustat_observe(&u, 1000u + 15000u, true, 100u);              /* exactly 62.5 us: bucket 0 */
    CHECK_EQ(u.bucket[0], 1);
    km_ustat_observe(&u, 99999u, true, 500u);                      /* already counted: no double count */
    CHECK_EQ(u.count, 1);
    const uint32_t edges[] = { 15001u, 30000u, 60000u, 120000u, 240000u, 240001u, 5000000u };
    const unsigned want[] = { 1, 1, 2, 3, 4, 5, 5 };
    uint32_t t = 0xFFFFFF00u;                                /* wrap-safe */
    for (unsigned i = 0; i < 7; i++) {
        km_ustat_sent(&u, t, 0xFFFFFFFEu + i);
        km_ustat_observe(&u, t + edges[i], true, 0xFFFFFFFEu + i + i);   /* wrap-safe micro-frame counter: i micro-frames */
        CHECK_EQ(u.last, edges[i]);
        CHECK(u.bucket[want[i]] >= 1);
    }
    CHECK_EQ(u.count, 8);
    CHECK_EQ(u.max, 5000000u);
    uint16_t v[KM_USTAT_VALUES];
    km_ustat_pack(&u, 240u, v);
    CHECK_EQ(v[0], 8);
    CHECK_EQ(v[7], 20833u);                                  /* 5e6 / 240 */
    unsigned total = 0;
    for (unsigned i = 1; i <= KM_USTAT_BUCKETS; i++) {
        total += v[i];
    }
    CHECK_EQ(total, 8);
}

static void test_uframes_and_cfg(void)
{
    km_ustat_t u;
    km_ustat_init(&u);
    const uint32_t d[] = { 0, 1, 2, 3, 4, 5, 7, 8, 40 };
    for (unsigned i = 0; i < 9; i++) {
        km_ustat_sent(&u, 0u, 1000u + i);
        km_ustat_observe(&u, 10u, true, 1000u + i + d[i]);
    }
    CHECK_EQ(u.uf[0], 1); CHECK_EQ(u.uf[1], 1); CHECK_EQ(u.uf[2], 1);
    CHECK_EQ(u.uf[3], 2); CHECK_EQ(u.uf[4], 2); CHECK_EQ(u.uf[5], 2);
    CHECK_EQ(u.uf_max, 40);
    uint16_t v[KM_USTAT_VALUES];
    km_ustat_pack_uf(&u, v);
    CHECK_EQ(v[5], 2); CHECK_EQ(v[6], 9); CHECK_EQ(v[7], 40);

    /* the interval the descriptor really carries */
    size_t n = 0;
    const uint8_t *c = km_usb_config_desc_for(KM_USB_VARIANT_PLAIN, &n);
    CHECK_EQ(km_usb_cfg_binterval(c, (unsigned)n, KM_USB_EP_KBD_IN), KM_USB_BINTERVAL_KBD);
    CHECK_EQ(km_usb_cfg_binterval(c, (unsigned)n, KM_USB_EP_NKRO_IN), KM_USB_BINTERVAL_NKRO);
    CHECK_EQ(km_usb_cfg_binterval(c, (unsigned)n, KM_USB_EP_VND_IN), KM_USB_BINTERVAL_VND);
    CHECK_EQ(km_usb_cfg_binterval(c, (unsigned)n, 0x7Fu), 0);
    CHECK_EQ(km_usb_cfg_binterval(c, 12u, KM_USB_EP_KBD_IN), 0);          /* truncated */
}

/* a scripted endpoint: ready only when the test says so */
typedef struct { bool ready; unsigned sends; } fake_t;
static bool f_link(void *c) { (void)c; return true; }
static bool f_ready(void *c) { return ((fake_t *)c)->ready; }
static bool f_send(void *c, const uint8_t *r, unsigned n) { (void)r; (void)n; ((fake_t *)c)->sends++; return true; }
static void f_rec(void *c) { (void)c; }

static void test_sync_latency_counters(void)
{
    fake_t f = { true, 0 };
    const km_sync_io_t io = { &f, f_link, f_ready, f_send, f_rec };
    const uint8_t rel[2] = { 1, 0 }, a[2] = { 1, 4 }, b[2] = { 1, 8 };
    km_report_sync_t s;
    km_report_sync_init(&s, rel, 2, 0u);
    km_report_sync_pump(&s, &io, rel, true, 10u);            /* the first (forced) report */
    CHECK_EQ(s.sent, 1);

    /* a press while the endpoint is free: sent in the same pass, wait 0, not late */
    km_report_sync_pump(&s, &io, a, true, 1000u);
    CHECK_EQ(s.sent, 2);
    CHECK_EQ(s.wait_last, 0);
    CHECK_EQ(s.deferred, 0);
    CHECK_EQ(s.late, 0);

    /* a change while the endpoint is busy: one deferred episode, accepted 20000 cycles later: not late */
    f.ready = false;
    km_report_sync_pump(&s, &io, b, true, 2000u);
    km_report_sync_pump(&s, &io, b, true, 5000u);            /* still the same episode */
    CHECK_EQ(s.deferred, 1);
    CHECK_EQ(s.sent, 2);
    f.ready = true;
    km_report_sync_pump(&s, &io, b, true, 22000u);
    CHECK_EQ(s.sent, 3);
    CHECK_EQ(s.wait_last, 20000u);
    CHECK_EQ(s.late, 0);
    CHECK_EQ(s.coalesced, 0);

    /* busy for longer than 125 us: late; and the target moves on while waiting: coalesced */
    f.ready = false;
    km_report_sync_pump(&s, &io, a, true, 30000u);
    km_report_sync_pump(&s, &io, rel, true, 40000u);         /* a changed to released while waiting */
    CHECK_EQ(s.coalesced, 1);
    f.ready = true;
    km_report_sync_pump(&s, &io, rel, true, 70000u);
    CHECK_EQ(s.sent, 4);
    CHECK_EQ(s.late, 1);
    CHECK_EQ(s.wait_max, 40000u);

    /* press and release both inside one busy window: back to what the host holds, nothing is sent, but it is counted */
    f.ready = false;
    km_report_sync_pump(&s, &io, a, true, 100000u);
    km_report_sync_pump(&s, &io, rel, true, 100500u);
    CHECK_EQ(s.coalesced, 2);
    CHECK_EQ(s.sent, 4);
    f.ready = true;
    km_report_sync_pump(&s, &io, rel, true, 101000u);
    CHECK_EQ(s.sent, 4);
    CHECK(!s.undelivered);
}

void test_usbfast(void)
{
    test_defer();
    test_ustat();
    test_uframes_and_cfg();
    test_sync_latency_counters();
}
