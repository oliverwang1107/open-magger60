/*
 * stab_sim.h - fault-injection simulator for the stability logic (M5 task T-E). Header only, used by test_health.c and
 * test_report_sync.c.
 *
 * It models the three things around km_health / km_report_sync that the real main loop (port/n32h48x/m5_stab_main.c) talks to:
 *   - a USB device endpoint with a host behind it: reports are accepted, take `service` cycles to be taken by the host (the
 *     endpoint is busy meanwhile), and can be refused, silently lost, or never taken (busy forever); the host keeps the key
 *     state that the last report it actually RECEIVED says. A bus reset or a re-enumeration makes the host forget all keys;
 *   - the scan chain: frames arrive every pass unless it is stalled; a timeout restarts it (or not, to model a dead chain);
 *   - the independent watchdog: it expires 2 s after the last feed.
 * sim_pass() is the same sequence of calls the main loop makes, so a test that passes here exercises the same policy.
 *
 * "Truth" is the key state the engine would report when everything works (sim.truth, 8-byte reports; all-zero = released).
 * The property every test checks is: the host never keeps a key the keyboard has released, and while a fault is active it holds
 * no key at all - within a bounded time (the tests state the bound).
 */
#ifndef STAB_SIM_H
#define STAB_SIM_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "km/km_health.h"
#include "km/km_report_sync.h"

#define SIM_REPORT_LEN 8u
#define SIM_PASS_CYCLES 24000u            /* 100 us per main-loop pass */
#define SIM_SCAN_TIMEOUT_CYCLES (3u * 240000u)
#define SIM_WDT_CYCLES (2u * 240000000u)

typedef struct {
    km_health_t h;
    km_report_sync_t sync;
    uint32_t now;

    /* USB device */
    bool configured, suspended;
    bool busy;
    uint32_t busy_until;
    uint32_t service_cycles;              /* the host takes a report this long after it was accepted (~1 ms) */
    unsigned refuse;                      /* refuse the next n sends (send() returns false) */
    bool lose_all;                        /* accept sends but the host never receives them */
    unsigned lose_next;                   /* ... only the next n */
    bool busy_forever;                    /* an accepted send never completes */
    bool recover_fixes;                   /* km recover callback clears busy_forever */
    unsigned recover_calls, sends;
    uint8_t host[SIM_REPORT_LEN];         /* the host's key state */
    bool poll_dead;                       /* km_usbhs_poll() does not run (hang): no USB progress */

    /* scan chain */
    bool scan_alive;
    bool restart_fixes;
    uint32_t last_frame;
    unsigned restarts;
    unsigned dma_error_burst;             /* report one DMA error in each of the next n passes */

    /* engine / calibration */
    uint8_t truth[SIM_REPORT_LEN];

    /* watchdog */
    uint32_t last_feed;
    bool wdt_reset;
    unsigned feeds;
} sim_t;

static inline bool sim_link_up(void *c)
{
    const sim_t *s = (const sim_t *)c;
    return s->configured && !s->suspended;
}

static inline bool sim_tx_ready(void *c)
{
    const sim_t *s = (const sim_t *)c;
    return !s->busy;
}

static inline bool sim_send(void *c, const uint8_t *r, unsigned len)
{
    sim_t *s = (sim_t *)c;
    if (!s->configured || s->busy || len != SIM_REPORT_LEN) {
        return false;
    }
    if (s->refuse > 0) {
        s->refuse--;
        return false;
    }
    s->sends++;
    s->busy = true;
    s->busy_until = s->now + s->service_cycles;
    if (s->busy_forever) {
        s->busy_until = 0;
    }
    if (s->lose_all) {
        return true;
    }
    if (s->lose_next > 0) {
        s->lose_next--;
        return true;
    }
    memcpy(s->host, r, len);              /* delivered when the host polls; modelled as immediate, busy models the delay */
    return true;
}

static inline void sim_recover(void *c)
{
    sim_t *s = (sim_t *)c;
    s->recover_calls++;
    if (s->recover_fixes) {
        s->busy_forever = false;
        s->busy = false;
    }
}

static inline km_sync_io_t sim_io(sim_t *s)
{
    km_sync_io_t io = { s, sim_link_up, sim_tx_ready, sim_send, sim_recover };
    return io;
}

static inline void sim_init(sim_t *s, uint32_t start_cycles)
{
    memset(s, 0, sizeof *s);
    s->now = start_cycles;
    s->service_cycles = 240000u;
    s->configured = true;
    s->scan_alive = true;
    s->restart_fixes = true;
    s->recover_fixes = true;
    s->last_frame = start_cycles;
    s->last_feed = start_cycles;
    km_health_init(&s->h, start_cycles, 0);
    km_health_on_calibration(&s->h, true);
    static const uint8_t zero[SIM_REPORT_LEN];
    km_report_sync_init(&s->sync, zero, SIM_REPORT_LEN, start_cycles);
}

/* the host loses its state on a bus reset / re-enumeration; the device side starts unconfigured then configured again */
static inline void sim_bus_reset(sim_t *s)
{
    memset(s->host, 0, sizeof s->host);
    s->busy = false;
    s->busy_forever = false;
    km_health_on_usb(&s->h, KM_USB_EV_RESET);
}

static inline void sim_pass(sim_t *s)
{
    s->now += SIM_PASS_CYCLES;
    if (s->busy && !s->busy_forever && (int32_t)(s->now - s->busy_until) >= 0) {
        s->busy = false;
    }
    if (!s->poll_dead && !km_report_sync_stalled(&s->sync, s->now)) {
        km_health_progress(&s->h, KM_PROG_USB);
    }
    if (s->scan_alive) {
        s->last_frame = s->now;
        km_health_on_frame(&s->h);
    } else if ((uint32_t)(s->now - s->last_frame) > SIM_SCAN_TIMEOUT_CYCLES) {
        if (km_health_on_scan_timeout(&s->h, s->now)) {
            s->restarts++;
            s->last_frame = s->now;
            if (s->restart_fixes) {
                s->scan_alive = true;
            }
        }
    }
    if (s->dma_error_burst > 0) {
        s->dma_error_burst--;
        km_health_on_dma_error(&s->h, s->now, 1);
    }
    km_health_tick(&s->h, s->now);
    const km_sync_io_t io = sim_io(s);
    km_report_sync_pump(&s->sync, &io, s->truth, km_health_keys_allowed(&s->h), s->now);
    km_health_set_report_stats(&s->h, s->sync.retries, s->sync.resyncs);
    if (km_health_should_feed(&s->h, s->now)) {
        s->last_feed = s->now;
        s->feeds++;
    }
    if ((uint32_t)(s->now - s->last_feed) > SIM_WDT_CYCLES) {
        s->wdt_reset = true;
    }
}

static inline void sim_run_ms(sim_t *s, unsigned ms)
{
    const uint32_t end = s->now + ms * 240000u;
    while ((int32_t)(end - s->now) > 0 && !s->wdt_reset) {
        sim_pass(s);
    }
}

static inline bool sim_host_any_key(const sim_t *s)
{
    for (unsigned i = 0; i < SIM_REPORT_LEN; i++) {
        if (s->host[i] != 0) {
            return true;
        }
    }
    return false;
}

static inline void sim_press(sim_t *s, uint8_t key)
{
    s->truth[2] = key;                    /* one key in the first key slot of an 8-byte boot-style report */
}

static inline void sim_release(sim_t *s)
{
    memset(s->truth, 0, sizeof s->truth);
}

#endif /* STAB_SIM_H */
