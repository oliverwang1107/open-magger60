/*
 * M5 T-E: km_report_sync on its own, against a plain scripted driver (no health module): when does it send, what does it count,
 * what does it never do. Boundaries are tested exactly so that a mutated constant or comparison fails.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "km/km_report_sync.h"
#include "km_test.h"

#define LEN 4u
#define HB KM_SYNC_HEARTBEAT_CYCLES

typedef struct {
    bool link, ready, accept;
    unsigned sends, recovers;
    uint8_t last[LEN];
    bool recover_frees;
} drv_t;

static bool d_link(void *c)
{
    return ((drv_t *)c)->link;
}

static bool d_ready(void *c)
{
    return ((drv_t *)c)->ready;
}

static bool d_send(void *c, const uint8_t *r, unsigned len)
{
    drv_t *d = (drv_t *)c;
    if (!d->accept || len != LEN) {
        return false;
    }
    memcpy(d->last, r, len);
    d->sends++;
    return true;
}

static void d_recover(void *c)
{
    drv_t *d = (drv_t *)c;
    d->recovers++;
    if (d->recover_frees) {
        d->ready = true;
    }
}

static km_sync_io_t io_of(drv_t *d)
{
    km_sync_io_t io = { d, d_link, d_ready, d_send, d_recover };
    return io;
}

static const uint8_t REL[LEN] = { 0, 0, 0, 0 };
static const uint8_t KEY[LEN] = { 0, 0, 4, 0 };

static void setup(km_report_sync_t *s, drv_t *d, uint32_t t0)
{
    memset(d, 0, sizeof *d);
    d->link = d->ready = d->accept = true;
    km_report_sync_init(s, REL, LEN, t0);
}

static void test_basic_send_rules(uint32_t t0)
{
    km_report_sync_t s;
    drv_t d;
    setup(&s, &d, t0);
    const km_sync_io_t io = io_of(&d);

    km_report_sync_pump(&s, &io, REL, true, t0);                 /* first contact: the host is told, even though it is "released" */
    CHECK_EQ(d.sends, 1);
    CHECK_EQ(s.resyncs, 1);
    km_report_sync_pump(&s, &io, REL, true, t0 + 100u);          /* nothing changed: nothing sent */
    CHECK_EQ(d.sends, 1);
    km_report_sync_pump(&s, &io, KEY, true, t0 + 200u);
    CHECK_EQ(d.sends, 2);
    CHECK_EQ(d.last[2], 4);
    km_report_sync_pump(&s, &io, KEY, true, t0 + 300u);
    CHECK_EQ(d.sends, 2);                                        /* same state: not repeated before the heartbeat */
    km_report_sync_pump(&s, &io, REL, true, t0 + 400u);
    CHECK_EQ(d.sends, 3);
    CHECK_EQ(d.last[2], 0);
}

static void test_heartbeat_boundary(uint32_t t0)
{
    km_report_sync_t s;
    drv_t d;
    setup(&s, &d, t0);
    const km_sync_io_t io = io_of(&d);
    km_report_sync_pump(&s, &io, KEY, true, t0);
    CHECK_EQ(d.sends, 1);
    km_report_sync_pump(&s, &io, KEY, true, t0 + HB - 1u);       /* one cycle early */
    CHECK_EQ(d.sends, 1);
    km_report_sync_pump(&s, &io, KEY, true, t0 + HB);            /* exactly on time */
    CHECK_EQ(d.sends, 2);
    CHECK_EQ(s.heartbeats, 1);
    km_report_sync_pump(&s, &io, KEY, true, t0 + HB + HB - 1u);  /* the period restarts at the heartbeat itself */
    CHECK_EQ(d.sends, 2);
    km_report_sync_pump(&s, &io, KEY, true, t0 + 2u * HB);
    CHECK_EQ(d.sends, 3);
    CHECK_EQ(s.heartbeats, 2);

    /* no heartbeat while everything is released, except the single echo after a release */
    km_report_sync_pump(&s, &io, REL, true, t0 + 2u * HB + 10u);
    CHECK_EQ(d.sends, 4);
    km_report_sync_pump(&s, &io, REL, true, t0 + 2u * HB + 10u + HB - 1u);
    CHECK_EQ(d.sends, 4);
    km_report_sync_pump(&s, &io, REL, true, t0 + 2u * HB + 10u + HB);
    CHECK_EQ(d.sends, 5);                                        /* the echo */
    CHECK_EQ(s.heartbeats, 3);
    km_report_sync_pump(&s, &io, REL, true, t0 + 10u * HB);
    CHECK_EQ(d.sends, 5);                                        /* only once */

    /* a keyboard that was never pressed does not echo */
    setup(&s, &d, t0);
    km_report_sync_pump(&s, &io, REL, true, t0);
    km_report_sync_pump(&s, &io, REL, true, t0 + 10u * HB);
    CHECK_EQ(d.sends, 1);
}

