/*
 * M6 latency probe (km_latency): when is a press counted, what is a sample, what is dropped, how the values are packed.
 * Pure logic; the same code is built into the probe image (km_m6p) and nothing else uses it.
 */
#include <stdint.h>

#include "km/km_latency.h"
#include "km_test.h"

static void test_press_edges(void)
{
    km_lat_t l;
    km_lat_init(&l);
    uint8_t a[1] = { 5 };
    uint8_t ab[2] = { 5, 9 };

    km_lat_observe(&l, 100u, a, 1);                 /* slot 5 goes down */
    CHECK_EQ(l.presses, 1);
    CHECK_EQ(l.pending, 1);
    km_lat_observe(&l, 110u, a, 1);                 /* held: not a new press */
    CHECK_EQ(l.presses, 1);
    km_lat_observe(&l, 120u, ab, 2);                /* slot 9 joins */
    CHECK_EQ(l.presses, 2);
    CHECK_EQ(l.pending, 2);
    km_lat_observe(&l, 130u, NULL, 0);              /* both released */
    km_lat_observe(&l, 140u, a, 1);                 /* slot 5 pressed again: a new press */
    CHECK_EQ(l.presses, 3);
    CHECK_EQ(l.press[0], 100u);                     /* the oldest press keeps its time */

    uint8_t high[1] = { 200 };                      /* slots beyond the bitmap are ignored */
    km_lat_observe(&l, 150u, high, 1);
    CHECK_EQ(l.presses, 3);
}

static void test_sample_and_sends(void)
{
    km_lat_t l;
    km_lat_init(&l);
    uint8_t a[1] = { 3 };
    uint8_t b[2] = { 3, 4 };

    km_lat_sent(&l, 50u);                           /* a report with nothing waiting: counted, no sample */
    CHECK_EQ(l.sends, 1);
    CHECK_EQ(l.samples, 0);

    km_lat_observe(&l, 1000u, a, 1);
    km_lat_observe(&l, 1100u, b, 2);                /* a second press while the first waits */
    km_lat_sent(&l, 1500u);                         /* one report answers both; the sample is from the oldest (500 cycles) */
    CHECK_EQ(l.samples, 1);
    CHECK_EQ(l.last, 500u);
    CHECK_EQ(l.min, 500u);
    CHECK_EQ(l.max, 500u);
    CHECK_EQ(l.pending, 0);
    CHECK_EQ(l.sends, 2);

    km_lat_observe(&l, 2000u, NULL, 0);
    km_lat_observe(&l, 3000u, a, 1);
    km_lat_sent(&l, 3300u);                         /* 300 cycles: smaller than the first */
    CHECK_EQ(l.samples, 2);
    CHECK_EQ(l.last, 300u);
    CHECK_EQ(l.min, 300u);
    CHECK_EQ(l.max, 500u);
}

static void test_wrap_and_ring(void)
{
    km_lat_t l;
    km_lat_init(&l);
    const uint32_t t0 = 0xFFFFFFF0u;               /* the cycle counter wraps: the difference is still right */
    uint8_t s[1];
    for (unsigned k = 0; k < KM_LAT_RING + 1u; k++) {  /* one more press than the ring holds */
        s[0] = (uint8_t)k;
        km_lat_observe(&l, t0 + k, s, 1);
    }
    CHECK_EQ(l.presses, KM_LAT_RING + 1u);
    CHECK_EQ(l.pending, KM_LAT_RING);
    CHECK_EQ(l.dropped, 1);
    km_lat_sent(&l, t0 + 0x20u);                    /* oldest press is at t0: 0x20 cycles, counted across the 32-bit wrap of t0 + 0x20 */
    CHECK_EQ(l.last, 0x20u);
    CHECK_EQ(l.pending, 0);
}

static void test_pack(void)
{
    km_lat_t l;
    km_lat_init(&l);
    uint16_t v[KM_LAT_VALUES];
    km_lat_pack(&l, 240u, v);                       /* nothing measured yet: the min is 0, not 0xFFFFFFFF */
    CHECK_EQ(v[0], 0);
    CHECK_EQ(v[2], 0);

    uint8_t s[1] = { 7 };
    km_lat_observe(&l, 0u, s, 1);
    km_lat_sent(&l, 2400u);                         /* 2400 cycles at 240 cycles/us = 10 us */
    km_lat_pack(&l, 240u, v);
    CHECK_EQ(v[0], 1);
    CHECK_EQ(v[1], 10);
    CHECK_EQ(v[2], 10);
    CHECK_EQ(v[3], 10);
    CHECK_EQ(v[4], 1);
    CHECK_EQ(v[5], 0);
    CHECK_EQ(v[6], 1);
    CHECK_EQ(v[7], 0);

    km_lat_observe(&l, 10u, NULL, 0);
    km_lat_observe(&l, 20u, s, 1);
    km_lat_sent(&l, 20u + 240u * 100000u);          /* 100 ms: saturates at 65535 us */
    km_lat_pack(&l, 240u, v);
    CHECK_EQ(v[3], 0xFFFF);

    km_lat_pack(&l, 0u, v);                         /* a zero rate must not divide by zero */
    CHECK(v[1] == 0xFFFF);
}

void test_latency_probe(void)
{
    test_press_edges();
    test_sample_and_sends();
    test_wrap_and_ring();
    test_pack();
}
