/*
 * km_dks.c - Dynamic Keystroke (M6 package P3). Rules and rationale: km_dks.h and core/DKS_NOTES.md.
 *
 * Per pass: (1) every pressed DKS key is found or entered in a small table of KM_DKS_KEYS entries; (2) per tracked key the
 * GENERATOR turns "in the list / depth" into events 1..4 and puts them in a short queue; (3) the APPLIER takes at most one
 * event per key and pass out of the queue (not before KM_DKS_STEP_MS after the previous one) and changes the "action is down"
 * bits; (4) the output is the union of the usages of all down actions. A key leaves the table when nothing is left to do.
 * Everything is bounded by KM_DKS_KEYS and KM_DKS_ACTIONS; nothing is allocated.
 */
#include "km/km_dks.h"

#include <string.h>

#define FREE_SLOT 0xFFu

#define PH_OPEN   0x01u   /* event 1 has been generated, event 4 not yet */
#define PH_BOTTOM 0x02u   /* event 2 has been generated and event 3 not yet */
#define PH_DONE   0x04u   /* event 2 has fired in this press (needed for the "not again" rule) */

#define EV_MASK_BOTTOM 0x06u   /* events 2 and 3 */

static inline bool is_before(uint32_t t, uint32_t now)   /* t <= now, wrap safe (spans stay far below 2^31 ms) */
{
    return (int32_t)(now - t) >= 0;
}

uint16_t km_dks_t_bot(const km_dks_rec_t *r)
{
    return r->t_bot_mm100 != 0u ? r->t_bot_mm100 : (uint16_t)KM_DKS_BOT_DEFAULT_MM100;
}

bool km_dks_rec_valid(const km_dks_rec_t *r)
{
    if (r == NULL || r->t_act_mm100 < KM_DKS_ACT_MIN_MM100 || r->flags > KM_DKS_FLAG_RETRIGGER || r->rsvd[0] != 0u ||
        r->rsvd[1] != 0u || r->rsvd[2] != 0u) {
        return false;
    }
    const unsigned bot = km_dks_t_bot(r);
    if (bot > KM_DKS_TRAVEL_MM100 || bot < (unsigned)r->t_act_mm100 + KM_DKS_GAP_MIN_MM100) {
        return false;
    }
    for (unsigned a = 0; a < KM_DKS_ACTIONS; a++) {
        const unsigned u = r->act[a].usage;
        if (u != 0u && (u < 0x04u || u > 0xE7u)) {
            return false;
        }
    }
    return true;
}

bool km_dks_engine_points(const km_dks_rec_t *r, uint16_t *press_mm100, uint16_t *release_mm100)
{
    if (!km_dks_rec_valid(r) || press_mm100 == NULL || release_mm100 == NULL) {
        return false;
    }
    *press_mm100 = r->t_act_mm100;
    *release_mm100 = (uint16_t)(r->t_act_mm100 - KM_DKS_RELEASE_BELOW_MM100);
    return true;
}

static void clear_keys(km_dks_t *d)
{
    for (unsigned i = 0; i < KM_DKS_KEYS; i++) {
        memset(&d->key[i], 0, sizeof d->key[i]);
        d->key[i].slot = FREE_SLOT;
    }
    memset(d->where, 0, sizeof d->where);
    d->n_tracked = 0;
    d->dirty = true;                                      /* the output must be rebuilt (and is empty) */
}

void km_dks_init(km_dks_t *d)
{
    memset(d, 0, sizeof *d);
    clear_keys(d);
}

void km_dks_reset(km_dks_t *d)
{
    clear_keys(d);
    d->resets++;
}

bool km_dks_set_records(km_dks_t *d, const km_dks_rec_t *recs, unsigned n)
{
    memset(d->rec, 0, sizeof d->rec);
    memset(d->ok, 0, sizeof d->ok);
    memset(d->ev_mask, 0, sizeof d->ev_mask);
    clear_keys(d);
    if (n == 0u) {
        return true;
    }
    if (recs == NULL || n > KM_DKS_MAX) {
        return false;
    }
    for (unsigned i = 0; i < n; i++) {
        if (!km_dks_rec_valid(&recs[i])) {
            return false;                                 /* nothing has been stored yet: all DKS stays off */
        }
    }
    for (unsigned i = 0; i < n; i++) {
        d->rec[i] = recs[i];
        d->ok[i] = 1u;
        uint8_t mask = 0;
        for (unsigned a = 0; a < KM_DKS_ACTIONS; a++) {
            if (recs[i].act[a].usage != 0u) {
                for (unsigned e = 0; e < 4u; e++) {
                    if (((recs[i].act[a].ev >> (2u * e)) & 3u) != KM_DKS_EV_NONE) {
                        mask = (uint8_t)(mask | (1u << e));
                    }
                }
            }
        }
        d->ev_mask[i] = mask;
    }
    return true;
}

/* ---- applier ---- */

