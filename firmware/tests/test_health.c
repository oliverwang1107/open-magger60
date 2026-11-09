/*
 * M5 T-E: km_health (counters, fault flags, progress-gated watchdog feed, keys_allowed) and the fault-injection scenarios of the
 * whole stability policy (km_health + km_report_sync driven through tests/sim/stab_sim.h). Every scenario ends in the same
 * property: the host never keeps a key the keyboard has released, and while a fault is active the host holds no key.
 * Each scenario runs twice, once with the cycle counter near 0 and once a few thousand cycles before it wraps.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "km/km_health.h"
#include "km/km_report_sync.h"
#include "km_test.h"
#include "sim/stab_sim.h"

#define MS KM_MS_CYCLES
static const uint32_t k_starts[2] = { 1000u, 0xFFFFF000u };

/* ---------- unit tests ---------- */

static void test_init_and_keys_allowed(uint32_t t0)
{
    km_health_t h;
    km_health_init(&h, t0, 0x20);
    CHECK_EQ(h.reset_cause, 0x20);
    CHECK_EQ(h.fault_flags, KM_FAULT_CAL_LOST);
    CHECK(!km_health_keys_allowed(&h));                   /* no calibration yet: no keys */
    km_health_on_calibration(&h, false);
    CHECK_EQ(h.calibration_refused, 1);
    CHECK(!km_health_keys_allowed(&h));
    km_health_on_calibration(&h, true);
    CHECK_EQ(h.calibrations, 1);
    CHECK(km_health_keys_allowed(&h));
    km_health_cal_lost(&h);
    CHECK(!km_health_keys_allowed(&h));
    CHECK_EQ(h.fault_flags, KM_FAULT_CAL_LOST);
    km_health_on_calibration(&h, true);
    CHECK(km_health_keys_allowed(&h));    km_health_on_calibration(&h, false);                  /* a later attempt that is refused (keys held) takes the calibration away again */
    CHECK(!km_health_keys_allowed(&h));
    CHECK_EQ(h.calibration_refused, 2);
    km_health_on_calibration(&h, true);
    CHECK(km_health_keys_allowed(&h));
}

static void test_feed_gating(uint32_t t0)
{
    km_health_t h;
    km_health_init(&h, t0, 0);
    /* grace: nothing has progressed yet, but the window has just started */
    CHECK(km_health_should_feed(&h, t0));
    CHECK(km_health_should_feed(&h, t0 + KM_HEALTH_FEED_WINDOW_CYCLES));          /* exactly one window: still fed */
    CHECK(!km_health_should_feed(&h, t0 + KM_HEALTH_FEED_WINDOW_CYCLES + 1u));    /* one cycle later: not */

    /* both parts progress: fed for ever */
    uint32_t t = t0;
    for (unsigned i = 0; i < 1000; i++) {
        t += 24000u;
        km_health_progress(&h, KM_PROG_SCAN);
        km_health_progress(&h, KM_PROG_USB);
        CHECK(km_health_should_feed(&h, t));
    }
    /* the scan stops: fed until its last progress is a window old, then never again */
    const uint32_t t_stall = t;
    bool fed_after = false;
    bool last = true;
    while ((uint32_t)(t - t_stall) < 3000u * MS) {
        t += 24000u;
        km_health_progress(&h, KM_PROG_USB);
        const bool f = km_health_should_feed(&h, t);
        if ((uint32_t)(t - t_stall) <= KM_HEALTH_FEED_WINDOW_CYCLES) {
            CHECK(f);
        } else {
            CHECK(!f);
            fed_after = fed_after || f;
        }
        last = f;
    }
    CHECK(!fed_after);
    CHECK(!last);
    /* the scan comes back: fed again at once */
    km_health_progress(&h, KM_PROG_SCAN);
    CHECK(km_health_should_feed(&h, t + 24000u));

    /* the USB side stops while the scan keeps going */
    km_health_init(&h, t0, 0);
    t = t0;
    for (unsigned i = 0; i < 100; i++) {
        t += 24000u;
        km_health_progress(&h, KM_PROG_SCAN);
        km_health_progress(&h, KM_PROG_USB);
        (void)km_health_should_feed(&h, t);
    }
    const uint32_t t_usb = t;
    for (; (uint32_t)(t - t_usb) < KM_HEALTH_FEED_WINDOW_CYCLES + 100u * 24000u;) {
        t += 24000u;
        km_health_progress(&h, KM_PROG_SCAN);
        (void)km_health_should_feed(&h, t);
    }
    CHECK(!km_health_should_feed(&h, t));

    /* engine progress alone is not enough, and a missing engine does not matter (informational) */
    km_health_init(&h, t0, 0);
    km_health_progress(&h, KM_PROG_ENGINE);
    CHECK(!km_health_should_feed(&h, t0 + KM_HEALTH_FEED_WINDOW_CYCLES + 2u * 24000u));
    km_health_progress(&h, KM_PROG_SCAN | KM_PROG_USB);
    CHECK(km_health_should_feed(&h, t0 + KM_HEALTH_FEED_WINDOW_CYCLES + 3u * 24000u));
    CHECK(km_health_should_feed(&h, t0 + 2u * KM_HEALTH_FEED_WINDOW_CYCLES));   /* ENGINE never required */
}