static void test_not_allowed_forces_release(uint32_t t0)
{
    km_report_sync_t s;
    drv_t d;
    setup(&s, &d, t0);
    const km_sync_io_t io = io_of(&d);
    km_report_sync_pump(&s, &io, KEY, true, t0);
    CHECK_EQ(d.last[2], 4);
    km_report_sync_pump(&s, &io, KEY, false, t0 + 10u);          /* a fault: the desired state is ignored */
    CHECK_EQ(d.last[2], 0);
    CHECK_EQ(s.forced_releases, 1);
    for (unsigned i = 1; i < 1000; i++) {
        km_report_sync_pump(&s, &io, KEY, false, t0 + 10u + i * 1000u);
    }
    CHECK_EQ(d.last[2], 0);
    CHECK_EQ(d.sends, 2);                                        /* held at released: no repeats within the echo time */
    km_report_sync_pump(&s, &io, KEY, true, t0 + 2000000u);      /* allowed again: back to the key state */
    CHECK_EQ(d.last[2], 4);
}

static void test_retry_and_busy(uint32_t t0)
{
    km_report_sync_t s;
    drv_t d;
    setup(&s, &d, t0);
    const km_sync_io_t io = io_of(&d);
    km_report_sync_pump(&s, &io, REL, true, t0);
    km_report_sync_pump(&s, &io, KEY, true, t0 + 10u);

    d.accept = false;
    km_report_sync_pump(&s, &io, REL, true, t0 + 20u);
    km_report_sync_pump(&s, &io, REL, true, t0 + 30u);
    CHECK_EQ(s.retries, 2);
    CHECK_EQ(d.last[2], 4);
    d.accept = true;
    km_report_sync_pump(&s, &io, REL, true, t0 + 40u);
    CHECK_EQ(d.last[2], 0);
    CHECK_EQ(s.retries, 2);

    /* busy endpoint: not a retry; recovery exactly at the limit, repeated at every further limit */
    setup(&s, &d, t0);
    d.recover_frees = false;
    km_report_sync_pump(&s, &io, REL, true, t0);
    d.ready = false;
    km_report_sync_pump(&s, &io, KEY, true, t0 + 1000u);
    km_report_sync_pump(&s, &io, KEY, true, t0 + 1000u + KM_SYNC_BUSY_LIMIT_CYCLES - 1u);
    CHECK_EQ(d.recovers, 0);
    km_report_sync_pump(&s, &io, KEY, true, t0 + 1000u + KM_SYNC_BUSY_LIMIT_CYCLES);
    CHECK_EQ(d.recovers, 1);
    CHECK_EQ(s.recoveries, 1);
    CHECK_EQ(s.retries, 0);
    km_report_sync_pump(&s, &io, KEY, true, t0 + 1000u + 2u * KM_SYNC_BUSY_LIMIT_CYCLES - 1u);
    CHECK_EQ(d.recovers, 1);
    km_report_sync_pump(&s, &io, KEY, true, t0 + 1000u + 2u * KM_SYNC_BUSY_LIMIT_CYCLES);
    CHECK_EQ(d.recovers, 2);
    /* a busy spell that ended must not make the next, short one look long */
    setup(&s, &d, t0);
    km_report_sync_pump(&s, &io, REL, true, t0);
    d.ready = false;
    km_report_sync_pump(&s, &io, KEY, true, t0 + 1000u);
    d.ready = true;
    km_report_sync_pump(&s, &io, KEY, true, t0 + 2000u);                       /* delivered */
    CHECK_EQ(d.sends, 2);
    d.ready = false;
    km_report_sync_pump(&s, &io, REL, true, t0 + 10u * KM_SYNC_BUSY_LIMIT_CYCLES);          /* busy again, 1 s later */
    km_report_sync_pump(&s, &io, REL, true, t0 + 10u * KM_SYNC_BUSY_LIMIT_CYCLES + 5000u);
    CHECK_EQ(d.recovers, 0);
    /* nothing due while busy: no recovery */
    setup(&s, &d, t0);
    km_report_sync_pump(&s, &io, REL, true, t0);
    d.ready = false;
    km_report_sync_pump(&s, &io, REL, true, t0 + 10u);
    km_report_sync_pump(&s, &io, REL, true, t0 + 10u + 5u * KM_SYNC_BUSY_LIMIT_CYCLES);
    CHECK_EQ(d.recovers, 0);
}

