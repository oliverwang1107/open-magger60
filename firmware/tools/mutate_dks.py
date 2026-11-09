#!/usr/bin/env python3
"""Mutation pass for the M6 P3 Dynamic Keystroke module (core/src/km_dks.c).

    python firmware/tools/mutate_dks.py            # run every mutation, list survivors
    python firmware/tools/mutate_dks.py --list     # just print them
    python firmware/tools/mutate_dks.py --only tap

Same idea as mutate_socd.py / mutate_engine.py: each mutation changes one expression in a scratch copy of the sources,
rebuilds tests/test_dks.c (with a tiny main, so it does not depend on the suite registration in test_main.c) and runs it.
"killed" = the build works and a check fails (or the program crashes / hangs); a "survivor" means the tests do not notice
the change and the test suite has a hole. Mutations that provably cannot change behaviour are listed in EQUIVALENT with
the reason.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FW = Path(__file__).resolve().parents[1]
SRC = ["core/src/km_dks.c", "core/src/km_keymap.c", "core/src/km_engine.c", "core/src/km_depth_model.c"]
F = "core/src/km_dks.c"

# (name, old text, new text); `old` must occur exactly once in km_dks.c.
MUTATIONS = [
    # ---- record validity ----
    ("valid: t_act minimum off by one", "r->t_act_mm100 < KM_DKS_ACT_MIN_MM100", "r->t_act_mm100 <= KM_DKS_ACT_MIN_MM100"),
    ("valid: t_act minimum too low", "r->t_act_mm100 < KM_DKS_ACT_MIN_MM100", "r->t_act_mm100 < KM_DKS_ACT_MIN_MM100 - 1u"),
    ("valid: flags up to 3", "r->flags > KM_DKS_FLAG_RETRIGGER", "r->flags > 3u"),
    ("valid: rsvd[0] not checked", "r->rsvd[0] != 0u", "0"),
    ("valid: rsvd[1] not checked", "r->rsvd[1] != 0u", "0"),
    ("valid: rsvd[2] not checked", "r->rsvd[2] != 0u", "0"),
    ("valid: t_bot may be one above the travel", "bot > KM_DKS_TRAVEL_MM100 ||", "bot > KM_DKS_TRAVEL_MM100 + 1u ||"),
    ("valid: gap of 40 not needed (> instead of >=)", "bot < (unsigned)r->t_act_mm100 + KM_DKS_GAP_MIN_MM100", "bot <= (unsigned)r->t_act_mm100 + KM_DKS_GAP_MIN_MM100"),
    ("valid: gap of 39 is enough", "bot < (unsigned)r->t_act_mm100 + KM_DKS_GAP_MIN_MM100", "bot + 1u < (unsigned)r->t_act_mm100 + KM_DKS_GAP_MIN_MM100"),
    ("valid: usage 3 accepted", "u < 0x04u ||", "u < 0x03u ||"),
    ("valid: usage 0xE8 accepted", "u > 0xE7u", "u > 0xE8u"),
    ("valid: usage 0 refused", "if (u != 0u && (u < 0x04u", "if ((u < 0x04u"),
    ("default bottom is 300", "(uint16_t)KM_DKS_BOT_DEFAULT_MM100;", "(uint16_t)300u;"),
    ("engine release point 5 below t_act", "(uint16_t)(r->t_act_mm100 - KM_DKS_RELEASE_BELOW_MM100)", "(uint16_t)(r->t_act_mm100 - 5u)"),
    ("engine press point one above t_act", "*press_mm100 = r->t_act_mm100;", "*press_mm100 = (uint16_t)(r->t_act_mm100 + 1u);"),
    ("engine points: NULL outputs accepted", "if (!km_dks_rec_valid(r) || press_mm100 == NULL || release_mm100 == NULL) {", "if (!km_dks_rec_valid(r)) {"),
    ("engine points: invalid record accepted", "if (!km_dks_rec_valid(r) || press_mm100 == NULL || release_mm100 == NULL) {", "if (press_mm100 == NULL || release_mm100 == NULL) {"),
    # ---- record sets ----
    ("set: nine records accepted", "n > KM_DKS_MAX) {", "n > KM_DKS_MAX + 1u) {"),
    ("set: NULL records with n > 0 accepted", "if (recs == NULL || n > KM_DKS_MAX) {", "if (n > KM_DKS_MAX) {"),
    ("set: a bad record does not reject the set", "if (!km_dks_rec_valid(&recs[i])) {", "if (0) {"),
    ("set: records never usable", "        d->ok[i] = 1u;", "        (void)0;"),
    ("set: record not copied", "        d->rec[i] = recs[i];", "        (void)0;"),
    ("set: the old running state survives a new set", "    memset(d->ev_mask, 0, sizeof d->ev_mask);\n    clear_keys(d);\n    if (n == 0u) {", "    memset(d->ev_mask, 0, sizeof d->ev_mask);\n    if (n == 0u) {"),
    ("mask: unused actions count", "if (recs[i].act[a].usage != 0u) {", "if (1) {"),
    ("mask: events with value 0 count", "!= KM_DKS_EV_NONE) {\n                        mask", "== KM_DKS_EV_NONE) {\n                        mask"),
    ("mask: wrong bit", "mask = (uint8_t)(mask | (1u << e));", "mask = (uint8_t)(mask | (1u << (e ^ 1u)));"),
    ("mask: all events configured", "        d->ev_mask[i] = mask;", "        d->ev_mask[i] = 0x0Fu;"),
    ("mask: no event configured", "        d->ev_mask[i] = mask;", "        d->ev_mask[i] = 0u;"),
    # ---- applying one event ----
    ("press: event bits read one place off", "switch ((r->act[a].ev >> (2u * (ev - 1u))) & 3u) {", "switch ((r->act[a].ev >> (2u * ev)) & 3u) {"),
    ("press: does not set the action down", "            k->held = (uint8_t)(k->held | bit);\n            k->tap = (uint8_t)(k->tap & ~bit);\n            k->usage[a] = r->act[a].usage;\n            break;\n        case KM_DKS_EV_RELEASE:", "            k->tap = (uint8_t)(k->tap & ~bit);\n            k->usage[a] = r->act[a].usage;\n            break;\n        case KM_DKS_EV_RELEASE:"),
    ("press: keeps a tap flag", "            k->held = (uint8_t)(k->held | bit);\n            k->tap = (uint8_t)(k->tap & ~bit);\n            k->usage[a] = r->act[a].usage;\n            break;\n        case KM_DKS_EV_RELEASE:", "            k->held = (uint8_t)(k->held | bit);\n            k->usage[a] = r->act[a].usage;\n            break;\n        case KM_DKS_EV_RELEASE:"),
    ("press: usage not recorded", "            k->tap = (uint8_t)(k->tap & ~bit);\n            k->usage[a] = r->act[a].usage;\n            break;\n        case KM_DKS_EV_RELEASE:", "            k->tap = (uint8_t)(k->tap & ~bit);\n            break;\n        case KM_DKS_EV_RELEASE:"),
    ("release: does not release", "        case KM_DKS_EV_RELEASE:\n            k->held = (uint8_t)(k->held & ~bit);\n", "        case KM_DKS_EV_RELEASE:\n"),
    ("release: keeps a tap flag", "            k->held = (uint8_t)(k->held & ~bit);\n            k->tap = (uint8_t)(k->tap & ~bit);\n            break;\n        case KM_DKS_EV_TAP:", "            k->held = (uint8_t)(k->held & ~bit);\n            break;\n        case KM_DKS_EV_TAP:"),
    ("tap: also shortens an action that is held", "            if ((k->held & bit) == 0u) {\n                k->held", "            if (1) {\n                k->held"),
    ("tap: not marked as a pulse", "                k->tap = (uint8_t)(k->tap | bit);", "                (void)0;"),
    ("tap: usage not recorded", "                k->usage[a] = r->act[a].usage;\n                k->tap_due[a] = now + KM_DKS_TAP_MS;", "                k->tap_due[a] = now + KM_DKS_TAP_MS;"),
    ("tap: pulse too short", "                k->usage[a] = r->act[a].usage;\n                k->tap_due[a] = now + KM_DKS_TAP_MS;", "                k->usage[a] = r->act[a].usage;\n                k->tap_due[a] = now + KM_DKS_TAP_MS - 1u;"),
    ("tap: pulse too long", "                k->usage[a] = r->act[a].usage;\n                k->tap_due[a] = now + KM_DKS_TAP_MS;", "                k->usage[a] = r->act[a].usage;\n                k->tap_due[a] = now + KM_DKS_TAP_MS + 1u;"),
    ("tap: a running pulse does not start over", "k->tap_due[a] = now + KM_DKS_TAP_MS;      /* a running pulse", "k->tap_due[a] = k->tap_due[a];      /* a running pulse"),
    ("event 4: leaves everything that is down", "k->held = (uint8_t)(k->held & k->tap);", "k->held = (uint8_t)(k->held | 0u);"),
    ("event 4: also cuts the tap pulses", "k->held = (uint8_t)(k->held & k->tap);", "k->held = 0u;"),
    ("event 4: wrong event number", "    if (ev == 4u) {\n        k->held", "    if (ev == 3u) {\n        k->held"),
    ("applied events not counted", "    d->events_applied++;\n    d->dirty = true;", "    d->dirty = true;"),
    ("output not marked for rebuild after an event", "    d->events_applied++;\n    d->dirty = true;", "    d->events_applied++;"),
    # ---- the queue ----
    ("queue: pop does not shift", "    memmove(&k->q[0], &k->q[1], k->q_n);", "    (void)0;"),
    ("queue: count not decreased", "    k->q_n--;\n", "    (void)0;\n"),
    ("queue: record taken from the wrong bits", "apply_event(d, k, e >> 2, (e & 3u) + 1u, now);", "apply_event(d, k, e & 7u, (e & 3u) + 1u, now);"),
    ("queue: event number off by one", "apply_event(d, k, e >> 2, (e & 3u) + 1u, now);", "apply_event(d, k, e >> 2, (e & 3u), now);"),
    ("queue: events without actions are queued", "if (ev != 4u && (d->ev_mask[k->rec] & (1u << (ev - 1u))) == 0u) {", "if (0) {"),
    ("queue: event 4 is skipped when it has no action", "if (ev != 4u && (d->ev_mask[k->rec] & (1u << (ev - 1u))) == 0u) {", "if ((d->ev_mask[k->rec] & (1u << (ev - 1u))) == 0u) {"),
    ("queue: mask bit one place off", "(d->ev_mask[k->rec] & (1u << (ev - 1u))) == 0u) {\n        return;", "(d->ev_mask[k->rec] & (1u << ev)) == 0u) {\n        return;"),
    ("queue: a full queue drops the new event", "    if (k->q_n >= KM_DKS_QUEUE) {                         /* never lose an event: apply what waits, at once, in order */\n        while (k->q_n != 0u) {\n            apply_queued(d, k, now);\n        }\n        d->queue_flushes++;\n    }", "    if (k->q_n >= KM_DKS_QUEUE) {\n        d->queue_flushes++;\n        return;\n    }"),
    ("queue: a full queue applies only one event", "        while (k->q_n != 0u) {\n            apply_queued(d, k, now);\n        }", "        if (k->q_n != 0u) {\n            apply_queued(d, k, now);\n        }"),
    ("queue: flushes not counted", "        d->queue_flushes++;\n    }\n    k->q[k->q_n++]", "    }\n    k->q[k->q_n++]"),
    ("queue: the record is not stored with the event", "k->q[k->q_n++] = (uint8_t)((k->rec << 2) | (ev - 1u));", "k->q[k->q_n++] = (uint8_t)(ev - 1u);"),
    ("queue: event number stored off by one", "k->q[k->q_n++] = (uint8_t)((k->rec << 2) | (ev - 1u));", "k->q[k->q_n++] = (uint8_t)((k->rec << 2) | ev);"),
    # ---- closing a press ----
    ("close: no event 3 for a key that is still bottomed", "    if ((k->phase & PH_BOTTOM) != 0u) {\n        enqueue(d, k, 3u, now);\n    }\n    enqueue(d, k, 4u, now);", "    enqueue(d, k, 4u, now);"),
    ("close: no event 4", "    enqueue(d, k, 4u, now);\n    k->phase = 0;", "    k->phase = 0;"),
    ("close: event 4 before event 3", "    if ((k->phase & PH_BOTTOM) != 0u) {\n        enqueue(d, k, 3u, now);\n    }\n    enqueue(d, k, 4u, now);", "    enqueue(d, k, 4u, now);\n    if ((k->phase & PH_BOTTOM) != 0u) {\n        enqueue(d, k, 3u, now);\n    }"),
    ("close: the press stays open", "    enqueue(d, k, 4u, now);\n    k->phase = 0;", "    enqueue(d, k, 4u, now);\n    k->phase = PH_OPEN;"),
    ("close: the bottom flag stays", "    enqueue(d, k, 4u, now);\n    k->phase = 0;", "    enqueue(d, k, 4u, now);\n    k->phase = (uint8_t)(k->phase & PH_DONE);"),
    # ---- the key table ----
    ("table: slot map holds the index itself", "d->where[slot] = (uint8_t)(i + 1u);", "d->where[slot] = (uint8_t)i;"),
    ("table: entries not counted", "            d->n_tracked++;\n            return &d->key[i];", "            return &d->key[i];"),
    ("table: freed entry stays in the slot map", "    d->where[k->slot] = 0;\n    k->slot = FREE_SLOT;", "    k->slot = FREE_SLOT;"),
    ("table: freed entry not counted", "    d->n_tracked--;\n", "    (void)0;\n"),
    ("table: slot not stored in a new entry", "            k->slot = (uint8_t)slot;\n", "            (void)0;\n"),
    # ---- the output ----
    ("output: no duplicate removal", "if (((seen_usage[u >> 5] >> (u & 31u)) & 1u) == 0u) {", "if (1) {"),
    ("output: duplicate test on the wrong bit", "if (((seen_usage[u >> 5] >> (u & 31u)) & 1u) == 0u) {", "if (((seen_usage[u >> 5] >> (u & 15u)) & 1u) == 0u) {"),
    ("output: usages of released actions", "            if ((k->held & (1u << a)) != 0u) {\n                const unsigned u = k->usage[a];", "            if (1) {\n                const unsigned u = k->usage[a];"),
    ("output: free entries are read", "if (k->slot == FREE_SLOT || k->held == 0u) {\n            continue;", "if (k->held == 0u) {\n            continue;"),
    ("output: count not stored", "    d->cur.n = (uint8_t)cnt;", "    (void)0;"),
    ("output: idle path does not rebuild", "        if (d->dirty) {\n            build_out(d);\n        }\n        *out = d->cur;\n        return out->n;\n    }\n    for", "        *out = d->cur;\n        return out->n;\n    }\n    for"),
    ("output: not rebuilt after the pass", "    if (d->dirty) {\n        build_out(d);\n    }\n    *out = d->cur;\n    return out->n;\n}\n\nunsigned km_dks_update_cycles", "    *out = d->cur;\n    return out->n;\n}\n\nunsigned km_dks_update_cycles"),
    # ---- one pass ----
    ("pass: idle path taken with keys still to finish", "if (n == 0u && d->n_tracked == 0u) {", "if (n == 0u) {"),
    ("pass: idle path taken whenever the table is empty", "if (n == 0u && d->n_tracked == 0u) {", "if (d->n_tracked == 0u) {"),
    ("pass: 'seen' marks never cleared", "    for (unsigned i = 0; i < KM_DKS_KEYS; i++) {\n        d->key[i].seen = 0;\n    }", "    (void)0;"),
    ("pass: slot 76 accepted", "if (slot >= KM_DKS_SLOTS || rec >= KM_DKS_MAX || d->ok[rec] == 0u) {", "if (slot > KM_DKS_SLOTS || rec >= KM_DKS_MAX || d->ok[rec] == 0u) {"),
    ("pass: record 8 accepted", "if (slot >= KM_DKS_SLOTS || rec >= KM_DKS_MAX || d->ok[rec] == 0u) {", "if (slot >= KM_DKS_SLOTS || rec > KM_DKS_MAX || d->ok[rec] == 0u) {"),
    ("pass: unusable record accepted", "if (slot >= KM_DKS_SLOTS || rec >= KM_DKS_MAX || d->ok[rec] == 0u) {", "if (slot >= KM_DKS_SLOTS || rec >= KM_DKS_MAX) {"),
    ("pass: overflow not counted", "                d->overflow_presses++;\n", "                (void)0;\n"),
    ("pass: new key not ready at once", "            k->rec = (uint8_t)rec;\n            k->next_ms = now;\n", "            k->rec = (uint8_t)rec;\n            k->next_ms = now + KM_DKS_STEP_MS;\n"),
    ("pass: new key keeps a stale clock", "            k->rec = (uint8_t)rec;\n            k->next_ms = now;\n", "            k->rec = (uint8_t)rec;\n"),
    ("pass: record of a new key not stored", "            k->rec = (uint8_t)rec;\n            k->next_ms = now;", "            k->next_ms = now;"),
    ("pass: a record change does not close the old press", "            if ((k->phase & PH_OPEN) != 0u) {\n                close_press(d, k, now);                   /* released and pressed again with another record */\n            }\n", ""),
    ("pass: a record change keeps the old record", "            k->rec = (uint8_t)rec;\n        }\n        k->seen = 1u;", "        }\n        k->seen = 1u;"),
    ("pass: a listed key is not marked", "        k->seen = 1u;\n    }", "    }"),
    ("generate: press never opens", "                k->phase = PH_OPEN;\n                enqueue(d, k, 1u, now);", "                enqueue(d, k, 1u, now);"),
    ("generate: wrong event for a press", "                enqueue(d, k, 1u, now);", "                enqueue(d, k, 2u, now);"),
    ("generate: depth read without a callback", "if (depth != NULL && (d->ev_mask[k->rec] & EV_MASK_BOTTOM) != 0u) {", "if ((d->ev_mask[k->rec] & EV_MASK_BOTTOM) != 0u) {"),
    ("generate: depth callback gets the wrong slot", "const unsigned dp = depth(ctx, k->slot);", "const unsigned dp = depth(ctx, k->rec);"),
    ("generate: bottom point one above", "if (dp >= bot && ((k->phase", "if (dp > bot && ((k->phase"),
    ("generate: bottom point one below", "if (dp >= bot && ((k->phase", "if (dp + 1u >= bot && ((k->phase"),
    ("generate: bottom always retriggers", "((k->phase & PH_DONE) == 0u || (d->rec[k->rec].flags", "(1 || (d->rec[k->rec].flags"),
    ("generate: the retrigger flag is ignored", "(k->phase & PH_DONE) == 0u || (d->rec[k->rec].flags & KM_DKS_FLAG_RETRIGGER) != 0u)", "(k->phase & PH_DONE) == 0u)"),
    ("generate: 'bottom used' not remembered", "k->phase = (uint8_t)(k->phase | PH_BOTTOM | PH_DONE);", "k->phase = (uint8_t)(k->phase | PH_BOTTOM);"),
    ("generate: bottomed state not remembered", "k->phase = (uint8_t)(k->phase | PH_BOTTOM | PH_DONE);", "k->phase = (uint8_t)(k->phase | PH_DONE);"),
    ("generate: wrong event at the bottom", "                        enqueue(d, k, 2u, now);", "                        enqueue(d, k, 3u, now);"),
    ("generate: hysteresis of 0.10 mm", "dp + KM_DKS_HYST_MM100 <= bot", "dp + 10u <= bot"),
    ("generate: hysteresis of 0.30 mm", "dp + KM_DKS_HYST_MM100 <= bot", "dp + 30u <= bot"),
    ("generate: back up exactly at the hysteresis point does not count", "dp + KM_DKS_HYST_MM100 <= bot", "dp + KM_DKS_HYST_MM100 < bot"),
    ("generate: an unknown depth (0) counts as shallow", "} else if (dp != 0u && dp + KM_DKS_HYST_MM100 <= bot) {", "} else if (dp + KM_DKS_HYST_MM100 <= bot) {"),
    ("generate: bottom flag not cleared on the way up", "                    k->phase = (uint8_t)(k->phase & ~PH_BOTTOM);\n                    enqueue(d, k, 3u, now);", "                    enqueue(d, k, 3u, now);"),
    ("generate: wrong event on the way up", "                    enqueue(d, k, 3u, now);\n                }\n            }\n        } else if", "                    enqueue(d, k, 2u, now);\n                }\n            }\n        } else if"),
    ("generate: a released key does not close", "        } else if ((k->phase & PH_OPEN) != 0u) {\n            close_press(d, k, now);\n        }", "        }"),
    ("clock: a stale key clock is not refreshed", "(uint32_t)(now - k->next_ms) > 0x40000000u", "(uint32_t)(now - k->next_ms) > 0xFFFFFFFFu"),
    ("taps: never expire", "        if (k->tap != 0u) {\n            for", "        if (0) {\n            for"),
    ("taps: expire one millisecond late", "is_before(k->tap_due[a], now)) {\n                    k->held", "is_before(k->tap_due[a] + 1u, now)) {\n                    k->held"),
    ("taps: expire when still running", "is_before(k->tap_due[a], now)) {\n                    k->held", "1) {\n                    k->held"),
    ("taps: expiry does not clear the pulse flag", "                    k->tap = (uint8_t)(k->tap & ~(1u << a));\n                    d->dirty = true;", "                    d->dirty = true;"),
    ("taps: expiry does not release", "                    k->held = (uint8_t)(k->held & ~(1u << a));\n                    k->tap = (uint8_t)(k->tap & ~(1u << a));\n                    d->dirty = true;", "                    k->tap = (uint8_t)(k->tap & ~(1u << a));\n                    d->dirty = true;"),
    ("taps: output not rebuilt after an expiry", "                    k->tap = (uint8_t)(k->tap & ~(1u << a));\n                    d->dirty = true;", "                    k->tap = (uint8_t)(k->tap & ~(1u << a));"),
    ("apply: no spacing between events", "if (k->q_n != 0u && is_before(k->next_ms, now)) {", "if (k->q_n != 0u) {"),
    ("apply: waits one millisecond too long", "if (k->q_n != 0u && is_before(k->next_ms, now)) {", "if (k->q_n != 0u && is_before(k->next_ms + 1u, now)) {"),
    ("apply: never waits for the time", "if (k->q_n != 0u && is_before(k->next_ms, now)) {", "if (k->q_n != 0u && is_before(now, k->next_ms)) {"),
    ("apply: spacing too short", "k->next_ms = now + KM_DKS_STEP_MS;", "k->next_ms = now + KM_DKS_STEP_MS - 1u;"),
    ("apply: spacing not started", "k->next_ms = now + KM_DKS_STEP_MS;", "k->next_ms = now;"),
    ("free: entry with queued events is freed", "k->phase == 0u && k->q_n == 0u && k->held == 0u) {\n            free_key", "k->phase == 0u && k->held == 0u) {\n            free_key"),
    ("free: entry with actions down is freed", "k->phase == 0u && k->q_n == 0u && k->held == 0u) {\n            free_key", "k->phase == 0u && k->q_n == 0u) {\n            free_key"),
    ("free: entries are never freed", "            free_key(d, k);", "            (void)0;"),
    ("free: an open press is freed", "if (k->seen == 0u && k->phase == 0u && k->q_n == 0u", "if (k->seen == 0u && k->q_n == 0u"),
    # ---- reset, clock, merge ----
    ("reset: resets not counted", "    d->resets++;", "    (void)0;"),
    ("reset: keys survive", "void km_dks_reset(km_dks_t *d)\n{\n    clear_keys(d);", "void km_dks_reset(km_dks_t *d)\n{"),
    ("clear: output not marked for rebuild", "    d->dirty = true;                                      /* the output must be rebuilt (and is empty) */", "    (void)0;"),
    ("clear: slot map kept", "    memset(d->where, 0, sizeof d->where);\n    d->n_tracked = 0;", "    d->n_tracked = 0;"),
    ("clear: count kept", "    memset(d->where, 0, sizeof d->where);\n    d->n_tracked = 0;", "    memset(d->where, 0, sizeof d->where);"),
    ("cycles: time never advances", "        d->ms += whole;\n", "        (void)0;\n"),
    ("cycles: remainder not kept", "        d->clk_prev += whole * cycles_per_ms;\n", "        (void)0;\n"),
    ("cycles: no clock does not advance", "        d->ms += KM_DKS_STEP_MS;                          /* no clock", "        d->ms += 0u;                          /* no clock"),
    ("merge: duplicates added", "if (!have && n_kbd < cap) {", "if (n_kbd < cap) {"),
    ("merge: capacity ignored", "if (!have && n_kbd < cap) {", "if (!have) {"),
    ("merge: nothing added", "if (!have && n_kbd < cap) {", "if (!have && n_kbd < cap && 0) {"),
    ("busy: reversed", "if (d->key[i].slot != FREE_SLOT) {\n            return true;", "if (d->key[i].slot == FREE_SLOT) {\n            return true;"),
]

# Mutations that cannot change behaviour, with the reason.
EQUIVALENT = {
    "set: NULL records with n > 0 accepted": "km_dks_rec_valid(NULL) is false, so the loop refuses a NULL array with n > 0 anyway (belt and braces)",
    "release: keeps a tap flag": "a stale pulse flag only matters while the action is down, and every way of putting it down again (press, tap) sets or clears the flag itself",
    "taps: expiry does not clear the pulse flag": "same: the flag of an action that is up is never looked at before the next press or tap rewrites it",
    "free: an open press is freed": "a key that is not listed has just been closed (phase 0) in the same pass, so the phase test is redundant with the seen test",
    "output: free entries are read": "a free entry never has an action down (every free path requires held == 0), so reading it adds nothing",
}


def env() -> dict:
    e = dict(os.environ)
    ucrt = Path("C:/msys64/ucrt64/bin")
    if os.name == "nt" and ucrt.exists():
        e["PATH"] = str(ucrt) + os.pathsep + e["PATH"]
    return e


STUB = r'''
#include <stdio.h>
#include "km_test.h"
int km_test_checks, km_test_failures;
size_t km_hex(const char *hex, uint8_t *out, size_t cap) { (void)hex; (void)out; (void)cap; return 0; }
void test_dks(void);
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_dks();
    printf("%d checks, %d failures\n", km_test_checks, km_test_failures);
    return km_test_failures == 0 ? 0 : 1;
}
'''


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--only", help="run only mutations whose name contains this text")
    args = ap.parse_args()
    if args.list:
        for i, m in enumerate(MUTATIONS, 1):
            print("%3d  %s" % (i, m[0]))
        return 0

    e = env()
    cc = shutil.which("gcc", path=e["PATH"])
    work = Path(tempfile.mkdtemp(prefix="km_mut_dks_"))
    (work / "main.c").write_text(STUB)
    survivors, broken = [], []
    try:
        for i, (name, old, new) in enumerate(MUTATIONS, 1):
            if args.only and args.only not in name:
                continue
            tree = work / ("m%d" % i)
            shutil.copytree(FW / "core", tree / "core")
            shutil.copytree(FW / "tests", tree / "tests", ignore=shutil.ignore_patterns("golden"))
            target = tree / F
            text = target.read_text(newline="").replace("\r\n", "\n")
            if text.count(old) != 1:
                print("%3d  %-62s BAD MUTATION (pattern found %d times)" % (i, name, text.count(old)))
                broken.append(name)
                continue
            target.write_text(text.replace(old, new), newline="")
            exe = tree / "t.exe"
            cmd = [cc, "-std=gnu11", "-O1", "-w", "-I" + str(tree / "core/include"), "-I" + str(tree / "tests"),
                   str(work / "main.c"), str(tree / "tests/test_dks.c"),
                   *[str(tree / s) for s in SRC], *[str(p) for p in sorted((tree / "core/gen").glob("*.c"))], "-o", str(exe)]
            r = subprocess.run(cmd, env=e, capture_output=True, text=True)
            if r.returncode != 0:
                print("%3d  %-62s BUILD FAILED" % (i, name))
                print(r.stderr[:300])
                broken.append(name)
                continue
            try:
                r = subprocess.run([str(exe)], env=e, capture_output=True, text=True, timeout=120)
                rc, out = r.returncode, r.stdout
            except subprocess.TimeoutExpired:
                rc, out = 1, "(timeout)"
            if rc == 0 and EQUIVALENT.get(name):
                print("%3d  %-62s equivalent (%s)" % (i, name, EQUIVALENT[name]))
            elif rc == 0:
                print("%3d  %-62s SURVIVED" % (i, name))
                survivors.append(name)
            else:
                first = next((l.strip() for l in out.splitlines() if "FAIL" in l), "(crash/timeout)")
                print("%3d  %-62s killed  %s" % (i, name, first[-60:]))
            shutil.rmtree(tree, ignore_errors=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("\n%d mutations, %d survivors, %d broken" % (len(MUTATIONS), len(survivors), len(broken)))
    for s in survivors:
        print("  survivor:", s)
    return 1 if survivors or broken else 0


if __name__ == "__main__":
    sys.exit(main())