static void test_transients(uint32_t t0)
{
    km_health_t h;
    km_health_init(&h, t0, 0);
    km_health_on_calibration(&h, true);

    /* DMA error: keys off for exactly the hold-off, and a new error extends it */
    km_health_on_dma_error(&h, t0, 0);
    CHECK(km_health_keys_allowed(&h));                           /* zero new errors is nothing */
    km_health_on_dma_error(&h, t0, 3);
    CHECK_EQ(h.dma_errors, 3);
    CHECK(!km_health_keys_allowed(&h));
    km_health_tick(&h, t0 + KM_HEALTH_HOLDOFF_CYCLES - 1u);
    CHECK(!km_health_keys_allowed(&h));
    km_health_on_dma_error(&h, t0 + 10u * MS, 1);                /* again 10 ms later */
    km_health_tick(&h, t0 + KM_HEALTH_HOLDOFF_CYCLES);
    CHECK(!km_health_keys_allowed(&h));                          /* the first hold-off is over, the second is not */
    km_health_tick(&h, t0 + 10u * MS + KM_HEALTH_HOLDOFF_CYCLES - 1u);
    CHECK(!km_health_keys_allowed(&h));
    km_health_tick(&h, t0 + 10u * MS + KM_HEALTH_HOLDOFF_CYCLES);
    CHECK(km_health_keys_allowed(&h));

    /* engine: above the budget only counted, above the limit a fault with hold-off */
    km_health_on_engine(&h, t0 + 100u * MS, KM_HEALTH_ENGINE_BUDGET_CYCLES);
    CHECK_EQ(h.engine_over_budget, 0);
    km_health_on_engine(&h, t0 + 100u * MS, KM_HEALTH_ENGINE_BUDGET_CYCLES + 1u);
    CHECK_EQ(h.engine_over_budget, 1);
    CHECK_EQ(h.engine_max_cycles, KM_HEALTH_ENGINE_BUDGET_CYCLES + 1u);
    CHECK(km_health_keys_allowed(&h));                           /* a 10.7k update is a statistic, not a fault */
    km_health_on_engine(&h, t0 + 100u * MS, KM_HEALTH_ENGINE_LIMIT_CYCLES);
    CHECK(km_health_keys_allowed(&h));
    km_health_on_engine(&h, t0 + 100u * MS, KM_HEALTH_ENGINE_LIMIT_CYCLES + 1u);
    CHECK_EQ(h.engine_overruns, 1);
    CHECK(!km_health_keys_allowed(&h));
    CHECK_EQ(h.fault_flags, KM_FAULT_ENGINE_OVERRUN);
    km_health_tick(&h, t0 + 100u * MS + KM_HEALTH_HOLDOFF_CYCLES);
    CHECK(km_health_keys_allowed(&h));
    CHECK_EQ(h.engine_max_cycles, KM_HEALTH_ENGINE_LIMIT_CYCLES + 1u);

    /* scan stall: the flag survives the hold-off until a frame has really arrived */
    CHECK(km_health_on_scan_timeout(&h, t0 + 200u * MS));
    CHECK_EQ(h.scan_stalls, 1);
    CHECK_EQ(h.scan_restarts, 1);
    km_health_tick(&h, t0 + 200u * MS + 10u * KM_HEALTH_HOLDOFF_CYCLES);
    CHECK(!km_health_keys_allowed(&h));                          /* still no frame: still stalled */
    km_health_on_frame(&h);
    km_health_tick(&h, t0 + 200u * MS + 10u * KM_HEALTH_HOLDOFF_CYCLES);
    CHECK(km_health_keys_allowed(&h));

    /* a second stall while the first one is still active */
    CHECK(km_health_on_scan_timeout(&h, t0 + 400u * MS));
    CHECK(km_health_on_scan_timeout(&h, t0 + 405u * MS));
    CHECK_EQ(h.scan_stalls, 3);
    km_health_on_frame(&h);
    km_health_tick(&h, t0 + 405u * MS + KM_HEALTH_HOLDOFF_CYCLES - 1u);
    CHECK(!km_health_keys_allowed(&h));
    km_health_tick(&h, t0 + 405u * MS + KM_HEALTH_HOLDOFF_CYCLES);
    CHECK(km_health_keys_allowed(&h));
}

