#include "km/km_ledprof.h"

#include <string.h>

void km_ledprof_default(km_ledprof_t *t)
{
    t->mask = 0;
    for (unsigned i = 0; i < KM_LEDPROF_ENTRIES; i++) {
        km_ledstore_rec_default(&t->rec[i]);
    }
}

bool km_ledprof_has_own(const km_ledprof_t *t, unsigned profile)
{
    return profile < KM_LEDPROF_PROFILES && (t->mask & (1u << (profile + 1u))) != 0u;
}

bool km_ledprof_effective(const km_ledprof_t *t, unsigned profile, km_ledstore_rec_t *r)
{
    if (profile >= KM_LEDPROF_PROFILES) {
        return false;
    }
    if (km_ledprof_has_own(t, profile)) {
        *r = t->rec[profile + 1u];
        return true;
    }
    if ((t->mask & 1u) != 0u) {
        *r = t->rec[0];
        return true;
    }
    return false;
}

bool km_ledprof_set(km_ledprof_t *t, unsigned profile, const km_ledstore_rec_t *r)
{
    if (profile >= KM_LEDPROF_PROFILES || !km_ledstore_rec_valid(r)) {
        return false;
    }
    t->rec[profile + 1u] = *r;
    t->mask = (uint8_t)(t->mask | (1u << (profile + 1u)));
    return true;
}

bool km_ledprof_set_global(km_ledprof_t *t, const km_ledstore_rec_t *r)
{
    if (!km_ledstore_rec_valid(r)) {
        return false;
    }
    t->rec[0] = *r;
    t->mask |= 1u;
    return true;
}

bool km_ledprof_clear(km_ledprof_t *t, unsigned profile)
{
    if (profile >= KM_LEDPROF_PROFILES) {
        return false;
    }
    t->mask = (uint8_t)(t->mask & ~(1u << (profile + 1u)));
    km_ledstore_rec_default(&t->rec[profile + 1u]);
    return true;
}

unsigned km_ledprof_encode(const km_ledprof_t *t, uint8_t *buf, unsigned cap)
{
    if (buf == NULL || cap < KM_LEDPROF_PAYLOAD_BYTES || (t->mask & ~0x1Fu) != 0u) {
        return 0;
    }
    for (unsigned i = 0; i < KM_LEDPROF_ENTRIES; i++) {
        if ((t->mask & (1u << i)) != 0u && !km_ledstore_rec_valid(&t->rec[i])) {
            return 0;
        }
    }
    memset(buf, 0, KM_LEDPROF_PAYLOAD_BYTES);
    buf[0] = (uint8_t)KM_LEDPROF_VERSION;
    buf[1] = t->mask;
    for (unsigned i = 0; i < KM_LEDPROF_ENTRIES; i++) {
        if ((t->mask & (1u << i)) == 0u) {
            continue;                                 /* a cleared entry is stored as zeros, whatever the RAM copy holds */
        }
        uint8_t *e = buf + KM_LEDPROF_HEADER_BYTES + i * KM_LEDPROF_ENTRY_BYTES;
        const km_ledstore_rec_t *r = &t->rec[i];
        e[0] = r->cfg.effect;
        e[1] = r->cfg.brightness;
        e[2] = r->cfg.speed;
        e[3] = r->cfg.color.r;
        e[4] = r->cfg.color.g;
        e[5] = r->cfg.color.b;
        e[6] = (uint8_t)(r->idle_s & 0xFFu);
        e[7] = (uint8_t)(r->idle_s >> 8);
    }
    return KM_LEDPROF_PAYLOAD_BYTES;
}

bool km_ledprof_decode(km_ledprof_t *t, const uint8_t *buf, unsigned len)
{
    if (t == NULL || buf == NULL || len != KM_LEDPROF_PAYLOAD_BYTES || buf[0] != KM_LEDPROF_VERSION || (buf[1] & ~0x1Fu) != 0u ||
        buf[2] != 0u || buf[3] != 0u) {
        return false;
    }
    km_ledprof_t n;
    km_ledprof_default(&n);
    n.mask = buf[1];
    for (unsigned i = 0; i < KM_LEDPROF_ENTRIES; i++) {
        const uint8_t *e = buf + KM_LEDPROF_HEADER_BYTES + i * KM_LEDPROF_ENTRY_BYTES;
        if ((n.mask & (1u << i)) == 0u) {
            for (unsigned k = 0; k < KM_LEDPROF_ENTRY_BYTES; k++) {
                if (e[k] != 0u) {
                    return false;
                }
            }
            continue;
        }
        if (e[8] != 0u || e[9] != 0u || e[10] != 0u || e[11] != 0u) {
            return false;                             /* scene reference and reserved bytes: a future version must not be misread */
        }
        km_ledstore_rec_t *r = &n.rec[i];
        r->cfg.effect = e[0];
        r->cfg.brightness = e[1];
        r->cfg.speed = e[2];
        r->cfg.color.r = e[3];
        r->cfg.color.g = e[4];
        r->cfg.color.b = e[5];
        r->idle_s = (uint16_t)(e[6] | ((unsigned)e[7] << 8));
        if (!km_ledstore_rec_valid(r)) {
            return false;
        }
    }
    *t = n;
    return true;
}

bool km_ledprof_load(const km_clog_t *log, const km_clog_t *legacy, km_ledprof_t *t)
{
    uint8_t buf[KM_LEDPROF_PAYLOAD_BYTES];
    if (log != NULL && log->payload_len == KM_LEDPROF_PAYLOAD_BYTES && km_clog_read(log, buf, sizeof buf) &&
        km_ledprof_decode(t, buf, sizeof buf)) {
        return true;
    }
    km_ledstore_rec_t r;
    if (legacy != NULL && km_ledstore_load(legacy, &r)) {
        km_ledprof_t n;
        km_ledprof_default(&n);
        (void)km_ledprof_set_global(&n, &r);
        *t = n;
        return true;
    }
    return false;
}