static void apply_event(km_dks_t *d, km_dks_key_t *k, unsigned rec, unsigned ev, uint32_t now)
{
    const km_dks_rec_t *r = &d->rec[rec];
    for (unsigned a = 0; a < KM_DKS_ACTIONS; a++) {
        if (r->act[a].usage == 0u) {
            continue;
        }
        const unsigned bit = 1u << a;
        switch ((r->act[a].ev >> (2u * (ev - 1u))) & 3u) {
        case KM_DKS_EV_PRESS:
            k->held = (uint8_t)(k->held | bit);
            k->tap = (uint8_t)(k->tap & ~bit);
            k->usage[a] = r->act[a].usage;
            break;
        case KM_DKS_EV_RELEASE:
            k->held = (uint8_t)(k->held & ~bit);
            k->tap = (uint8_t)(k->tap & ~bit);
            break;
        case KM_DKS_EV_TAP:
            if ((k->held & bit) == 0u) {
                k->held = (uint8_t)(k->held | bit);
                k->tap = (uint8_t)(k->tap | bit);
                k->usage[a] = r->act[a].usage;
                k->tap_due[a] = now + KM_DKS_TAP_MS;
            } else if ((k->tap & bit) != 0u) {
                k->tap_due[a] = now + KM_DKS_TAP_MS;      /* a running pulse starts over; a held action stays as it is */
            }
            break;
        default:
            break;
        }
    }
    if (ev == 4u) {
        k->held = (uint8_t)(k->held & k->tap);            /* the key is up: only tap pulses may still finish */
    }
    d->events_applied++;
    d->dirty = true;
}

static void apply_queued(km_dks_t *d, km_dks_key_t *k, uint32_t now)
{
    const unsigned e = k->q[0];
    k->q_n--;
    memmove(&k->q[0], &k->q[1], k->q_n);
    apply_event(d, k, e >> 2, (e & 3u) + 1u, now);
}

/* ---- generator ---- */

static void enqueue(km_dks_t *d, km_dks_key_t *k, unsigned ev, uint32_t now)
{
    if (ev != 4u && (d->ev_mask[k->rec] & (1u << (ev - 1u))) == 0u) {
        return;                                           /* nothing is configured for this event */
    }
    if (k->q_n >= KM_DKS_QUEUE) {                         /* never lose an event: apply what waits, at once, in order */
        while (k->q_n != 0u) {
            apply_queued(d, k, now);
        }
        d->queue_flushes++;
    }
    k->q[k->q_n++] = (uint8_t)((k->rec << 2) | (ev - 1u));
}

static void close_press(km_dks_t *d, km_dks_key_t *k, uint32_t now)
{
    if ((k->phase & PH_BOTTOM) != 0u) {
        enqueue(d, k, 3u, now);
    }
    enqueue(d, k, 4u, now);
    k->phase = 0;
}

static km_dks_key_t *find_key(km_dks_t *d, unsigned slot)
{
    const unsigned w = d->where[slot];
    return w != 0u ? &d->key[w - 1u] : NULL;
}

static km_dks_key_t *alloc_key(km_dks_t *d, unsigned slot)
{
    if (d->n_tracked >= KM_DKS_KEYS) {
        return NULL;
    }
    for (unsigned i = 0; i < KM_DKS_KEYS; i++) {
        if (d->key[i].slot == FREE_SLOT) {
            d->where[slot] = (uint8_t)(i + 1u);
            d->n_tracked++;
            return &d->key[i];
        }
    }
    return NULL;
}

static void free_key(km_dks_t *d, km_dks_key_t *k)
{
    d->where[k->slot] = 0;
    k->slot = FREE_SLOT;
    d->n_tracked--;
    d->dirty = true;
}