static void test_usb_events(uint32_t t0)
{
    km_health_t h;
    km_health_init(&h, t0, 0);
    km_health_on_calibration(&h, true);
    km_health_on_usb(&h, KM_USB_EV_SUSPEND);
    CHECK(!km_health_keys_allowed(&h));
    CHECK_EQ(h.usb_suspends, 1);
    km_health_on_usb(&h, KM_USB_EV_RESUME);
    CHECK(km_health_keys_allowed(&h));
    km_health_on_usb(&h, KM_USB_EV_SUSPEND);
    km_health_on_usb(&h, KM_USB_EV_RESET);                       /* a reset ends the suspend as well */
    CHECK(km_health_keys_allowed(&h));
    CHECK_EQ(h.usb_resets, 1);
    /* level form: only changes count */
    km_health_set_usb_suspended(&h, false);
    CHECK_EQ(h.usb_suspends, 2);
    km_health_set_usb_suspended(&h, true);
    km_health_set_usb_suspended(&h, true);
    CHECK_EQ(h.usb_suspends, 3);
    CHECK(!km_health_keys_allowed(&h));
    km_health_set_usb_suspended(&h, false);
    km_health_set_usb_suspended(&h, false);
    CHECK_EQ(h.usb_resumes, 2);
    CHECK(km_health_keys_allowed(&h));
    /* a suspend does not hide a calibration loss */
    km_health_cal_lost(&h);
    km_health_on_usb(&h, KM_USB_EV_SUSPEND);
    km_health_on_usb(&h, KM_USB_EV_RESUME);
    CHECK(!km_health_keys_allowed(&h));
    CHECK_EQ(h.fault_flags, KM_FAULT_CAL_LOST);
}

static void test_pack_and_cause(void)
{
    km_health_t h;
    km_health_init(&h, 5, 0x28);
    h.frames = 0x12345;
    h.scan_stalls = 7;
    h.dma_errors = 0x20000;                                      /* saturates */
    h.engine_max_cycles = 10700;
    h.fault_flags = KM_FAULT_DMA_ERROR;
    const uint16_t extra[7] = { 1, 2, 3, 4, 5, 6, 7 };
    uint16_t v[KM_HEALTH_SCOPE_VALUES];
    km_health_pack_scope(&h, extra, v);
    CHECK_EQ(v[0], 0x2802);
    CHECK_EQ(v[1], 0x2345);
    CHECK_EQ(v[2], 1);
    CHECK_EQ(v[3], 7);
    CHECK_EQ(v[4], 0xFFFF);
    CHECK_EQ(v[6], 10700);
    for (unsigned i = 0; i < 7; i++) {
        CHECK_EQ(v[17 + i], i + 1);
    }
    km_health_pack_scope(&h, NULL, v);
    CHECK_EQ(v[23], 0);

    const uint8_t down[] = { 0, 15, 16, 75, 76, 200 };           /* 76 and above are ignored */
    uint16_t m[KM_KEYMASK_SCOPE_VALUES];
    km_health_pack_keymask(down, 6, 0xBEEF, 0x03, true, m);
    CHECK_EQ(m[0], 0x8001);
    CHECK_EQ(m[1], 0x0001);
    CHECK_EQ(m[2], 0);
    CHECK_EQ(m[3], 0);
    CHECK_EQ(m[4], 1u << (75 - 64));
    CHECK_EQ(m[5], 0xBEEF);
    CHECK_EQ(m[6], 3);
    CHECK_EQ(m[7], 1);
    km_health_pack_keymask(NULL, 0, 1, 0, false, m);
    CHECK_EQ(m[0] | m[1] | m[2] | m[3] | m[4] | m[7], 0);

    CHECK_EQ(km_health_reset_cause_from_reg(0x24000000u), 0x24);
    CHECK_EQ(km_health_reset_cause_from_reg(0x25000000u), 0x24);   /* the clear-request bit is dropped */
    CHECK_EQ(km_health_reset_cause_from_reg(0x00FFFFFFu), 0);
    CHECK(strcmp(km_health_reset_cause_name(KM_RESET_IWDG | KM_RESET_PIN), "iwdg") == 0);
    CHECK(strcmp(km_health_reset_cause_name(KM_RESET_POR | KM_RESET_PIN), "por") == 0);
    CHECK(strcmp(km_health_reset_cause_name(KM_RESET_SOFT), "soft") == 0);
    CHECK(strcmp(km_health_reset_cause_name(0), "none") == 0);
}

