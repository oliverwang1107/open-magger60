/*
 * km_socd.c - SOCD filter (M6 package P1). Rules and rationale: km_socd.h and core/SOCD_NOTES.md.
 *
 * One pass over the engine's pressed list finds, for the (at most 8) slots that belong to a pair, where they sit in the
 * list; the position is the press order. Then each pair in conflict names its loser, and one more pass copies the list
 * without the losers. The loser of LAST / FIRST / NEUTRAL follows from the positions alone; only DEEPER keeps one byte of
 * state per pair (who currently wins), which is cleared the moment the pair is not in conflict.
 */
#include "km/km_socd.h"

#include <string.h>

#include "km/km_keymap.h"

#define NONE 0xFFu   /* position of a slot that is not in the list */

static void clear_config(km_socd_t *s)
{
    memset(s->slot_a, 0, sizeof s->slot_a);
    memset(s->slot_b, 0, sizeof s->slot_b);
    memset(s->mode, 0, sizeof s->mode);
    memset(s->flags, 0, sizeof s->flags);
    memset(s->owner, 0, sizeof s->owner);
    memset(s->role, 0, sizeof s->role);
    s->n_used = 0;
    memset(s->supp, 0, sizeof s->supp);
}

void km_socd_init(km_socd_t *s)
{
    memset(s, 0, sizeof *s);
}

void km_socd_reset(km_socd_t *s)
{
    memset(s->owner, 0, sizeof s->owner);
    memset(s->supp, 0, sizeof s->supp);
}

bool km_socd_pair_valid(const km_socd_pair_t *p)
{
    if (p == NULL || p->mode > KM_SOCD_MODE_LAST_VALID || p->flags > KM_SOCD_FLAG_LAYER0) {
        return false;
    }
    if (p->mode == KM_SOCD_OFF) {
        return true;                               /* an unused entry: its slots do not matter */
    }
    return p->slot_a != p->slot_b && km_keymap_slot_is_key(p->slot_a) && km_keymap_slot_is_key(p->slot_b);
}

bool km_socd_set_pairs(km_socd_t *s, const km_socd_pair_t *pairs, unsigned n)
{
    clear_config(s);
    if (n == 0u) {
        return true;                               /* no pairs: all off */
    }
    if (pairs == NULL || n > KM_SOCD_MAX) {
        return false;
    }
    for (unsigned i = 0; i < n; i++) {
        if (!km_socd_pair_valid(&pairs[i])) {
            clear_config(s);                       /* the pairs loaded so far are dropped too */
            return false;
        }
        if (pairs[i].mode == KM_SOCD_OFF) {
            continue;
        }
        if (s->role[pairs[i].slot_a] != 0u || s->role[pairs[i].slot_b] != 0u) {
            clear_config(s);                       /* a slot in two pairs */
            return false;
        }
        const unsigned u = s->n_used;              /* the used pairs are packed to the front: the pass loops over n_used only */
        s->role[pairs[i].slot_a] = (uint8_t)(1u + 2u * u);
        s->role[pairs[i].slot_b] = (uint8_t)(2u + 2u * u);
        s->slot_a[u] = pairs[i].slot_a;
        s->slot_b[u] = pairs[i].slot_b;
        s->mode[u] = pairs[i].mode;
        s->flags[u] = pairs[i].flags;
        s->n_used++;
    }
    return true;
}

static inline unsigned popcount_small(uint32_t x)  /* at most 8 bits are ever set */
{
    unsigned c = 0;
    while (x != 0u) {
        x &= x - 1u;
        c++;
    }
    return c;
}

static inline void mark(uint32_t supp[3], unsigned slot)
{
    supp[slot >> 5] |= 1u << (slot & 31u);
}

