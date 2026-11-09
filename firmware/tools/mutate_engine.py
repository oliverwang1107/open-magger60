#!/usr/bin/env python3
"""Mutation pass for the M4 T-B code (km_engine, km_keymap, km_report), including the M5 T-A rapid trigger.

    python firmware/tools/mutate_engine.py            # run every mutation, list survivors
    python firmware/tools/mutate_engine.py --list     # just print them

Each mutation changes one expression in a scratch copy of the sources, rebuilds only the engine test
(tests/test_engine.c with a tiny main) and runs it. A mutation is "killed" when the build still works but
the test fails; a "survivor" means the tests do not notice the change and need another check.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FW = Path(__file__).resolve().parents[1]
SRC = ["core/src/km_engine.c", "core/src/km_keymap.c", "core/src/km_report.c", "core/src/km_depth_model.c"]
HDR = ["core/include/km/km_engine.h", "core/include/km/km_keymap.h", "core/include/km/km_report.h"]

# (name, file, old text, new text); `old` must occur exactly once in the file.
MUTATIONS = [
    ("press compare < instead of <=", "core/src/km_engine.c",
     "if (f <= (int32_t)k->press_q4) {", "if (f < (int32_t)k->press_q4) {"),
    ("press debounce one frame longer", "core/src/km_engine.c",
     "            if (++k->cnt >= KM_ENGINE_PRESS_FRAMES) {", "            if (++k->cnt > KM_ENGINE_PRESS_FRAMES) {"),
    ("release debounce one frame shorter", "core/src/km_engine.c",
     "    } else if (f >= (int32_t)k->release_q4) {\n        if (++k->cnt >= KM_ENGINE_RELEASE_FRAMES) {",
     "    } else if (f >= (int32_t)k->release_q4) {\n        if (++k->cnt >= KM_ENGINE_RELEASE_FRAMES - 1u) {"),
    ("release compare > instead of >=", "core/src/km_engine.c",
     "} else if (f >= (int32_t)k->release_q4) {", "} else if (f > (int32_t)k->release_q4) {"),
    ("no hysteresis (release at press point)", "core/src/km_engine.c",
     "int32_t release = (er + 1) * 16; ", "int32_t release = (ep + 1) * 16; "),
    ("press debounce counter not reset", "core/src/km_engine.c",
     "        } else {\n            k->cnt = 0;\n            if (drift_tick) {", "        } else {\n            if (drift_tick) {"),
    ("glitch limit off by one", "core/src/km_engine.c",
     "if (nn >= KM_ENGINE_GLITCH_ONSET) {", "if (nn > KM_ENGINE_GLITCH_ONSET) {"),
    ("glitch guard removed", "core/src/km_engine.c",
     "if (nn >= KM_ENGINE_GLITCH_ONSET) {", "if (nn >= 1000u) {"),
    ("drift window ignored", "core/src/km_engine.c",
     "if (d < -win || d > win) {", "if (0) {"),
    ("drift also pulls the guessed baseline limit", "core/src/km_engine.c",
     "const int32_t limit = (int32_t)(assumed ? ASSUMED_LIMIT : KM_ENGINE_DRIFT_LIMIT) << 16;",
     "const int32_t limit = (int32_t)(ASSUMED_LIMIT) << 16;"),
    ("bottom learning off", "core/src/km_engine.c",
     "    } else {\n        k->cnt = 0;\n        if (f < (int32_t)k->learn_q4) {", "    } else {\n        k->cnt = 0;\n        if (0) {"),
    ("pressed list: no shift on removal", "core/src/km_engine.c",
     "memmove(&e->down[i], &e->down[i + 1], (size_t)(e->n_down - i - 1u));", "(void)0;"),
    ("force_up leaves the key in the list", "core/src/km_engine.c",
     "        k->flags = (uint8_t)(k->flags & ~F_DOWN);\n        down_remove(e, slot);\n    }\n    k->cnt = 0;",
     "        k->flags = (uint8_t)(k->flags & ~F_DOWN);\n    }\n    k->cnt = 0;"),
    ("new presses not appended to the list", "core/src/km_engine.c",
     "e->down[e->n_down++] = newly[i];", "(void)newly[i];"),
    ("fault level lowered", "core/include/km/km_engine.h",
     "#define KM_ENGINE_FAULT_LOW      1500u", "#define KM_ENGINE_FAULT_LOW      1000u"),
    ("faulted slot re-armed too early", "core/src/km_engine.c",
     "    if (f < (int32_t)k->release_q4) {\n        return true;\n    }\n    k->flags = (uint8_t)(k->flags & ~F_FAULT);",
     "    k->flags = (uint8_t)(k->flags & ~F_FAULT);"),
    ("input not clamped to 12 bit", "core/src/km_engine.c",
     "        if (v > 4095u) {\n            v = 4095u;\n        }\n        int32_t f = k->filt_q4;",
     "        int32_t f = k->filt_q4;"),
    ("filter too slow (shift 3)", "core/include/km/km_engine.h",
     "#define KM_ENGINE_FILTER_SHIFT   2u", "#define KM_ENGINE_FILTER_SHIFT   3u"),
    ("no filter (shift 0)", "core/include/km/km_engine.h",
     "#define KM_ENGINE_FILTER_SHIFT   2u", "#define KM_ENGINE_FILTER_SHIFT   0u"),
    ("held-key limit off by one", "core/src/km_engine.c",
     "if (held > KM_ENGINE_CAL_MAX_BAD) {", "if (held >= KM_ENGINE_CAL_MAX_BAD) {"),
    ("moving-key limit off by one", "core/src/km_engine.c",
     "if (moving > KM_ENGINE_CAL_MAX_BAD) {", "if (moving >= KM_ENGINE_CAL_MAX_BAD) {"),
    ("median replaced by the minimum", "core/src/km_engine.c",
     "const uint16_t med = sorted[n_ok / 2u];", "const uint16_t med = sorted[0];"),
    ("floating inputs accepted at calibration", "core/src/km_engine.c",
     "mean[s] >= KM_ENGINE_FAULT_LOW && mean[s] <= KM_ENGINE_VALUE_MAX;\n        if (!usable) {\n            k->flags |= F_BADIDLE;",
     "mean[s] <= KM_ENGINE_VALUE_MAX;\n        if (!usable) {\n            k->flags |= F_BADIDLE;"),
    ("railed inputs accepted at calibration", "core/src/km_engine.c",
     "mean[s] >= KM_ENGINE_FAULT_LOW && mean[s] <= KM_ENGINE_VALUE_MAX;\n        if (!usable) {\n            k->flags |= F_BADIDLE;",
     "mean[s] >= KM_ENGINE_FAULT_LOW;\n        if (!usable) {\n            k->flags |= F_BADIDLE;"),
    ("assumed baseline not marked", "core/src/km_engine.c",
     "        if (bad) {\n            k->flags |= F_ASSUMED;\n        }", "        (void)bad;"),
    ("threshold recompute never runs", "core/src/km_engine.c",
     "    if (e->dirty) {\n        refresh_one(e);\n    }", "    (void)refresh_one;"),
    ("non-key slots evaluated", "core/src/km_engine.c",
     "        if (km_keymap_slot_is_key(s)) {\n            e->slot[s].flags = F_PRESENT;", "        if (1) {\n            e->slot[s].flags = F_PRESENT;"),
    ("disable does not force the key up", "core/src/km_engine.c",
     "    if (!enabled) {\n        force_up(e, slot);", "    if (!enabled) {"),
    ("edge search compares > instead of >=", "core/src/km_engine.c",
     "if (km_depth_model_mm100(cal, (uint16_t)mid) >= depth_mm100) {", "if (km_depth_model_mm100(cal, (uint16_t)mid) > depth_mm100) {"),
    ("baseline not rounded", "core/src/km_engine.c",
     "return (uint16_t)((k->base_q16 + 0x8000u) >> 16);", "return (uint16_t)(k->base_q16 >> 16);"),
    ("config accepts release >= press", "core/src/km_engine.c",
     "cfg->release_mm100 < cfg->press_mm100 &&", "cfg->release_mm100 <= cfg->press_mm100 &&"),
    ("config accepts travel + 1", "core/src/km_engine.c",
     "cfg->press_mm100 <= KM_TRAVEL_MM100;", "cfg->press_mm100 <= KM_TRAVEL_MM100 + 1u;"),
    # rapid trigger (M5 T-A)
    ('rt cfg: distance floor excludes the floor itself', 'core/src/km_engine.c',
     'return v >= KM_ENGINE_RT_MIN_MM100 && v <= KM_TRAVEL_MM100;', 'return v > KM_ENGINE_RT_MIN_MM100 && v <= KM_TRAVEL_MM100;'),
    ('rt cfg: distance floor one lower', 'core/src/km_engine.c',
     'return v >= KM_ENGINE_RT_MIN_MM100 && v <= KM_TRAVEL_MM100;', 'return v >= KM_ENGINE_RT_MIN_MM100 - 1u && v <= KM_TRAVEL_MM100;'),
    ('rt cfg: floor lowered to 10', 'core/include/km/km_engine.h',
     '#define KM_ENGINE_RT_MIN_MM100 20u', '#define KM_ENGINE_RT_MIN_MM100 10u'),
    ('rt cfg: travel not allowed as a distance', 'core/src/km_engine.c',
     'return v >= KM_ENGINE_RT_MIN_MM100 && v <= KM_TRAVEL_MM100;', 'return v >= KM_ENGINE_RT_MIN_MM100 && v < KM_TRAVEL_MM100;'),
    ('rt cfg: no upper limit on distances', 'core/src/km_engine.c',
     'return v >= KM_ENGINE_RT_MIN_MM100 && v <= KM_TRAVEL_MM100;', 'return v >= KM_ENGINE_RT_MIN_MM100;'),
    ('rt cfg: dead zone 0 not allowed', 'core/src/km_engine.c',
     'return v == 0u || rt_dist_ok(v);', 'return rt_dist_ok(v);'),
    ('rt cfg: dead zone below the floor allowed', 'core/src/km_engine.c',
     'return v == 0u || rt_dist_ok(v);', 'return v <= KM_TRAVEL_MM100;'),
    ('rt cfg: continuous accepts any value', 'core/src/km_engine.c',
     'cfg->continuous > 1u) {', 'cfg->continuous > 254u) {'),
    ('rt cfg: actuation point not checked', 'core/src/km_engine.c',
     'if (cfg->continuous == 0u && !rt_dist_ok(cfg->press_mm100)) {', 'if (0) {'),
    ('rt cfg: actuation point checked even when continuous', 'core/src/km_engine.c',
     'if (cfg->continuous == 0u && !rt_dist_ok(cfg->press_mm100)) {', 'if (!rt_dist_ok(cfg->press_mm100)) {'),
    ('rt cfg: unreachable press accepted', 'core/src/km_engine.c',
     'if (cfg->rt_down_mm100 > KM_TRAVEL_MM100 - cfg->bottom_dead_mm100) {', 'if (0) {'),
    ('rt cfg: reachability off by one', 'core/src/km_engine.c',
     'if (cfg->rt_down_mm100 > KM_TRAVEL_MM100 - cfg->bottom_dead_mm100) {', 'if (cfg->rt_down_mm100 >= KM_TRAVEL_MM100 - cfg->bottom_dead_mm100) {'),
    ('rt cfg: zones may overlap', 'core/src/km_engine.c',
     'top_dead_eff(cfg) + cfg->bottom_dead_mm100 + KM_ENGINE_RT_MIN_MM100 <= KM_TRAVEL_MM100;', 'top_dead_eff(cfg) + cfg->bottom_dead_mm100 <= KM_TRAVEL_MM100;'),
    ('rt cfg: zone sum off by one', 'core/src/km_engine.c',
     'top_dead_eff(cfg) + cfg->bottom_dead_mm100 + KM_ENGINE_RT_MIN_MM100 <= KM_TRAVEL_MM100;', 'top_dead_eff(cfg) + cfg->bottom_dead_mm100 + KM_ENGINE_RT_MIN_MM100 < KM_TRAVEL_MM100;'),
    ('rt cfg: top zone 0 counts as 0', 'core/src/km_engine.c',
     'return c->top_dead_mm100 != 0u ? c->top_dead_mm100 : (uint16_t)KM_ENGINE_RT_MIN_MM100;', 'return c->top_dead_mm100;'),
    ('rt cfg: unknown mode accepted as rapid', 'core/src/km_engine.c',
     'if (cfg->mode != KM_ENGINE_TRIG_RAPID) {\n        return false;\n    }', 'if (0) {\n        return false;\n    }'),
    ('rt apply: invalid config leaves the old one', 'core/src/km_engine.c',
     '    apply_cfg(e, &e->slot[slot], &def);\n    return false;', '    return false;'),
    ('rt apply: invalid config reported as applied', 'core/src/km_engine.c',
     '    apply_cfg(e, &e->slot[slot], &def);\n    return false;', '    apply_cfg(e, &e->slot[slot], &def);\n    return true;'),
    ('rt: corrupt config used by recompute', 'core/src/km_engine.c',
     '    if (!km_engine_cfg_valid(&k->cfg)) {\n        km_engine_key_cfg_default(&k->cfg);     /* never run on a corrupt config */\n    }', ''),
    ('rt: new config not applied at once', 'core/src/km_engine.c',
     '        recompute(k);\n        k->g_q4 = k->filt_q4;', '        k->g_q4 = k->filt_q4;'),
    ('rt: new config leaves the old extreme', 'core/src/km_engine.c',
     '    k->cfg = *cfg;\n    k->ext = EXT_NONE;', '    k->cfg = *cfg;'),
    ('rt: rapid never dispatched', 'core/src/km_engine.c',
     'if (k->cfg.mode == KM_ENGINE_TRIG_RAPID) {\n            trigger_rapid', 'if (0) {\n            trigger_rapid'),
    ('rt: rest does not forget the extreme', 'core/src/km_engine.c',
     '        k->cnt = 0;\n        k->ext = k->thr_base;\n        if (drift_tick) {\n            drift_step(e, k, f);', '        k->cnt = 0;\n        if (drift_tick) {\n            drift_step(e, k, f);'),
    ('rt: rest does not reset the press counter', 'core/src/km_engine.c',
     '        k->cnt = 0;\n        k->ext = k->thr_base;\n        if (drift_tick) {\n            drift_step(e, k, f);', '        k->ext = k->thr_base;\n        if (drift_tick) {\n            drift_step(e, k, f);'),
    ('rt: drift not tracked at rest', 'core/src/km_engine.c',
     '        if (drift_tick) {\n            drift_step(e, k, f);\n        }\n        return;', '        return;'),
('rt: slow stage does not follow at rest', 'core/src/km_engine.c',
     '        k->g_q4 = (uint16_t)f;                  /* the slow stage follows the fast one at rest */\n', ''),
('rt: a pressed key is treated as at rest', 'core/src/km_engine.c',
     'if (f > (int32_t)k->rest_q4 && !(k->flags & F_DOWN)) {', 'if (f > (int32_t)k->rest_q4) {'),
    ('rt: general rest branch does not forget the extreme', 'core/src/km_engine.c',
     '        k->ext = k->thr_base;                   /* at rest: the top of the travel */', '        (void)0;'),
    ('rt: second stage too fast (shift 2)', 'core/include/km/km_engine.h',
     '#define KM_ENGINE_RT_FILTER_SHIFT 3u', '#define KM_ENGINE_RT_FILTER_SHIFT 2u'),
    ('rt: second stage too slow (shift 5)', 'core/include/km/km_engine.h',
     '#define KM_ENGINE_RT_FILTER_SHIFT 3u', '#define KM_ENGINE_RT_FILTER_SHIFT 5u'),
    ('rt: second stage always slow (no snap at rest)', 'core/src/km_engine.c',
     '    if (f <= (int32_t)k->rest_q4) {\n        g = k->g_q4;', '    if (1) {\n        g = k->g_q4;'),
    ('rt: second stage never slow', 'core/src/km_engine.c',
     '    if (f <= (int32_t)k->rest_q4) {\n        g = k->g_q4;', '    if (0) {\n        g = k->g_q4;'),
    ('rt: press ignores the direction guard', 'core/src/km_engine.c',
     'if (g <= (int32_t)k->press_q4 && f <= g + RT_DIR_TOL_Q4 &&', 'if (g <= (int32_t)k->press_q4 &&'),
    ('rt: release ignores the direction guard', 'core/src/km_engine.c',
     'const bool rising = f >= g - RT_DIR_TOL_Q4;', 'const bool rising = true;'),
    ('rt: direction tolerance 0', 'core/src/km_engine.c',
     '#define RT_DIR_TOL_Q4   (8 << 4)', '#define RT_DIR_TOL_Q4   0'),
    ('rt: press inside the top zone', 'core/src/km_engine.c',
     'if (g <= (int32_t)k->press_q4 && f <= g + RT_DIR_TOL_Q4 &&', 'if (f <= g + RT_DIR_TOL_Q4 &&'),
    ('rt: press without the rise', 'core/src/km_engine.c',
     'want = d >= (unsigned)rapid_depth(k, top) + k->cfg.rt_down_mm100 &&\n               (k->cfg.continuous', 'want = (k->cfg.continuous'),
    ('rt: press uses rt_up', 'core/src/km_engine.c',
     'rapid_depth(k, top) + k->cfg.rt_down_mm100', 'rapid_depth(k, top) + k->cfg.rt_up_mm100'),
    ('rt: release uses rt_down', 'core/src/km_engine.c',
     'rapid_depth(k, low) - rapid_depth(k, c) >= k->cfg.rt_up_mm100', 'rapid_depth(k, low) - rapid_depth(k, c) >= k->cfg.rt_down_mm100'),
    ('rt: actuation point ignored (always continuous)', 'core/src/km_engine.c',
     '(k->cfg.continuous || d >= k->cfg.press_mm100)', '(1)'),
    ('rt: actuation point always used', 'core/src/km_engine.c',
     '(k->cfg.continuous || d >= k->cfg.press_mm100)', '(d >= k->cfg.press_mm100)'),
    ('rt: shallowest point not tracked', 'core/src/km_engine.c',
     '    if (top == EXT_NONE || c > top) {', '    if (top == EXT_NONE) {'),
    ('rt: deepest point not tracked', 'core/src/km_engine.c',
     '        if (c < low) {', '        if (low == EXT_NONE) {'),
    ('rt: unset shallowest point is not replaced', 'core/src/km_engine.c',
     '    if (top == EXT_NONE || c > top) {', '    if (c > top) {'),
    ('rt: press debounce one frame longer', 'core/src/km_engine.c',
     '        if (++k->cnt >= KM_ENGINE_PRESS_FRAMES) {\n            k->cnt = 0;\n            k->flags |= F_DOWN;\n            k->ext = (uint16_t)c;', '        if (++k->cnt > KM_ENGINE_PRESS_FRAMES) {\n            k->cnt = 0;\n            k->flags |= F_DOWN;\n            k->ext = (uint16_t)c;'),
    ('rt: press debounce one frame shorter', 'core/src/km_engine.c',
     '        if (++k->cnt >= KM_ENGINE_PRESS_FRAMES) {\n            k->cnt = 0;\n            k->flags |= F_DOWN;\n            k->ext = (uint16_t)c;', '        if (++k->cnt >= KM_ENGINE_PRESS_FRAMES - 1u) {\n            k->cnt = 0;\n            k->flags |= F_DOWN;\n            k->ext = (uint16_t)c;'),
    ('rt: release (zone) debounce one frame shorter', 'core/src/km_engine.c',
     '        } else if (++k->cnt >= KM_ENGINE_RELEASE_FRAMES) {\n            k->ext =', '        } else if (++k->cnt >= KM_ENGINE_RELEASE_FRAMES - 1u) {\n            k->ext ='),
    ('rt: release (zone) debounce one frame longer', 'core/src/km_engine.c',
     '        } else if (++k->cnt >= KM_ENGINE_RELEASE_FRAMES) {\n            k->ext =', '        } else if (++k->cnt > KM_ENGINE_RELEASE_FRAMES) {\n            k->ext ='),
    ('rt: release (reversal) debounce one frame shorter', 'core/src/km_engine.c',
     '            if (++k->cnt >= KM_ENGINE_RELEASE_FRAMES) {\n                k->ext = (uint16_t)c; ', '            if (++k->cnt >= KM_ENGINE_RELEASE_FRAMES - 1u) {\n                k->ext = (uint16_t)c; '),
    ('rt: zone release without hysteresis', 'core/src/km_engine.c',
     '(uint16_t)(td - KM_ENGINE_RT_EDGE_HYST_MM100 + 1u)', '(uint16_t)(td + 1u)'),
    ('rt: zone release hysteresis too small (5)', 'core/include/km/km_engine.h',
     '#define KM_ENGINE_RT_EDGE_HYST_MM100 10u', '#define KM_ENGINE_RT_EDGE_HYST_MM100 5u'),
    ('rt: zone release never', 'core/src/km_engine.c',
     '    if (g >= (int32_t)k->release_q4) {          /* back in the top dead zone */', '    if (0) {          /* back in the top dead zone */'),
    ('rt: after a zone release the shallowest point is forgotten', 'core/src/km_engine.c',
     'k->ext = (uint16_t)((k->release_q4 + 8u) >> 4);', 'k->ext = EXT_NONE;'),
    ('rt: top zone 0 used as 0 in the thresholds', 'core/src/km_engine.c',
     'const uint16_t td = top_dead_eff(&k->cfg);', 'const uint16_t td = k->cfg.top_dead_mm100;'),
    ('rt: bottom zone ignored', 'core/src/km_engine.c',
     '    } else if (g <= (int32_t)k->bz_q4) {', '    } else if (0) {'),
    ('rt: bottom zone width ignored', 'core/src/km_engine.c',
     '(uint16_t)(KM_TRAVEL_MM100 - k->cfg.bottom_dead_mm100)', '(uint16_t)(KM_TRAVEL_MM100)'),
    ('rt: bottom zone twice as wide', 'core/src/km_engine.c',
     '(uint16_t)(KM_TRAVEL_MM100 - k->cfg.bottom_dead_mm100)', '(uint16_t)(KM_TRAVEL_MM100 - 2u * k->cfg.bottom_dead_mm100)'),
    ('rt: bottom zone does not count as fully pressed', 'core/src/km_engine.c',
     '        k->ext = k->bottom;                     /* the deepest point there is */\n', ''),
    ('rt: nothing learned in the bottom zone', 'core/src/km_engine.c',
     '        k->cnt = 0;\n        if (f < (int32_t)k->learn_q4) {\n            learn_step(e, k, f);\n        }\n        k->ext = k->bottom;', '        k->cnt = 0;\n        k->ext = k->bottom;'),
    ('rt: rest line moved to 0.60', 'core/include/km/km_engine.h',
     '#define KM_ENGINE_RT_REST_MM100 5u ', '#define KM_ENGINE_RT_REST_MM100 60u'),
    ('rt: glitch undo keeps the deep extreme', 'core/src/km_engine.c',
     '            e->slot[newly[i]].ext = e->slot[newly[i]].thr_base; /* rapid trigger: qualify again from the top */\n', ''),
    ('rt: fault / disable does not reset the extreme', 'core/src/km_engine.c',
     '    k->cnt = 0;\n    k->ext = EXT_NONE;\n}', '    k->cnt = 0;\n}'),
    ('rt: reading taken without rounding (key up)', 'core/src/km_engine.c',
     'const unsigned c = (unsigned)((g + 8) >> 4);\n    unsigned top', 'const unsigned c = (unsigned)(g >> 4);\n    unsigned top'),
    ('rt: reading taken without rounding (key down)', 'core/src/km_engine.c',
     'const unsigned c = (unsigned)((g + 8) >> 4);\n        unsigned low', 'const unsigned c = (unsigned)(g >> 4);\n        unsigned low'),
    ('rt: prefilter needs twice the stroke (press)', 'core/src/km_engine.c',
     'k->mc_dn = stroke_counts(&cal, k->cfg.rt_down_mm100);', 'k->mc_dn = 2u * stroke_counts(&cal, k->cfg.rt_down_mm100);'),
    ('rt: prefilter needs twice the stroke (release)', 'core/src/km_engine.c',
     'k->mc_up = stroke_counts(&cal, k->cfg.rt_up_mm100);', 'k->mc_up = 2u * stroke_counts(&cal, k->cfg.rt_up_mm100);'),
    ('rt: prefilter stroke one unit too long', 'core/src/km_engine.c',
     'edge_value(cal, (uint16_t)(dist_mm100 - 1u))', 'edge_value(cal, (uint16_t)(dist_mm100))'),
    ('rt: prefilter stroke measured with the wrong distance (release)', 'core/src/km_engine.c',
     'k->mc_up = stroke_counts(&cal, k->cfg.rt_up_mm100);', 'k->mc_up = stroke_counts(&cal, k->cfg.rt_down_mm100);'),
    ('rt: prefilter off (press)', 'core/src/km_engine.c',
     'k->mc_dn = stroke_counts(&cal, k->cfg.rt_down_mm100);', 'k->mc_dn = 1u;'),
    ('rt: prefilter off (release)', 'core/src/km_engine.c',
     'k->mc_up = stroke_counts(&cal, k->cfg.rt_up_mm100);', 'k->mc_up = 1u;'),
    # reports
    ("boot report: no duplicate filter", "core/src/km_report.c",
     "        if (dup) {\n            continue;\n        }\n", ""),
    ("boot report: rollover at seven keys late", "core/src/km_report.c",
     "if (count == KM_REPORT_BOOT_KEYS) {", "if (count == KM_REPORT_BOOT_KEYS + 1u) {"),
    ("boot report: rollover forgets modifiers", "core/src/km_report.c",
     "    report[0] = mods;", "    report[0] = overflow ? 0 : mods;"),
    ("boot report: rollover code 0x02", "core/src/km_report.c",
     "#include \"km/km_keymap.h\"\n", "#include \"km/km_keymap.h\"\n#undef KM_REPORT_ROLLOVER\n#define KM_REPORT_ROLLOVER 0x02u\n"),
    ("boot report: modifier bit order reversed", "core/src/km_report.c",
     "mods = (uint8_t)(mods | (1u << (usage - KM_USAGE_MOD_FIRST)));", "mods = (uint8_t)(mods | (0x80u >> (usage - KM_USAGE_MOD_FIRST)));"),
    ("boot report: keys sorted by usage", "core/src/km_report.c",
     "        keys[count++] = usage;", "        keys[count++] = usage;\n        for (unsigned q = count - 1u; q > 0 && keys[q - 1] > keys[q]; q--) {\n            uint8_t t = keys[q]; keys[q] = keys[q - 1]; keys[q - 1] = t;\n        }"),
    ("nkro: bit order reversed", "core/src/km_report.c",
     "report[usage >> 3] = (uint8_t)(report[usage >> 3] | (1u << (usage & 7u)));", "report[usage >> 3] = (uint8_t)(report[usage >> 3] | (0x80u >> (usage & 7u)));"),
    ("nkro: last usage dropped", "core/src/km_report.c",
     "usage < KM_REPORT_NKRO_BITS", "usage < KM_REPORT_NKRO_BITS - 1u"),
    ("nkro: usage 4 dropped", "core/src/km_report.c",
     "if (usage >= 4u &&", "if (usage >= 5u &&"),
    # key map
    ("keymap: two usages swapped (Y/Z)", "core/src/km_keymap.c",
     '{ 0x1C, "Y" }', '{ 0x1D, "Y" }'),
    ("keymap: Fn mapped to a key", "core/src/km_keymap.c",
     '{ 0x00, "FN" }', '{ 0x65, "FN" }'),
    ("keymap: slot out of range accepted", "core/src/km_keymap.c",
     "if (layer >= KM_KEYMAP_LAYERS || slot >= KM_KEYMAP_SLOTS) {", "if (layer >= KM_KEYMAP_LAYERS) {"),
    ("keymap: mirror slot is a key", "core/src/km_keymap.c",
     '/* 15 */ { 0x00, "" }', '/* 15 */ { 0x00, "?" }'),
    ("keymap: right GUI wrong", "core/src/km_keymap.c",
     '{ 0xE7, "RWIN" }', '{ 0xE8, "RWIN" }'),
]


STUB = '''
#include <stdio.h>
#include "km_test.h"
int km_test_checks, km_test_failures;
size_t km_hex(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (hex[0] && hex[1] && n < cap) {
        unsigned v;
        char t[3] = { hex[0], hex[1], 0 };
        sscanf(t, "%2x", &v);
        out[n++] = (uint8_t)v;
        hex += 2;
    }
    return n;
}
int main(void) { test_engine(); return km_test_failures ? 1 : 0; }
'''


# Reviewed by hand: these change a threshold comparison by 1/16 count (the Q4 resolution of the filter) or
# the baseline by less than one count, far below the +-15 count noise; no test can tell them apart.
EQUIVALENT = {
    "press compare < instead of <=": "differs only at filt == press_q4, which a filter converging from above never reaches",
    "release compare > instead of >=": "differs by 1/16 count",
    "baseline not rounded": "baseline off by < 1 count",
    # rapid trigger
    "rt: general rest branch does not forget the extreme": "g is above the rest line while f is not only for a few frames, and the minimum tracking of rapid_up lowers the extreme on the next evaluation",
    "rt: rest does not reset the press counter": "redundant: every path to the rest region passes a frame in which the rapid-trigger "
                                                   "press condition is false (the direction guard or the top-zone test), which already clears the counter",
    "rt: fault / disable does not reset the extreme": "the key is in the top zone when it re-arms, where the shallowest/deepest point is "
                                                      "overwritten on the very next evaluation",
    "rt: reading taken without rounding (key up)": "half a count (about 0.005 mm at the top, less below)",
    "rt: reading taken without rounding (key down)": "half a count (about 0.005 mm at the top, less below)",
    "rt: after a zone release the shallowest point is forgotten": "the key has just read at or above the edge of the top zone; the next "
                                                                  "evaluation takes the highest reading since, which is that one or a higher one (only a fall back "
                                                                  "below the edge within a frame would differ, and the 0.10 mm hysteresis keeps it out)",
}


def env() -> dict:
    e = dict(os.environ)
    ucrt = Path("C:/msys64/ucrt64/bin")
    if os.name == "nt" and ucrt.exists():
        e["PATH"] = str(ucrt) + os.pathsep + e["PATH"]
    return e


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--only", help="run only mutations whose name contains this text")
    args = ap.parse_args()
    if args.list:
        for i, m in enumerate(MUTATIONS, 1):
            print("%2d  %s" % (i, m[0]))
        return 0

    e = env()
    cc = shutil.which("gcc", path=e["PATH"])
    work = Path(tempfile.mkdtemp(prefix="km_mut_"))
    (work / "main.c").write_text(STUB)
    survivors, bad_build = [], []
    try:
        for i, (name, rel, old, new) in enumerate(MUTATIONS, 1):
            if args.only and args.only not in name:
                continue
            tree = work / ("m%d" % i)
            shutil.copytree(FW / "core", tree / "core")
            shutil.copytree(FW / "tests", tree / "tests", ignore=shutil.ignore_patterns("golden", "sim"))
            target = tree / rel
            text = target.read_text(newline="")
            text = text.replace("\r\n", "\n")
            if text.count(old) != 1:
                print("%2d  %-50s BAD MUTATION (pattern found %d times)" % (i, name, text.count(old)))
                bad_build.append(name)
                continue
            target.write_text(text.replace(old, new), newline="")
            exe = tree / "t.exe"
            cmd = [cc, "-std=gnu11", "-O1", "-w", "-I" + str(tree / "core/include"), "-I" + str(tree / "tests"),
                   str(work / "main.c"), str(tree / "tests/test_engine.c"),
                   *[str(tree / s) for s in SRC], "-o", str(exe)]
            r = subprocess.run(cmd, env=e, capture_output=True, text=True)
            if r.returncode != 0:
                print("%2d  %-50s BUILD FAILED" % (i, name))
                print(r.stderr[:300])
                bad_build.append(name)
                continue
            r = subprocess.run([str(exe)], env=e, capture_output=True, text=True, timeout=600)
            if r.returncode == 0 and name in EQUIVALENT:
                print("%2d  %-50s equivalent (%s)" % (i, name, EQUIVALENT[name]))
            elif r.returncode == 0:
                print("%2d  %-50s SURVIVED" % (i, name))
                survivors.append(name)
            else:
                first = next((l.strip() for l in r.stdout.splitlines() if "FAIL" in l), "(crash)")
                print("%2d  %-50s killed  %s" % (i, name, first[:90]))
            shutil.rmtree(tree, ignore_errors=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("\n%d mutations, %d survivors, %d broken" % (len(MUTATIONS), len(survivors), len(bad_build)))
    for s in survivors:
        print("  survivor:", s)
    return 1 if survivors or bad_build else 0


if __name__ == "__main__":
    sys.exit(main())
