/*
 * km_layers.c - layers, FN, media, profile, toggle, tap-hold, one-shot, toggle-key and Mod-Morph keys (M5 tasks T-C and L4,
 * M6 task P2). Original work, rules: see km_layers.h.
 *
 * Structure. The pressed slots of every update are turned into press / release EVENTS (with the time they were seen). Events
 * go through one small resolver:
 *
 *   intake    compare the pressed set with the previous one; a release of a key the resolver already pressed is handled at
 *             once, everything else is appended to a FIFO (capacity KM_LAYERS_EVENTS)
 *   drain     pops events in order and runs press / release handling; stops while a tap-hold key is undecided (g_pend) and
 *             while the output is "paced" (g_stall: a tap pulse or a replayed event must be visible for a few ms)
 *   output    rebuilt from the per-slot records of the keys that are down, the tap pulse, and the one-shot modifiers
 *
 * A key's record (g_act) is made when its press is HANDLED and holds what it resolved to, so a key keeps the layer it was
 * pressed on until it is released (the T-C latching rule). Events that had to wait (behind an undecided tap-hold key or a
 * pulse) are paced KM_LAYERS_STEP_MS apart when they are replayed, so that no press is shorter than the host can see and no
 * key is "lost" between two reports.
 *
 * Consumer page (0x0C) usages, USB HID Usage Tables "Consumer Page": 0xB5 Scan Next Track, 0xB6 Scan Previous
 * Track, 0xCD Play/Pause, 0xE2 Mute, 0xE9 Volume Increment, 0xEA Volume Decrement.
 */
#include "km/km_layers.h"

#include <string.h>

#define NSLOTS KM_KEYMAP_SLOTS

typedef enum {
    ACT_NONE = 0,   /* not pressed (as far as the resolver is concerned) */
    ACT_INERT,      /* pressed, emits nothing (no key, reserved code, toggle key, profile key of an unknown kind) */
    ACT_KBD,        /* keyboard usage `code` */
    ACT_MEDIA,      /* media function code `code` */
    ACT_HOLD,       /* holds layer `code` */
    ACT_PROFILE,    /* profile-switch key: the request is made on release */
    ACT_OSM         /* one-shot modifier key (held = ordinary modifier), usage `code` */
} act_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t code;
    uint8_t carry;   /* one-shot modifiers (bit i = usage 0xE0 + i) this key carries; released with the key */
    uint8_t suppress;/* Mod-Morph: modifiers (bit i = usage 0xE0 + i) that are kept out of the report while this key is down */
    bool    sup_key; /* Mod-Morph: this key was made with a suppress mask (counted in g_sup_keys) */
    bool    used;    /* ACT_OSM: some other key was pressed while it was held */
    uint32_t t;      /* ACT_OSM: when it was pressed */
} act_t;

typedef struct {
    uint8_t  slot;
    bool     down;
    bool     waited;  /* it stayed in the queue across at least one drain: pace what follows it */
    uint32_t t;
} ev_t;

static act_t     g_act[NSLOTS];
static km_layers_entry_t g_ent[NSLOTS]; /* the table entry each key in g_act resolved to (km_layers_locked_entry) */
static uint8_t   g_order[NSLOTS];        /* slots with g_act.kind != ACT_NONE, in the order they were handled */
static unsigned  g_order_n;
static bool      g_raw[NSLOTS];          /* the pressed set of the previous update */
static uint8_t   g_holds[KM_LAYERS];     /* how many held keys hold each layer (index 0 unused) */
static uint8_t   g_toggled;              /* bit l = layer l is toggled on (bits 1..3) */
static uint8_t   g_tk[KM_LAYERS_TOGGLE_KEYS]; /* toggle keys that are on: their usages, in the order they were switched on */
static unsigned  g_tk_n;
static km_morph_rec_t g_morph[KM_MORPH_MAX];  /* Mod-Morph records of the active profile (km_layers_set_morph) */
static unsigned  g_morph_n;
static unsigned  g_sup_keys;             /* down keys that carry a Mod-Morph suppress mask */
static ev_t      g_q[KM_LAYERS_EVENTS];
static unsigned  g_qn;

static struct {
    bool     on;
    uint8_t  slot, tap, hold, policy;
    uint16_t term;
    uint32_t t0;
} g_pend;

static struct {
    bool     on;
    bool     media;
    uint8_t  code;
    uint8_t  mods;
    uint32_t until;
} g_pulse;