/* ---------- fault-injection scenarios ---------- */

/* Keys are pressed for 200 ms, then released; the host must see the key while it is held and nothing afterwards. */
static void test_baseline_typing(uint32_t t0)
{
    sim_t s;
    sim_init(&s, t0);
    sim_run_ms(&s, 5);
    CHECK(!sim_host_any_key(&s));
    sim_press(&s, 0x04);
    sim_run_ms(&s, 5);
    CHECK_EQ(s.host[2], 0x04);
    sim_run_ms(&s, 2000);                                        /* long hold: heartbeats, no reset, no stuck state */
    CHECK_EQ(s.host[2], 0x04);
    CHECK(s.sync.heartbeats >= 3);
    sim_release(&s);
    sim_run_ms(&s, 5);
    CHECK(!sim_host_any_key(&s));
    CHECK(!s.wdt_reset);
    CHECK(km_health_keys_allowed(&s.h));
}

/* A release that is refused 1000 times before it is accepted: the key is gone as soon as it is accepted, nothing else happens. */
static void test_release_fails_1000_times(uint32_t t0)
{
    sim_t s;
    sim_init(&s, t0);
    sim_press(&s, 0x2C);
    sim_run_ms(&s, 5);
    CHECK_EQ(s.host[2], 0x2C);
    sim_release(&s);
    s.refuse = 1000;
    sim_run_ms(&s, 50);                                          /* 1000 passes at 100 us = 100 ms: not done yet */
    CHECK(s.refuse > 0);
    CHECK_EQ(s.host[2], 0x2C);                                   /* still down on the host while the driver refuses */
    sim_run_ms(&s, 100);
    CHECK_EQ(s.refuse, 0);
    CHECK_EQ(s.sync.retries, 1000);
    sim_run_ms(&s, 5);
    CHECK(!sim_host_any_key(&s));
    CHECK(!s.wdt_reset);                                         /* 100 ms of refusals are far below the 1 s dead limit */
}

/* The accepted release never reaches the host: the echo one heartbeat later does. */
static void test_lost_release_is_echoed(uint32_t t0)
{
    sim_t s;
    sim_init(&s, t0);
    sim_press(&s, 0x2C);
    sim_run_ms(&s, 5);
    sim_release(&s);
    s.lose_next = 1;
    sim_run_ms(&s, 50);
    CHECK_EQ(s.host[2], 0x2C);                                   /* lost: the host still holds the key */
    sim_run_ms(&s, 600);
    CHECK(!sim_host_any_key(&s));                                /* healed by the echo within heartbeat + a few ms */
    CHECK_EQ(s.sync.heartbeats, 1);
    CHECK(!s.wdt_reset);

    /* a lost press while the key stays down is repaired by the heartbeat as well */
    sim_init(&s, t0);
    s.lose_next = 1;
    sim_press(&s, 0x05);
    sim_run_ms(&s, 50);
    CHECK_EQ(s.host[2], 0);
    sim_run_ms(&s, 600);
    CHECK_EQ(s.host[2], 0x05);
}