unsigned km_socd_filter(km_socd_t *s, const uint8_t *down_in, unsigned n_in, unsigned layer, km_socd_depth_fn depth,
                        void *ctx, uint8_t *down_out)
{
    uint32_t supp[3] = { 0u, 0u, 0u };

    if (s->n_used != 0u && n_in >= 2u) {          /* one key (or none) can not be in conflict */
        uint8_t pos[2u * KM_SOCD_MAX];
        memset(pos, NONE, sizeof pos);
        const unsigned scan = n_in < NONE ? n_in : NONE;   /* positions are bytes; the engine list never exceeds 64 */
        for (unsigned i = 0; i < scan; i++) {
            const unsigned slot = down_in[i];
            if (slot < KM_SOCD_SLOTS) {
                const unsigned r = s->role[slot];
                if (r != 0u && pos[r - 1u] == NONE) {
                    pos[r - 1u] = (uint8_t)i;
                }
            }
        }
        bool conflict = false;
        for (unsigned p = 0; p < s->n_used; p++) {
            const unsigned mode = s->mode[p];
            const unsigned pa = pos[2u * p];
            const unsigned pb = pos[2u * p + 1u];
            if (pa == NONE || pb == NONE || ((s->flags[p] & KM_SOCD_FLAG_LAYER0) != 0u && layer != 0u)) {
                s->owner[p] = 0u;                  /* no conflict (or the pair sleeps on this layer) */
                continue;
            }
            conflict = true;
            const unsigned a_first = pa < pb;      /* A was pressed earlier */
            const unsigned sa = s->slot_a[p];
            const unsigned sb = s->slot_b[p];
            switch (mode) {
            case KM_SOCD_LAST:
                mark(supp, a_first ? sa : sb);     /* the earlier key loses */
                break;
            case KM_SOCD_FIRST:
                mark(supp, a_first ? sb : sa);     /* the later key loses */
                break;
            case KM_SOCD_NEUTRAL:
                mark(supp, sa);
                mark(supp, sb);
                break;
            default: {                             /* KM_SOCD_DEEPER */
                if (s->owner[p] == 0u) {
                    s->owner[p] = a_first ? 1u : 2u;
                }
                if (depth != NULL) {
                    const unsigned da = depth(ctx, sa);
                    const unsigned db = depth(ctx, sb);
                    if (s->owner[p] == 1u) {
                        if (db >= da + KM_SOCD_DEEPER_HYST_MM100) {
                            s->owner[p] = 2u;
                        }
                    } else if (da >= db + KM_SOCD_DEEPER_HYST_MM100) {
                        s->owner[p] = 1u;
                    }
                }
                mark(supp, s->owner[p] == 1u ? sb : sa);
                break;
            }
            }
        }
        if (conflict) {
            s->calls_in_conflict++;
        }
    } else {
        memset(s->owner, 0, sizeof s->owner);      /* fewer than two keys pressed: no conflict can be running */
    }

    s->suppress_events += popcount_small(supp[0] & ~s->supp[0]) + popcount_small(supp[1] & ~s->supp[1]) +
                          popcount_small(supp[2] & ~s->supp[2]);
    s->supp[0] = supp[0];
    s->supp[1] = supp[1];
    s->supp[2] = supp[2];

    if ((supp[0] | supp[1] | supp[2]) == 0u) {
        if (down_out != down_in && n_in != 0u) {
            memcpy(down_out, down_in, n_in);
        }
        return n_in;
    }
    unsigned w = 0;
    for (unsigned i = 0; i < n_in; i++) {
        const unsigned slot = down_in[i];
        if (slot < KM_SOCD_SLOTS && ((supp[slot >> 5] >> (slot & 31u)) & 1u) != 0u) {
            continue;
        }
        down_out[w++] = (uint8_t)slot;
    }
    return w;
}

bool km_socd_suppressed(const km_socd_t *s, unsigned slot)
{
    return slot < KM_SOCD_SLOTS && ((s->supp[slot >> 5] >> (slot & 31u)) & 1u) != 0u;
}