static bool     g_stall_on;
static uint32_t g_stall_until;
static uint8_t  g_os_armed;              /* one-shot modifiers waiting for the next key */
static uint32_t g_os_t;
static uint8_t  g_profile_req;
static uint32_t g_gen;
static uint32_t g_now;
static uint32_t g_clock_ms;              /* km_layers_update_cycles(): milliseconds accumulated from the cycle counter */
static uint32_t g_clock_prev;
static bool     g_clock_init;
static bool     g_ready;

static bool time_reached(uint32_t now, uint32_t t)
{
    return (int32_t)(now - t) >= 0;
}

void km_layers_reset(void)
{
    memset(g_act, 0, sizeof g_act);
    memset(g_ent, 0, sizeof g_ent);
    memset(g_raw, 0, sizeof g_raw);
    memset(g_holds, 0, sizeof g_holds);
    memset(g_q, 0, sizeof g_q);
    memset(&g_pend, 0, sizeof g_pend);
    memset(&g_pulse, 0, sizeof g_pulse);
    g_order_n = 0;
    g_qn = 0;
    g_toggled = 0;
    g_tk_n = 0;
    g_sup_keys = 0;
    g_stall_on = false;
    g_os_armed = 0;
    g_profile_req = 0;
    g_gen = km_keymap_generation();
    g_ready = true;
}

void km_layers_set_morph(const km_morph_rec_t *rec, unsigned n)
{
    memset(g_morph, 0, sizeof g_morph);
    g_morph_n = 0;
    if (rec == NULL) {
        return;
    }
    if (n > KM_MORPH_MAX) {
        n = KM_MORPH_MAX;
    }
    for (unsigned i = 0; i < n; i++) {
        /* a record that breaks the rules of km_settings_morph_valid() stays all-zero: its mask is empty, it never morphs */
        if (rec[i].mod_mask != 0u && rec[i].usage >= 0x04u && rec[i].usage <= 0xE7u &&
            (rec[i].flags & ~KM_MORPH_FLAGS_MASK) == 0u && rec[i].rsvd == 0u) {
            g_morph[i] = rec[i];
        }
    }
    g_morph_n = n;
}

uint16_t km_layers_consumer_usage(uint8_t code)
{
    switch (code) {
    case KM_CODE_VOL_DOWN:   return 0x00EAu;
    case KM_CODE_VOL_UP:     return 0x00E9u;
    case KM_CODE_MUTE:       return 0x00E2u;
    case KM_CODE_PREV_TRACK: return 0x00B6u;
    case KM_CODE_PLAY_PAUSE: return 0x00CDu;
    case KM_CODE_NEXT_TRACK: return 0x00B5u;
    default:                 return 0;
    }
}

/* Which layer a hold code switches on (0 when the code holds nothing). */
static unsigned hold_layer(uint8_t code)
{
    switch (code) {
    case KM_CODE_HOLD_L1: return 1;
    case KM_CODE_HOLD_L2: return 2;
    case KM_CODE_HOLD_L3: return 3;
    default:              return 0;
    }
}

static unsigned active_layer(void)
{
    for (unsigned l = KM_LAYERS - 1u; l > 0; l--) {
        if (g_holds[l] != 0 || (g_toggled & (1u << l)) != 0) {
            return l;
        }
    }
    return 0;
}

typedef struct {
    uint8_t code, a0, a1;
} entry_t;

/* The entry a key gets when it is pressed now. */
static entry_t resolve(unsigned slot)
{
    unsigned layer = active_layer();
    uint8_t code = km_keymap_code(layer, slot);
    if (code == KM_CODE_TRANSPARENT) {
        layer = 0;
        code = km_keymap_code(0, slot);
    }
    entry_t e = { code, km_keymap_arg(layer, slot, 0), km_keymap_arg(layer, slot, 1) };
    if (code == KM_CODE_TRANSPARENT) {
        e.code = (uint8_t)KM_USAGE_NONE;
        e.a0 = e.a1 = 0;
    }
    return e;
}

/* ---- the table of pressed keys ---- */

static void act_set(unsigned slot, uint8_t kind, uint8_t code, uint8_t carry)
{
    g_act[slot].kind = kind;
    g_act[slot].code = code;
    g_act[slot].carry = carry;
    g_act[slot].suppress = 0;
    g_act[slot].sup_key = false;
    g_act[slot].used = false;
    g_order[g_order_n++] = (uint8_t)slot;
    if (kind == ACT_HOLD) {
        g_holds[code]++;
    }
}