/* Endpoint busy forever: the recovery frees it, then the release goes through. */
static void test_busy_forever(uint32_t t0)
{
    sim_t s;
    sim_init(&s, t0);
    sim_press(&s, 0x2C);
    sim_run_ms(&s, 5);
    CHECK_EQ(s.host[2], 0x2C);
    s.busy_forever = true;
    s.busy = true;
    s.busy_until = 0;
    sim_release(&s);
    sim_run_ms(&s, 50);
    CHECK_EQ(s.host[2], 0x2C);                                   /* blocked */
    CHECK_EQ(s.recover_calls, 0);
    sim_run_ms(&s, 100);                                         /* past the 100 ms busy limit */
    CHECK_EQ(s.recover_calls, 1);
    sim_run_ms(&s, 5);
    CHECK(!sim_host_any_key(&s));
    CHECK_EQ(s.sync.recoveries, 1);
    CHECK(!s.wdt_reset);

    /* recovery does not help: the release stays undeliverable; after one second the USB progress stops and the watchdog resets
     * the keyboard (a reset drops the USB connection, which releases the key on the host) */
    sim_init(&s, t0);
    s.recover_fixes = false;
    sim_press(&s, 0x2C);
    sim_run_ms(&s, 5);
    s.busy_forever = true;
    s.busy = true;
    sim_release(&s);
    sim_run_ms(&s, 900);
    CHECK(!s.wdt_reset);
    CHECK(s.sync.recoveries >= 8);                               /* retried every 100 ms */
    sim_run_ms(&s, 6000);
    CHECK(s.wdt_reset);
    /* the reset happened between 1 s (dead limit) + 0.5 s (window) + 2 s (watchdog) and a little over that after the failure */
    CHECK(s.now - t0 < 4u * 240000000u + 240000000u / 2u);
}

/* Bus reset with a key down: the host forgets the state, the keyboard sends it again; a release afterwards is not lost. */
static void test_bus_reset_with_key_down(uint32_t t0)
{
    sim_t s;
    sim_init(&s, t0);
    sim_press(&s, 0x1A);
    sim_run_ms(&s, 5);
    CHECK_EQ(s.host[2], 0x1A);
    s.configured = false;                                        /* the host resets the bus: unconfigured, then enumerates again */
    sim_bus_reset(&s);
    sim_run_ms(&s, 20);
    CHECK(!sim_host_any_key(&s));
    s.configured = true;
    sim_run_ms(&s, 5);
    CHECK_EQ(s.host[2], 0x1A);                                   /* key still physically down: reported again */
    CHECK(s.sync.resyncs >= 2);
    sim_release(&s);
    sim_run_ms(&s, 5);
    CHECK(!sim_host_any_key(&s));
    CHECK(!s.wdt_reset);
}

/* Suspend with a key down; the key is released while suspended; resume must leave the host with no key. */
static void test_suspend_with_key_down(uint32_t t0)
{
    sim_t s;
    sim_init(&s, t0);
    sim_press(&s, 0x2C);
    sim_run_ms(&s, 5);
    CHECK_EQ(s.host[2], 0x2C);
    s.suspended = true;
    km_health_on_usb(&s.h, KM_USB_EV_SUSPEND);
    sim_release(&s);
    sim_run_ms(&s, 3000);                                        /* the PC sleeps for 3 s */
    CHECK(!s.wdt_reset);                                         /* suspend is not a dead endpoint */
    CHECK(!km_health_keys_allowed(&s.h));
    s.suspended = false;
    km_health_on_usb(&s.h, KM_USB_EV_RESUME);
    sim_run_ms(&s, 5);
    CHECK(!sim_host_any_key(&s));                                /* resync released the key the host still held */
    CHECK(km_health_keys_allowed(&s.h));
    /* and a key held through the resume is reported again */
    s.suspended = true;
    km_health_on_usb(&s.h, KM_USB_EV_SUSPEND);
    sim_press(&s, 0x04);
    sim_run_ms(&s, 100);
    CHECK(!sim_host_any_key(&s));                                /* never reported while suspended / not allowed */
    s.suspended = false;
    km_health_on_usb(&s.h, KM_USB_EV_RESUME);
    sim_run_ms(&s, 5);
    CHECK_EQ(s.host[2], 0x04);
}

/* DMA errors in a burst: keys go up at once, nothing is reported during the burst and the hold-off, then the real state returns. */
static void test_dma_error_burst(uint32_t t0)
{
    sim_t s;
    sim_init(&s, t0);
    sim_press(&s, 0x2C);
    sim_run_ms(&s, 5);
    CHECK_EQ(s.host[2], 0x2C);
    s.dma_error_burst = 1000;                                    /* an error every pass for 100 ms */
    sim_run_ms(&s, 3);
    CHECK(!sim_host_any_key(&s));                                /* released within a few passes of the first error */
    for (unsigned i = 0; i < 1000; i++) {
        sim_pass(&s);
        if (sim_host_any_key(&s)) {
            CHECK(false);                                        /* a key appeared during the burst */
            break;
        }
    }
    CHECK_EQ(s.h.dma_errors, 1000);
    sim_release(&s);                                             /* released during the hold-off */
    sim_run_ms(&s, 40);
    CHECK(km_health_keys_allowed(&s.h));
    CHECK(!sim_host_any_key(&s));
    sim_press(&s, 0x2C);
    sim_run_ms(&s, 5);
    CHECK_EQ(s.host[2], 0x2C);                                   /* normal operation again */
    CHECK(!s.wdt_reset);
}

