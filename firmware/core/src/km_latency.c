#include "km/km_latency.h"

#include <string.h>

void km_lat_init(km_lat_t *l)
{
    memset(l, 0, sizeof *l);
    l->min = 0xFFFFFFFFu;
}

void km_lat_observe(km_lat_t *l, uint32_t now, const uint8_t *down, unsigned n)
{
    uint32_t cur[KM_LAT_SLOT_WORDS] = { 0, 0, 0 };
    for (unsigned i = 0; i < n; i++) {
        const unsigned s = down[i];
        if (s < KM_LAT_SLOT_WORDS * 32u) {
            cur[s >> 5] |= 1u << (s & 31u);
        }
    }
    for (unsigned w = 0; w < KM_LAT_SLOT_WORDS; w++) {
        const uint32_t fresh = cur[w] & ~l->prev[w];
        for (unsigned b = 0; b < 32u; b++) {
            if ((fresh >> b) & 1u) {
                l->presses++;
                if (l->pending < KM_LAT_RING) {
                    l->press[l->pending++] = now;
                } else {
                    l->dropped++;
                }
            }
        }
        l->prev[w] = cur[w];
    }
}

void km_lat_sent(km_lat_t *l, uint32_t now)
{
    l->sends++;
    if (l->pending == 0u) {
        return;
    }
    const uint32_t d = now - l->press[0];       /* wrap-safe: cycles since the oldest waiting press */
    l->samples++;
    l->last = d;
    if (d < l->min) {
        l->min = d;
    }
    if (d > l->max) {
        l->max = d;
    }
    l->pending = 0u;
}

static uint16_t sat16(uint32_t v)
{
    return v > 0xFFFFu ? 0xFFFFu : (uint16_t)v;
}

void km_lat_pack(const km_lat_t *l, uint32_t cycles_per_us, uint16_t out[KM_LAT_VALUES])
{
    const uint32_t per = cycles_per_us == 0u ? 1u : cycles_per_us;
    out[0] = sat16(l->samples);
    out[1] = sat16(l->last / per);
    out[2] = sat16(l->samples == 0u ? 0u : l->min / per);
    out[3] = sat16(l->max / per);
    out[4] = sat16(l->presses);
    out[5] = sat16(l->dropped);
    out[6] = (uint16_t)(l->sends & 0xFFFFu);
    out[7] = l->pending;
}