static void act_clear(unsigned slot)
{
    for (unsigned i = 0; i < g_order_n; i++) {
        if (g_order[i] == slot) {
            memmove(&g_order[i], &g_order[i + 1u], g_order_n - i - 1u);
            g_order_n--;
            break;
        }
    }
    if (g_act[slot].sup_key && g_sup_keys > 0) {
        g_sup_keys--;
    }
    g_act[slot].sup_key = false;
    g_act[slot].suppress = 0;
    g_act[slot].kind = ACT_NONE;
}

/* A key press was handled: every one-shot modifier key that is held now counts as "used as a normal modifier". */
static void note_press_for_osm(void)
{
    for (unsigned i = 0; i < g_order_n; i++) {
        if (g_act[g_order[i]].kind == ACT_OSM) {
            g_act[g_order[i]].used = true;
        }
    }
}

/* The key that is pressed now takes the armed one-shot modifiers with it. */
static uint8_t take_armed(void)
{
    const uint8_t m = g_os_armed;
    g_os_armed = 0;
    return m;
}

static void stall_for(uint32_t now, uint32_t ms)
{
    g_stall_on = true;
    g_stall_until = now + ms;
}

static bool stalled(uint32_t now)
{
    if (g_stall_on && time_reached(now, g_stall_until)) {
        g_stall_on = false;
    }
    return g_stall_on;
}

/* ---- tap-hold ---- */

static void pend_hold(uint32_t now)
{
    const unsigned slot = g_pend.slot;
    const uint8_t hold = g_pend.hold;
    g_pend.on = false;
    note_press_for_osm();
    const unsigned hl = hold_layer(hold);
    if (hl != 0) {
        act_set(slot, ACT_HOLD, (uint8_t)hl, 0);
    } else {
        /* a hold action that is an ordinary key (format 3) takes an armed one-shot modifier like any key; a modifier does not */
        act_set(slot, ACT_KBD, hold, km_usage_is_modifier(hold) ? 0u : take_armed());
        stall_for(now, KM_LAYERS_STEP_MS);   /* the modifier is seen on its own before the key that waited behind it */
    }
}

static void pend_tap(uint32_t now)
{
    const uint8_t tap = g_pend.tap;
    g_pend.on = false;
    note_press_for_osm();
    g_pulse.on = true;
    g_pulse.media = tap >= KM_CODE_VOL_DOWN;
    g_pulse.code = tap;
    g_pulse.mods = (!g_pulse.media && km_usage_is_modifier(tap)) ? 0u : take_armed();
    g_pulse.until = now + KM_LAYERS_STEP_MS;
    stall_for(now, KM_LAYERS_STEP_MS);
}

static void queue_remove(unsigned i)
{
    memmove(&g_q[i], &g_q[i + 1u], (g_qn - i - 1u) * sizeof g_q[0]);
    g_qn--;
}

/* Decide the undecided tap-hold key from the queued events and the time. true = decided (g_pend.on is now false). */
static bool pend_decide(uint32_t now)
{
    const uint32_t deadline = g_pend.t0 + g_pend.term;
    bool others[NSLOTS];
    memset(others, 0, sizeof others);
    for (unsigned i = 0; i < g_qn; i++) {
        const ev_t *e = &g_q[i];
        if (time_reached(e->t, deadline)) {          /* the timeout came before this event */
            pend_hold(now);
            return true;
        }
        if (!e->down && e->slot == g_pend.slot) {    /* released in time: a tap */
            queue_remove(i);
            pend_tap(now);
            return true;
        }
        if (e->down && e->slot != g_pend.slot) {
            if (g_pend.policy == KM_TAP_POLICY_OTHER_KEY) {
                pend_hold(now);
                return true;
            }
            others[e->slot] = true;
        } else if (!e->down && others[e->slot] && g_pend.policy == KM_TAP_POLICY_PERMISSIVE) {
            pend_hold(now);                          /* another key was pressed and released inside this one */
            return true;
        }
    }
    if (time_reached(now, deadline)) {
        pend_hold(now);
        return true;
    }
    return false;
}

/* ---- toggle keys and Mod-Morph (M6) ---- */

/* A toggle key was pressed: usage u goes on (at the end of the list) or, when it is on, off. A full list ignores a new key. */
static void toggle_key(uint8_t u)
{
    for (unsigned i = 0; i < g_tk_n; i++) {
        if (g_tk[i] == u) {
            memmove(&g_tk[i], &g_tk[i + 1u], g_tk_n - i - 1u);
            g_tk_n--;
            return;
        }
    }
    if (g_tk_n < KM_LAYERS_TOGGLE_KEYS) {
        g_tk[g_tk_n++] = u;
    }
}

