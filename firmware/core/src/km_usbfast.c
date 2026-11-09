#include "km/km_usbfast.h"

#include <string.h>

void km_defer_init(km_defer_t *d)
{
    memset(d, 0, sizeof *d);
}

bool km_defer_slow_ok(km_defer_t *d, bool event, uint32_t now, uint32_t max_cycles)
{
    if (!event) {
        d->active = false;
        return true;
    }
    if (!d->active) {
        d->active = true;
        d->since = now;
    }
    if ((uint32_t)(now - d->since) >= max_cycles) {
        d->forced++;
        d->since = now;                  /* the window restarts: at most one run per max_cycles under constant events */
        return true;
    }
    d->skipped++;
    return false;
}

void km_ustat_init(km_ustat_t *u)
{
    memset(u, 0, sizeof *u);
}

void km_ustat_sent(km_ustat_t *u, uint32_t now, uint32_t sof)
{
    u->busy = true;
    u->t_sent = now;
    u->t_sof = sof;
}

static unsigned bucket_of(uint32_t cyc)
{
    /* 62.5 us = 15000 cycles, 125 us = 30000, 250 us, 500 us, 1 ms */
    if (cyc <= 15000u) { return 0; }
    if (cyc <= 30000u) { return 1; }
    if (cyc <= 60000u) { return 2; }
    if (cyc <= 120000u) { return 3; }
    if (cyc <= 240000u) { return 4; }
    return 5;
}

void km_ustat_observe(km_ustat_t *u, uint32_t now, bool ready, uint32_t sof)
{
    if (!u->busy || !ready) {
        return;
    }
    const uint32_t d = (uint32_t)(now - u->t_sent);
    u->busy = false;
    u->count++;
    u->bucket[bucket_of(d)]++;
    u->last = d;
    const uint32_t f = (uint32_t)(sof - u->t_sof);
    u->uf[f <= 2u ? f : f <= 4u ? 3u : f <= 7u ? 4u : 5u]++;
    if (f > u->uf_max) {
        u->uf_max = f;
    }
    if (d > u->max) {
        u->max = d;
    }
}

static uint16_t sat16(uint32_t v)
{
    return v > 0xFFFFu ? 0xFFFFu : (uint16_t)v;
}

void km_ustat_pack(const km_ustat_t *u, uint32_t cycles_per_us, uint16_t out[KM_USTAT_VALUES])
{
    out[0] = (uint16_t)u->count;
    for (unsigned i = 0; i < KM_USTAT_BUCKETS; i++) {
        out[1 + i] = sat16(u->bucket[i]);
    }
    out[7] = sat16(cycles_per_us != 0u ? u->max / cycles_per_us : 0u);
}

void km_ustat_pack_uf(const km_ustat_t *u, uint16_t out[KM_USTAT_VALUES])
{
    for (unsigned i = 0; i < KM_USTAT_UF; i++) {
        out[i] = sat16(u->uf[i]);
    }
    out[6] = (uint16_t)u->count;
    out[7] = sat16(u->uf_max);
}

unsigned km_usb_cfg_binterval(const uint8_t *cfg, unsigned len, uint8_t addr)
{
    unsigned i = 0;
    while (i + 2u <= len) {
        const unsigned l = cfg[i];
        if (l < 2u || i + l > len) {
            return 0;
        }
        if (cfg[i + 1] == 0x05u && l >= 7u && cfg[i + 2] == addr) {
            return cfg[i + 6];
        }
        i += l;
    }
    return 0;
}