/* Scan stall that recovers with a restart: released within a few ms of the timeout, typing resumes after the hold-off. */
static void test_scan_stall_recovers(uint32_t t0)
{
    sim_t s;
    sim_init(&s, t0);
    sim_press(&s, 0x2C);
    sim_run_ms(&s, 5);
    s.scan_alive = false;
    sim_run_ms(&s, 6);                                           /* timeout (3 ms) + a couple of passes */
    CHECK(!sim_host_any_key(&s));
    CHECK_EQ(s.restarts, 1);
    sim_run_ms(&s, 40);
    CHECK(km_health_keys_allowed(&s.h));
    CHECK_EQ(s.host[2], 0x2C);                                   /* key still held by the user: reported again */
    CHECK(!s.wdt_reset);
    CHECK(s.h.scan_stalls == 1 && s.h.scan_restarts == 1);
}

/* Scan stall that does not recover: keys are released, the feed stops and the watchdog resets in 2.0 .. 2.6 s. */
static void test_scan_stall_dead(uint32_t t0)
{
    sim_t s;
    sim_init(&s, t0);
    sim_press(&s, 0x2C);
    sim_run_ms(&s, 5);
    s.scan_alive = false;
    s.restart_fixes = false;
    const uint32_t t_stall = s.now;
    sim_run_ms(&s, 10);
    CHECK(!sim_host_any_key(&s));                                /* safe state first ... */
    CHECK(!s.wdt_reset);
    sim_run_ms(&s, 1500);
    CHECK(!s.wdt_reset);
    CHECK(!sim_host_any_key(&s));
    sim_run_ms(&s, 1500);
    CHECK(s.wdt_reset);                                          /* ... then the reset */
    const uint32_t dt = s.now - t_stall;
    CHECK(dt >= 2u * 240000000u);
    CHECK(dt <= 2u * 240000000u + 240000000u / 2u + 5u * MS);    /* 2.0 s + window 0.5 s + slack */
    CHECK(s.h.scan_restarts > 100);
}

/* The USB poll hangs while the scan runs: no feed, reset. */
static void test_usb_poll_dead(uint32_t t0)
{
    sim_t s;
    sim_init(&s, t0);
    s.poll_dead = true;
    sim_run_ms(&s, 2300);
    CHECK(!s.wdt_reset);
    sim_run_ms(&s, 500);
    CHECK(s.wdt_reset);
}

/* Calibration lost while a key is down. */
static void test_calibration_lost(uint32_t t0)
{
    sim_t s;
    sim_init(&s, t0);
    sim_press(&s, 0x2C);
    sim_run_ms(&s, 5);
    CHECK_EQ(s.host[2], 0x2C);
    km_health_cal_lost(&s.h);
    sim_run_ms(&s, 3);
    CHECK(!sim_host_any_key(&s));
    sim_run_ms(&s, 3000);                                        /* refused calibrations while the key is held */
    km_health_on_calibration(&s.h, false);
    CHECK(!sim_host_any_key(&s));
    CHECK(!s.wdt_reset);
    sim_release(&s);
    km_health_on_calibration(&s.h, true);
    sim_run_ms(&s, 5);
    CHECK(!sim_host_any_key(&s));
    sim_press(&s, 0x05);
    sim_run_ms(&s, 5);
    CHECK_EQ(s.host[2], 0x05);
}

/* The engine takes far too long once: keys off for the hold-off. */
static void test_engine_overrun(uint32_t t0)
{
    sim_t s;
    sim_init(&s, t0);
    sim_press(&s, 0x2C);
    sim_run_ms(&s, 5);
    km_health_on_engine(&s.h, s.now, 80000u);
    sim_run_ms(&s, 3);
    CHECK(!sim_host_any_key(&s));
    sim_run_ms(&s, 40);
    CHECK_EQ(s.host[2], 0x2C);
    CHECK_EQ(s.h.engine_overruns, 1);
}