static uint8_t mod_bit(uint8_t usage)
{
    return km_usage_is_modifier(usage) ? (uint8_t)(1u << (usage - KM_USAGE_MOD_FIRST)) : 0u;
}

/* The modifiers (bit i = usage 0xE0 + i) that the keys handled so far are sending, before any Mod-Morph suppression: held
 * modifier keys, modifiers carried by keys, armed one-shot modifiers (the next key takes them) and toggled modifier keys. */
static uint8_t raw_mods(void)
{
    uint8_t m = g_os_armed;
    for (unsigned i = 0; i < g_order_n; i++) {
        const act_t *a = &g_act[g_order[i]];
        m |= a->carry;
        if (a->kind == ACT_KBD || a->kind == ACT_OSM) {
            m |= mod_bit(a->code);
        }
    }
    for (unsigned i = 0; i < g_tk_n; i++) {
        m |= mod_bit(g_tk[i]);
    }
    return m;
}

/* A Mod-Morph key (arg0 = usage, arg1 = record) was pressed: it becomes the record's usage if one of the record's modifiers is
 * being sent right now, and keeps that for as long as it is down. */
static void press_morph(unsigned slot, entry_t e)
{
    uint8_t usage = e.a0;
    uint8_t sup = 0;
    if (e.a1 < g_morph_n) {
        const km_morph_rec_t *r = &g_morph[e.a1];
        const uint8_t hit = (uint8_t)(raw_mods() & r->mod_mask);
        if (hit != 0u) {
            usage = r->usage;
            if ((r->flags & KM_MORPH_FLAG_SUPPRESS) != 0u) {
                sup = (uint8_t)(hit & ~mod_bit(usage));   /* never suppress the key's own output */
            }
        }
    }
    note_press_for_osm();
    act_set(slot, ACT_KBD, usage, km_usage_is_modifier(usage) ? 0u : take_armed());
    if (sup != 0u) {
        g_act[slot].suppress = sup;
        g_act[slot].sup_key = true;
        g_sup_keys++;
    }
}

/* ---- press / release handling ---- */

static void handle_press(unsigned slot, uint32_t t)
{
    const entry_t e = resolve(slot);
    g_ent[slot].code = e.code;
    g_ent[slot].arg0 = e.a0;
    g_ent[slot].arg1 = e.a1;
    switch (e.code) {
    case KM_CODE_TAP_HOLD:
        g_pend.on = true;
        g_pend.slot = (uint8_t)slot;
        g_pend.tap = e.a0;
        g_pend.hold = e.a1;
        g_pend.t0 = t;
        g_pend.term = km_keymap_opts()->tap_term_ms;
        g_pend.policy = km_keymap_opts()->tap_policy;
        return;
    case KM_CODE_TOGGLE:
        note_press_for_osm();
        g_toggled ^= (uint8_t)(1u << e.a0);
        act_set(slot, ACT_INERT, 0, 0);
        return;
    case KM_CODE_ONESHOT_MOD:
        note_press_for_osm();
        act_set(slot, ACT_OSM, e.a0, 0);
        g_act[slot].t = t;
        return;
    case KM_CODE_TOGGLE_KEY:
        /* like a layer toggle key: another key was pressed, but it does not use up an armed one-shot modifier */
        note_press_for_osm();
        if (e.a0 >= 0x04u && e.a0 <= 0xE7u) {
            toggle_key(e.a0);
        }
        act_set(slot, ACT_INERT, 0, 0);
        return;
    case KM_CODE_MOD_MORPH:
        press_morph(slot, e);
        return;
    case KM_CODE_DKS:
    case KM_CODE_PAD_BTN:
    case KM_CODE_PAD_AXIS:
    case KM_CODE_LIGHT:
        /* M6 codes that other modules own (read through km_layers_locked_entry): the key is down and silent here */
        note_press_for_osm();
        act_set(slot, ACT_INERT, 0, 0);
        return;
    default:
        break;
    }
    note_press_for_osm();
    if (e.code >= 0x04u && e.code <= 0xE7u) {
        /* a plain modifier does not use up an armed one-shot modifier; any other key does */
        act_set(slot, ACT_KBD, e.code, km_usage_is_modifier(e.code) ? 0u : take_armed());
    } else if (hold_layer(e.code) != 0) {
        act_set(slot, ACT_HOLD, (uint8_t)hold_layer(e.code), 0);
    } else if (e.code >= KM_CODE_PROFILE_1 && e.code <= KM_CODE_PROFILE_4) {
        act_set(slot, ACT_PROFILE, e.code, 0);
    } else if (km_layers_consumer_usage(e.code) != 0) {
        act_set(slot, ACT_MEDIA, e.code, 0);
    } else {
        act_set(slot, ACT_INERT, 0, 0);              /* no key, 0xFD / 0xFE, 0xFF on layer 0 */
    }
}

