/*
 * km_led_guard.c - kill switch policy for the LED hardware, see km_led_guard.h.
 */
#include "km/km_led_guard.h"

#include <stddef.h>

#define REC_MAGIC 0x4B4D4C47u      /* "KMLG" */

static uint32_t check_of(const km_led_rec_t *r)
{
    return REC_MAGIC ^ r->build ^ (r->strikes * 0x9E3779B1u) ^ (r->state * 0x85EBCA6Bu) ^ 0x5A5A5A5Au;
}

static void rec_write(km_led_rec_t *r, uint32_t build, uint32_t strikes, uint32_t state)
{
    r->magic = REC_MAGIC;
    r->build = build;
    r->strikes = strikes;
    r->state = state;
    r->check = check_of(r);
}

uint32_t km_led_build_id(const char *s)
{
    uint32_t h = 2166136261u;            /* FNV-1a */
    while (*s != '\0') {
        h = (h ^ (uint8_t)*s++) * 16777619u;
    }
    return h;
}

void km_led_guard_boot(km_led_guard_t *g, km_led_rec_t *rec, uint32_t build)
{
    *g = (km_led_guard_t){ 0 };
    g->rec = rec;
    g->boot_allowed = true;
    if (rec == NULL) {
        return;
    }
    uint32_t strikes = 0, state = KM_LED_REC_OK;
    if (rec->magic == REC_MAGIC && rec->build == build && rec->check == check_of(rec) && rec->state <= KM_LED_REC_DISABLED &&
        rec->strikes <= 1000u) {
        strikes = rec->strikes;
        state = rec->state;
        if (state == KM_LED_REC_STARTING) {          /* the last boot started the hardware and never got to "stable" */
            strikes++;
            state = KM_LED_REC_OK;
        }
    }
    g->strikes = strikes;
    if (state == KM_LED_REC_DISABLED || strikes >= KM_LED_GUARD_MAX_STRIKES) {
        g->boot_allowed = false;
        g->tripped = true;
        g->why = KM_LED_WHY_BOOT_LOOP;
        rec_write(rec, build, strikes, KM_LED_REC_DISABLED);
    } else {
        rec_write(rec, build, strikes, state);
    }
}

bool km_led_guard_may_start(const km_led_guard_t *g)
{
    return g->boot_allowed && !g->tripped;
}

void km_led_guard_hw_starting(km_led_guard_t *g, uint32_t now_ms)
{
    g->hw_started = true;
    g->stable = false;
    g->start_ms = now_ms;
    g->calm_since_ms = now_ms;
    if (g->rec != NULL) {
        rec_write(g->rec, g->rec->build, g->strikes, KM_LED_REC_STARTING);
    }
}

void km_led_guard_trip(km_led_guard_t *g, uint8_t why)
{
    if (!g->tripped) {
        g->tripped = true;
        g->why = why;
    }
}

void km_led_guard_hw_failed(km_led_guard_t *g)
{
    km_led_guard_trip(g, KM_LED_WHY_HW_START);
}

km_led_verdict_t km_led_guard_step(km_led_guard_t *g, uint32_t now_ms, uint8_t faults)
{
    if (g->tripped) {
        return KM_LED_KILL;
    }
    if (faults != 0u) {
        if (!g->in_fault) {
            g->in_fault = true;
            if (g->hw_started && ++g->episodes >= KM_LED_GUARD_MAX_EPISODES) {
                km_led_guard_trip(g, KM_LED_WHY_FAULTS);
                return KM_LED_KILL;
            }
        }
        g->start_ms = now_ms;                       /* "stable" counts calm time only */
        g->calm_since_ms = now_ms;
        return KM_LED_PAUSE;
    }
    g->in_fault = false;
    if (g->episodes != 0u && (uint32_t)(now_ms - g->calm_since_ms) >= KM_LED_GUARD_FORGIVE_MS) {
        g->episodes = 0;
    }
    if (g->hw_started && !g->stable && (uint32_t)(now_ms - g->start_ms) >= KM_LED_GUARD_STABLE_MS) {
        g->stable = true;
        if (g->rec != NULL) {
            rec_write(g->rec, g->rec->build, 0, KM_LED_REC_OK);
        }
    }
    return KM_LED_RUN;
}

void km_led_guard_dma_result(km_led_guard_t *g, bool timed_out)
{
    if (!timed_out) {
        g->dma_timeouts = 0;
    } else if (++g->dma_timeouts >= KM_LED_GUARD_MAX_DMA_TIMEOUTS) {
        km_led_guard_trip(g, KM_LED_WHY_DMA);
    }
}