/* Random fault soup: whatever happens, whenever the keyboard is in a fault state for more than 10 ms the host holds no key, and
 * whenever the truth has been all-released for 1.5 s with a working link the host holds no key either. */
static void test_random_soup(uint32_t t0)
{
    sim_t s;
    sim_init(&s, t0);
    uint32_t rng = 12345u ^ t0, rng2 = 777u;
    uint32_t fault_since = 0;
    bool in_fault = false;
    uint32_t released_since = s.now;
    uint32_t last_inject = s.now;
    unsigned cal_back_in = 0, resume_in = 0;
    unsigned checks_fault = 0, checks_stuck = 0;
    for (unsigned i = 0; i < 800000 && !s.wdt_reset; i++) {
        rng = rng * 1664525u + 1013904223u;
        rng2 = rng2 * 22695477u + 1u;
        const unsigned r = (rng2 >> 14) & 0x7FFFu;            /* typing events: a press every ~1.6 s on average */
        const unsigned f = (rng >> 4) & 0x3FFFFu;             /* faults: rare, every few seconds in total */
        if (r == 1) {
            sim_press(&s, (uint8_t)(0x04 + ((rng >> 8) & 7u)));
        } else if (r == 2 || r == 3) {
            if (s.truth[2] != 0) {
                released_since = s.now;
            }
            sim_release(&s);
        }
        if (f == 4u) {
            s.refuse = (rng >> 24) & 63u;
            last_inject = s.now;
        } else if (f == 5u) {
            s.lose_next = (rng >> 24) & 1u;                    /* one lost report; two in a row are beyond the design */
            last_inject = s.now;
        } else if (f == 6u) {
            s.dma_error_burst = (rng >> 24) & 31u;
        } else if (f == 7u) {
            s.scan_alive = false;
        } else if (f == 8u && cal_back_in == 0) {
            km_health_cal_lost(&s.h);
            cal_back_in = 500u + ((rng >> 24) & 1023u);
        } else if (f == 9u && !s.suspended) {
            s.suspended = true;
            km_health_on_usb(&s.h, KM_USB_EV_SUSPEND);
            resume_in = 100u + ((rng >> 24) & 4095u);
        }
        if (cal_back_in > 0 && --cal_back_in == 0) {
            km_health_on_calibration(&s.h, true);
        }
        if (resume_in > 0 && --resume_in == 0) {
            s.suspended = false;
            km_health_on_usb(&s.h, KM_USB_EV_RESUME);
        }
        sim_pass(&s);
        if (!s.h.fault_flags) {
            in_fault = false;
        } else if (!in_fault) {
            in_fault = true;
            fault_since = s.now;
        }
        const bool quiet = (uint32_t)(s.now - last_inject) > 700u * MS && s.refuse == 0;   /* no injected loss lately */
        if (in_fault && !s.suspended && quiet && (uint32_t)(s.now - fault_since) > 10u * MS) {
            checks_fault++;
            if (sim_host_any_key(&s)) {
                CHECK(false);                                  /* a key on the host during a fault */
                break;
            }
        }
        if (!in_fault && !s.suspended && quiet && (uint32_t)(s.now - released_since) > 1500u * MS && s.truth[2] == 0) {
            checks_stuck++;
            if (sim_host_any_key(&s)) {
                CHECK(false);                                  /* a stuck key */
                break;
            }
        }
    }
    CHECK(checks_fault > 100);
    CHECK(checks_stuck > 100);
    CHECK(!s.wdt_reset);
}

void test_health(void)
{
    test_pack_and_cause();
    for (unsigned i = 0; i < 2; i++) {
        const uint32_t t0 = k_starts[i];
        test_init_and_keys_allowed(t0);
        test_feed_gating(t0);
        test_transients(t0);
        test_usb_events(t0);
        test_baseline_typing(t0);
        test_release_fails_1000_times(t0);
        test_lost_release_is_echoed(t0);
        test_busy_forever(t0);
        test_bus_reset_with_key_down(t0);
        test_suspend_with_key_down(t0);
        test_dma_error_burst(t0);
        test_scan_stall_recovers(t0);
        test_scan_stall_dead(t0);
        test_usb_poll_dead(t0);
        test_calibration_lost(t0);
        test_engine_overrun(t0);
        test_random_soup(t0);
    }
}