static void handle_release(unsigned slot, uint32_t now)
{
    const act_t a = g_act[slot];
    if (a.kind == ACT_NONE) {
        return;
    }
    act_clear(slot);
    switch (a.kind) {
    case ACT_HOLD:
        if (g_holds[a.code] > 0) {
            g_holds[a.code]--;
        }
        break;
    case ACT_PROFILE:
        g_profile_req = (uint8_t)(a.code - KM_CODE_PROFILE_1 + 1u);
        break;
    case ACT_OSM:
        if (!a.used && (uint32_t)(now - a.t) < km_keymap_opts()->tap_term_ms) {   /* a tap: the modifier stays for the next key */
            g_os_armed |= (uint8_t)(1u << (a.code - KM_USAGE_MOD_FIRST));
            g_os_t = now;
        }
        break;
    default:
        break;
    }
}

static void drain(uint32_t now)
{
    for (unsigned guard = 0; guard < 4u * KM_LAYERS_EVENTS + 8u; guard++) {
        if (g_pulse.on && time_reached(now, g_pulse.until)) {
            g_pulse.on = false;
        }
        if (g_pend.on) {
            if (!pend_decide(now)) {
                break;
            }
            continue;
        }
        if (g_qn == 0 || stalled(now)) {
            break;
        }
        const ev_t e = g_q[0];
        queue_remove(0);
        if (e.down) {
            handle_press(e.slot, e.t);
        } else {
            handle_release(e.slot, now);
        }
        if (e.waited) {
            stall_for(now, KM_LAYERS_STEP_MS);
        }
    }
    for (unsigned i = 0; i < g_qn; i++) {
        g_q[i].waited = true;
    }
}

/* The queue is full (never in normal use): decide everything now, ignoring pacing. No event is dropped. */
static void flush_all(uint32_t now)
{
    for (unsigned guard = 0; guard < 4u * KM_LAYERS_EVENTS + 8u && (g_qn != 0 || g_pend.on); guard++) {
        g_stall_on = false;
        if (g_pend.on) {
            if (!pend_decide(now)) {
                pend_hold(now);
            }
            continue;
        }
        const ev_t e = g_q[0];
        queue_remove(0);
        if (e.down) {
            handle_press(e.slot, e.t);
        } else {
            handle_release(e.slot, now);
        }
    }
    g_stall_on = false;
}

static void enqueue(unsigned slot, bool down, uint32_t now)
{
    if (g_qn >= KM_LAYERS_EVENTS) {
        flush_all(now);
    }
    g_q[g_qn].slot = (uint8_t)slot;
    g_q[g_qn].down = down;
    g_q[g_qn].waited = false;
    g_q[g_qn].t = now;
    g_qn++;
}

/* Is an event of this slot still waiting in the queue? (A release must not overtake it, or the later press is left without one.) */
static bool queued_for(unsigned slot)
{
    for (unsigned i = 0; i < g_qn; i++) {
        if (g_q[i].slot == slot) {
            return true;
        }
    }
    return false;
}

/* ---- the update ---- */