/* the union of what is down, each usage once, in the order key table / action number */
static void build_out(km_dks_t *d)
{
    uint32_t seen_usage[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    unsigned cnt = 0;
    for (unsigned i = 0; i < KM_DKS_KEYS; i++) {
        const km_dks_key_t *k = &d->key[i];
        if (k->slot == FREE_SLOT || k->held == 0u) {
            continue;
        }
        for (unsigned a = 0; a < KM_DKS_ACTIONS; a++) {
            if ((k->held & (1u << a)) != 0u) {
                const unsigned u = k->usage[a];
                if (((seen_usage[u >> 5] >> (u & 31u)) & 1u) == 0u) {
                    seen_usage[u >> 5] |= 1u << (u & 31u);
                    d->cur.usage[cnt++] = (uint8_t)u;
                }
            }
        }
    }
    d->cur.n = (uint8_t)cnt;
    d->dirty = false;
}

unsigned km_dks_update_ms(km_dks_t *d, const km_dks_press_t *down, unsigned n, uint32_t now, km_dks_depth_fn depth, void *ctx,
                          km_dks_out_t *out)
{
    if (n == 0u && d->n_tracked == 0u) {                  /* idle: nothing pressed, nothing to finish */
        if (d->dirty) {
            build_out(d);
        }
        *out = d->cur;
        return out->n;
    }
    for (unsigned i = 0; i < KM_DKS_KEYS; i++) {
        d->key[i].seen = 0;
    }
    /* 1. which DKS keys are pressed (a bad record index or a full table is ignored: nothing to track, nothing to release) */
    for (unsigned i = 0; i < n; i++) {
        const unsigned slot = down[i].slot;
        const unsigned rec = down[i].rec;
        if (slot >= KM_DKS_SLOTS || rec >= KM_DKS_MAX || d->ok[rec] == 0u) {
            continue;
        }
        km_dks_key_t *k = find_key(d, slot);
        if (k == NULL) {
            k = alloc_key(d, slot);
            if (k == NULL) {
                d->overflow_presses++;
                continue;
            }
            k->slot = (uint8_t)slot;
            k->rec = (uint8_t)rec;
            k->next_ms = now;
        } else if (k->rec != rec) {
            if ((k->phase & PH_OPEN) != 0u) {
                close_press(d, k, now);                   /* released and pressed again with another record */
            }
            k->rec = (uint8_t)rec;
        }
        k->seen = 1u;
    }
    /* 2. generate, 3. apply, then free the entries that have finished */
    for (unsigned i = 0; i < KM_DKS_KEYS; i++) {
        km_dks_key_t *k = &d->key[i];
        if (k->slot == FREE_SLOT) {
            continue;
        }
        if (k->seen != 0u) {
            if ((k->phase & PH_OPEN) == 0u) {
                k->phase = PH_OPEN;
                enqueue(d, k, 1u, now);
            }
            if (depth != NULL && (d->ev_mask[k->rec] & EV_MASK_BOTTOM) != 0u) {
                const unsigned dp = depth(ctx, k->slot);
                const unsigned bot = km_dks_t_bot(&d->rec[k->rec]);
                if ((k->phase & PH_BOTTOM) == 0u) {
                    if (dp >= bot && ((k->phase & PH_DONE) == 0u || (d->rec[k->rec].flags & KM_DKS_FLAG_RETRIGGER) != 0u)) {
                        k->phase = (uint8_t)(k->phase | PH_BOTTOM | PH_DONE);
                        enqueue(d, k, 2u, now);
                    }
                } else if (dp != 0u && dp + KM_DKS_HYST_MM100 <= bot) {
                    k->phase = (uint8_t)(k->phase & ~PH_BOTTOM);
                    enqueue(d, k, 3u, now);
                }
            }
        } else if ((k->phase & PH_OPEN) != 0u) {
            close_press(d, k, now);
        }
        /* a key idle for a very long time must not let its clock wrap into the "future" */
        if (k->q_n == 0u && (uint32_t)(now - k->next_ms) > 0x40000000u) {
            k->next_ms = now;
        }
        if (k->tap != 0u) {
            for (unsigned a = 0; a < KM_DKS_ACTIONS; a++) {
                if ((k->tap & (1u << a)) != 0u && is_before(k->tap_due[a], now)) {
                    k->held = (uint8_t)(k->held & ~(1u << a));
                    k->tap = (uint8_t)(k->tap & ~(1u << a));
                    d->dirty = true;
                }
            }
        }
        if (k->q_n != 0u && is_before(k->next_ms, now)) {
            apply_queued(d, k, now);
            k->next_ms = now + KM_DKS_STEP_MS;
        }
        if (k->seen == 0u && k->phase == 0u && k->q_n == 0u && k->held == 0u) {
            free_key(d, k);
        }
    }
    /* 4. the union of what is down (rebuilt only when an action went down or up) */
    if (d->dirty) {
        build_out(d);
    }
    *out = d->cur;
    return out->n;
}

unsigned km_dks_update_cycles(km_dks_t *d, const km_dks_press_t *down, unsigned n, uint32_t now_cycles, uint32_t cycles_per_ms,
                              km_dks_depth_fn depth, void *ctx, km_dks_out_t *out)
{
    if (!d->clk_init) {
        d->clk_init = true;
        d->clk_prev = now_cycles;
    }
    if (cycles_per_ms != 0u) {
        const uint32_t whole = (now_cycles - d->clk_prev) / cycles_per_ms;
        d->ms += whole;
        d->clk_prev += whole * cycles_per_ms;
    } else {
        d->ms += KM_DKS_STEP_MS;                          /* no clock: a pass counts as one step, queued events still drain */
    }
    return km_dks_update_ms(d, down, n, d->ms, depth, ctx, out);
}

unsigned km_dks_merge(uint8_t *kbd, unsigned n_kbd, unsigned cap, const km_dks_out_t *dks)
{
    for (unsigned i = 0; i < dks->n; i++) {
        bool have = false;
        for (unsigned j = 0; j < n_kbd; j++) {
            have = have || kbd[j] == dks->usage[i];
        }
        if (!have && n_kbd < cap) {
            kbd[n_kbd++] = dks->usage[i];
        }
    }
    return n_kbd;
}

bool km_dks_busy(const km_dks_t *d)
{
    for (unsigned i = 0; i < KM_DKS_KEYS; i++) {
        if (d->key[i].slot != FREE_SLOT) {
            return true;
        }
    }
    return false;
}
