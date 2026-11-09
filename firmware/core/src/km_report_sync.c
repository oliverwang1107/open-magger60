#include "km/km_report_sync.h"

#include <string.h>

/* `span` cycles or more have passed since `then` (wrap-safe; a stale `then` older than 17.9 s only gives a harmless early answer) */
static bool elapsed(uint32_t now, uint32_t then, uint32_t span)
{
    return (uint32_t)(now - then) >= span;
}

void km_report_sync_init(km_report_sync_t *s, const uint8_t *released, unsigned len, uint32_t now_cycles)
{
    memset(s, 0, sizeof *s);
    if (len > KM_SYNC_MAX_LEN) {
        len = KM_SYNC_MAX_LEN;
    }
    memcpy(s->released, released, len);
    memcpy(s->last, released, len);
    s->len = (uint8_t)len;
    s->force = true;                 /* nothing has been sent yet: the first pass tells the host */
    s->last_tx = now_cycles;
}

void km_report_sync_resync(km_report_sync_t *s)
{
    s->force = true;
}

void km_report_sync_pump(km_report_sync_t *s, const km_sync_io_t *io, const uint8_t *desired, bool keys_allowed, uint32_t now_cycles)
{
    if (!io->link_up(io->ctx)) {
        if (s->link_was_up) {
            s->link_was_up = false;
            s->force = true;         /* whatever the host holds is unknown until the link is back */
        }
        s->busy_timing = false;
        s->undelivered = false;      /* nobody to deliver to: not counted as a dead endpoint */
        return;
    }
    if (!s->link_was_up) {
        s->link_was_up = true;
        s->force = true;
        s->resyncs++;
    }

    const uint8_t *target = keys_allowed ? desired : s->released;
    const bool differs = memcmp(target, s->last, s->len) != 0;
    const bool any_down = memcmp(target, s->released, s->len) != 0;
    /* While a key is down the state is repeated every heartbeat; after a release it is repeated once (the echo), so a release
     * that was accepted but never arrived heals as well. */
    const bool heartbeat = (any_down || s->echo_pending) && !differs && elapsed(now_cycles, s->last_tx, KM_SYNC_HEARTBEAT_CYCLES);
    if (!differs && !s->force && !heartbeat) {
        if (s->undelivered && memcmp(target, s->pend, s->len) != 0) {
            s->coalesced++;                              /* it was due, and by the time the endpoint was free the state was back to what the host holds */
        }
        s->busy_timing = false;
        s->undelivered = false;
        return;
    }
    if (!s->undelivered) {
        s->undelivered_since = now_cycles;
        s->blocked_noted = false;
        memcpy(s->pend, target, s->len);
    } else if (memcmp(target, s->pend, s->len) != 0) {
        s->coalesced++;                                  /* the state moved on while the previous one was still waiting for the endpoint */
        memcpy(s->pend, target, s->len);
    }
    s->undelivered = true;

    if (!io->tx_ready(io->ctx)) {
        if (!s->blocked_noted) {
            s->blocked_noted = true;
            s->deferred++;
        }
        if (!s->busy_timing) {
            s->busy_timing = true;
            s->busy_since = now_cycles;
        } else if (elapsed(now_cycles, s->busy_since, KM_SYNC_BUSY_LIMIT_CYCLES)) {
            io->recover(io->ctx);
            s->recoveries++;
            s->busy_since = now_cycles;
        }
        return;
    }
    s->busy_timing = false;
    if (io->send(io->ctx, target, s->len)) {
        if (!keys_allowed && differs) {
            s->forced_releases++;
        }
        if (heartbeat) {
            s->heartbeats++;
        }
        s->echo_pending = !any_down && (differs ? s->had_down : s->echo_pending && !heartbeat);
        s->had_down = any_down;
        memcpy(s->last, target, s->len);
        s->force = false;
        s->last_tx = now_cycles;
        s->sent++;
        if (!heartbeat) {
            const uint32_t w = (uint32_t)(now_cycles - s->undelivered_since);
            s->wait_last = w;
            if (w > s->wait_max) {
                s->wait_max = w;
            }
            if (w > KM_SYNC_LATE_CYCLES) {
                s->late++;
            }
        }
        s->undelivered = false;
    } else {
        s->retries++;
    }
}

bool km_report_sync_stalled(const km_report_sync_t *s, uint32_t now_cycles)
{
    return s->undelivered && elapsed(now_cycles, s->undelivered_since, KM_SYNC_DEAD_CYCLES);
}