void km_layers_update_ms(const uint8_t *down_slots, unsigned n, uint32_t now, km_layers_out_t *out)
{
    if (!g_ready) {
        km_layers_reset();
    }
    memset(out, 0, sizeof *out);
    g_now = now;
    if (g_gen != km_keymap_generation()) {           /* another table / profile: toggled layers and armed modifiers end */
        g_gen = km_keymap_generation();
        g_toggled = 0;
        g_tk_n = 0;
        g_os_armed = 0;
    }
    g_profile_req = 0;

    bool down[NSLOTS] = { false };
    for (unsigned i = 0; i < n; i++) {
        if (down_slots[i] < NSLOTS && km_keymap_slot_is_key(down_slots[i])) {
            down[down_slots[i]] = true;
        }
    }

    /* releases first; a key the resolver has already pressed is released at once, whatever is waiting in the queue */
    for (unsigned s = 0; s < NSLOTS; s++) {
        if (g_raw[s] && !down[s]) {
            g_raw[s] = false;
            if (g_act[s].kind != ACT_NONE && !queued_for(s)) {
                handle_release(s, now);
            } else {
                enqueue(s, false, now);
            }
        }
    }
    /* new presses in list (= press) order */
    for (unsigned i = 0; i < n; i++) {
        const unsigned s = down_slots[i];
        if (s < NSLOTS && down[s] && !g_raw[s]) {
            g_raw[s] = true;
            enqueue(s, true, now);
        }
    }

    if (g_os_armed != 0 && km_keymap_opts()->oneshot_ms != 0 && time_reached(now, g_os_t + km_keymap_opts()->oneshot_ms)) {
        g_os_armed = 0;
    }
    drain(now);

    /* outputs: handled keys in the order they were pressed, then the tap pulse and the one-shot modifiers */
    bool usage_seen[256] = { false };
    /* Mod-Morph suppression: a modifier a morph key has hidden stays hidden until that key is released; one that was let go
       since (no key sends it any more) is dropped from the key's mask, so pressing it again is seen */
    uint8_t sup = 0;
    if (g_sup_keys != 0) {
        const uint8_t raw = raw_mods();
        for (unsigned i = 0; i < g_order_n; i++) {
            act_t *a = &g_act[g_order[i]];
            a->suppress &= raw;
            sup |= a->suppress;
        }
    }
#define ADD_KBD(u)                                                              \
    do {                                                                        \
        if (!usage_seen[(u)] && out->kbd_n < KM_KEYMAP_SLOTS &&                 \
            ((u) < KM_USAGE_MOD_FIRST || (sup & (1u << ((u) - KM_USAGE_MOD_FIRST))) == 0u)) { \
            usage_seen[(u)] = true;                                             \
            out->kbd[out->kbd_n++] = (uint8_t)(u);                              \
        }                                                                       \
    } while (0)
    uint8_t mods = g_os_armed;
    for (unsigned i = 0; i < g_order_n; i++) {
        const act_t *a = &g_act[g_order[i]];
        mods |= a->carry;
        if (a->kind == ACT_KBD || a->kind == ACT_OSM) {
            ADD_KBD(a->code);
        } else if (a->kind == ACT_MEDIA) {
            out->consumer = km_layers_consumer_usage(a->code);  /* later in press order overwrites: the most recent media key wins */
        }
    }
    if (g_pulse.on) {
        mods |= g_pulse.mods;
        if (g_pulse.media) {
            out->consumer = km_layers_consumer_usage(g_pulse.code);
        } else {
            ADD_KBD(g_pulse.code);
        }
    }
    for (unsigned i = 0; i < g_tk_n; i++) {
        ADD_KBD(g_tk[i]);
    }
    for (unsigned b = 0; b < 8u; b++) {
        if ((mods & (1u << b)) != 0) {
            ADD_KBD(KM_USAGE_MOD_FIRST + b);
        }
    }
#undef ADD_KBD
    out->profile_request = g_profile_req;
    out->layer = (uint8_t)active_layer();
    out->toggled = g_toggled;
    out->toggle_keys = (uint8_t)g_tk_n;
    out->suppressed = sup;
    out->pending = g_pend.on;
    out->armed = g_os_armed;
}

bool km_layers_locked_entry(unsigned slot, km_layers_entry_t *out)
{
    if (!g_ready || slot >= NSLOTS || out == NULL || g_act[slot].kind == ACT_NONE) {
        return false;
    }
    *out = g_ent[slot];
    return true;
}

void km_layers_update(const uint8_t *down_slots, unsigned n, km_layers_out_t *out)
{
    km_layers_update_ms(down_slots, n, g_now, out);
}

void km_layers_update_cycles(const uint8_t *down_slots, unsigned n, uint32_t now_cycles, uint32_t cycles_per_ms,
                             km_layers_out_t *out)
{
    if (!g_clock_init) {
        g_clock_init = true;
        g_clock_prev = now_cycles;
    }
    if (cycles_per_ms != 0) {
        const uint32_t whole = (now_cycles - g_clock_prev) / cycles_per_ms;
        g_clock_ms += whole;
        g_clock_prev += whole * cycles_per_ms;
    }
    km_layers_update_ms(down_slots, n, g_clock_ms, out);
}