static void test_dead_detection(uint32_t t0)
{
    km_report_sync_t s;
    drv_t d;
    setup(&s, &d, t0);
    const km_sync_io_t io = io_of(&d);
    km_report_sync_pump(&s, &io, REL, true, t0);
    CHECK(!km_report_sync_stalled(&s, t0));
    d.ready = false;
    d.recover_frees = false;
    km_report_sync_pump(&s, &io, KEY, true, t0 + 100u);
    CHECK(!km_report_sync_stalled(&s, t0 + 100u + KM_SYNC_DEAD_CYCLES - 1u));
    CHECK(km_report_sync_stalled(&s, t0 + 100u + KM_SYNC_DEAD_CYCLES));
    /* the link going away (suspend) clears it */
    d.link = false;
    km_report_sync_pump(&s, &io, KEY, true, t0 + 100u + KM_SYNC_DEAD_CYCLES + 1u);
    CHECK(!km_report_sync_stalled(&s, t0 + 100u + KM_SYNC_DEAD_CYCLES + 2u));
    /* and when it is back the clock starts again */
    d.link = true;
    km_report_sync_pump(&s, &io, KEY, true, t0 + 5u * KM_SYNC_DEAD_CYCLES);
    CHECK(!km_report_sync_stalled(&s, t0 + 5u * KM_SYNC_DEAD_CYCLES + 10u));
    CHECK(km_report_sync_stalled(&s, t0 + 6u * KM_SYNC_DEAD_CYCLES));
    /* a delivered report ends it */
    d.ready = true;
    km_report_sync_pump(&s, &io, KEY, true, t0 + 6u * KM_SYNC_DEAD_CYCLES + 5u);
    CHECK(!km_report_sync_stalled(&s, t0 + 7u * KM_SYNC_DEAD_CYCLES));
}

static void test_link_changes(uint32_t t0)
{
    km_report_sync_t s;
    drv_t d;
    setup(&s, &d, t0);
    const km_sync_io_t io = io_of(&d);
    km_report_sync_pump(&s, &io, KEY, true, t0);
    CHECK_EQ(d.sends, 1);
    d.link = false;
    km_report_sync_pump(&s, &io, KEY, true, t0 + 10u);
    km_report_sync_pump(&s, &io, REL, true, t0 + 20u);
    CHECK_EQ(d.sends, 1);                                        /* nothing is sent while the link is down */
    d.link = true;
    km_report_sync_pump(&s, &io, REL, true, t0 + 30u);
    CHECK_EQ(d.sends, 2);                                        /* resync: sent although "released" was the last state */
    CHECK_EQ(d.last[2], 0);
    CHECK_EQ(s.resyncs, 2);
    km_report_sync_resync(&s);                                   /* e.g. protocol switch */
    km_report_sync_pump(&s, &io, REL, true, t0 + 40u);
    CHECK_EQ(d.sends, 3);

    /* the same state resent after a resume when a key is down */
    d.link = false;
    km_report_sync_pump(&s, &io, KEY, true, t0 + 50u);
    d.link = true;
    km_report_sync_pump(&s, &io, KEY, true, t0 + 60u);
    km_report_sync_pump(&s, &io, KEY, true, t0 + 70u);
    CHECK_EQ(d.sends, 4);
    CHECK_EQ(d.last[2], 4);
}

void test_report_sync(void)
{
    static const uint32_t starts[2] = { 1000u, 0xFFFFF000u };
    for (unsigned i = 0; i < 2; i++) {
        test_basic_send_rules(starts[i]);
        test_heartbeat_boundary(starts[i]);
        test_not_allowed_forces_release(starts[i]);
        test_retry_and_busy(starts[i]);
        test_dead_detection(starts[i]);
        test_link_changes(starts[i]);
    }
}
