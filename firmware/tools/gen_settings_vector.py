#!/usr/bin/env python3
"""Cross-language test vectors for the settings blob: the REAL firmware encoder/decoder against the JavaScript codec.

    python firmware/tools/gen_settings_vector.py            # build the C harness, write ui/tests/vectors/settings_vectors.json
    python firmware/tools/gen_settings_vector.py --check    # same, but only compare with the committed file (exit 1 if stale)

Settings format version 3 (M6 task P0) is covered the same way: scenarios that need version 3 are encoded by the real writer, hand-made and
damaged version 3 blobs are decoded by the real reader (and written out again), and the table-entry rules of the new codes are dumped as
complete tables, so ui/tests/settings-codec.test.mjs can check the JS codec against all of it.

What it does: compiles a tiny C program (in a temporary directory, nothing is kept) from the real firmware/core/src/km_settings.c
km_keymap.c and km_engine.c (the engine owns the key-config rules; it needs km_depth_model.c), feeds it scenarios (settings to encode, blobs to decode, key configs and usage bytes to validate) and writes
what the firmware answered as hex into the vector file. ui/tests/settings-codec.test.mjs then makes the JavaScript codec
produce/accept/refuse exactly the same bytes. Nothing is pasted by hand: change km_settings.c, run this, and the test shows
whether the JS codec still agrees.

On Windows the MSYS2 ucrt64 compiler is put first on PATH (as in run_tests.py).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

FW = Path(__file__).resolve().parents[1]
REPO = FW.parent
OUT = REPO / "ui" / "tests" / "vectors" / "settings_vectors.json"

PROFILES, LAYERS, SLOTS, SIZE = 4, 4, 76, 7944
V1_SIZE = 5488                      # settings format version 1: still read (and migrated) by the firmware
KEYS, V3_MAX = 61, 8176             # settings format version 3: slots that carry a key, largest blob (one store sector minus the header)
NO_KEY_SLOTS = [15, 17, 20, 24, 33, 35, 40, 41, 46, 50, 51, 53, 58, 69, 71]
KEY_SLOTS = [i for i in range(SLOTS) if i not in NO_KEY_SLOTS]
CFG_BYTES, OPTS_BYTES = 14, 6
V1_PROFILE = SLOTS * CFG_BYTES + LAYERS * SLOTS
V2_PROFILE = SLOTS * CFG_BYTES + OPTS_BYTES + LAYERS * SLOTS + LAYERS * SLOTS * 2


def opts_off(p):
    return 12 + p * V2_PROFILE + SLOTS * CFG_BYTES


def code_off(p, l, slot):
    return opts_off(p) + OPTS_BYTES + l * SLOTS + slot


def arg_off(p, l, slot, i):
    return opts_off(p) + OPTS_BYTES + LAYERS * SLOTS + (l * SLOTS + slot) * 2 + i


def to_v1(v2: bytes) -> bytes:
    """The version 1 blob that holds the same keys and codes as a version 2 blob (arguments and layer options dropped)."""
    out = bytearray(v2[:12])
    out[4:6] = (1).to_bytes(2, "little")
    out[6:8] = V1_SIZE.to_bytes(2, "little")
    for p in range(PROFILES):
        out += v2[12 + p * V2_PROFILE: opts_off(p)]
        out += v2[opts_off(p) + OPTS_BYTES: opts_off(p) + OPTS_BYTES + LAYERS * SLOTS]
    out += zlib.crc32(bytes(out)).to_bytes(4, "little")
    assert len(out) == V1_SIZE
    return bytes(out)

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "km/km_settings.h"
#include "km/km_keymap.h"

static km_settings_t g_s;
static uint8_t g_buf[KM_SETTINGS_V3_MAX_SIZE + 64];

static void print_hex(const uint8_t *b, unsigned n) { for (unsigned i = 0; i < n; i++) printf("%02x", b[i]); }

static int hexval(int c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

/* The "intended" default: km_keymap_default_table() raw codes (the Fn key holds layer 1, overlay entries are 'transparent' 0xFF).
 * With flatten, 'transparent' is resolved to the layer-0 code of the slot: equal in behaviour, and storable by a firmware whose
 * usage rule stops at 0xFA. (km_settings_default() itself fills layers from km_keymap_usage(), which reads every function code as 0.) */
static void table_default(km_settings_t *s, int flatten)
{
    uint8_t t[KM_LAYERS][KM_KEYMAP_SLOTS];
    km_settings_default(s);
    km_keymap_default_table(t);
    for (unsigned p = 0; p < KM_PROFILES; p++)
        for (unsigned l = 0; l < KM_LAYERS; l++)
            for (unsigned i = 0; i < KM_KEYMAP_SLOTS; i++) {
                uint8_t c = t[l][i];
                if (flatten && c == 0xFFu) c = t[0][i];
                if (flatten && c == 0xFFu) c = 0;
                s->profile[p].layer_usage[l][i] = c;
            }
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "r");
    if (!f) return 3;
    char op[16], name[64];
    while (fscanf(f, "%15s", op) == 1) {
        if (!strcmp(op, "DEFAULT") || !strcmp(op, "FLATDEFAULT") || !strcmp(op, "TABLEDEFAULT")) {
            if (op[0] == 'F') table_default(&g_s, 1); else if (op[0] == 'T') table_default(&g_s, 0); else km_settings_default(&g_s);
            unsigned n = km_settings_encode(&g_s, g_buf, sizeof g_buf);
            printf("R %s ", op);
            if (n) print_hex(g_buf, n); else printf("REJECT");
            printf("\n");
        } else if (!strcmp(op, "CASE")) {
            unsigned active, nc, nu, no;
            if (fscanf(f, "%63s %u %u %u %u", name, &active, &nc, &nu, &no) != 5) return 4;
            km_settings_default(&g_s);
            g_s.active = (uint8_t)active;
            for (unsigned i = 0; i < nc; i++) {
                unsigned p, slot, pr, rl, mode, cont, rd, ru, td, bd;
                if (fscanf(f, "%u %u %u %u %u %u %u %u %u %u", &p, &slot, &pr, &rl, &mode, &cont, &rd, &ru, &td, &bd) != 10) return 5;
                km_engine_key_cfg_t *c = &g_s.profile[p].key[slot];
                c->press_mm100 = (uint16_t)pr; c->release_mm100 = (uint16_t)rl; c->mode = (uint8_t)mode; c->continuous = (uint8_t)cont;
                c->rt_down_mm100 = (uint16_t)rd; c->rt_up_mm100 = (uint16_t)ru; c->top_dead_mm100 = (uint16_t)td; c->bottom_dead_mm100 = (uint16_t)bd;
            }
            for (unsigned i = 0; i < nu; i++) {
                unsigned p, l, slot, v, a0, a1;
                if (fscanf(f, "%u %u %u %u %u %u", &p, &l, &slot, &v, &a0, &a1) != 6) return 6;
                g_s.profile[p].layer_usage[l][slot] = (uint8_t)v;
                g_s.profile[p].layer_arg[l][slot][0] = (uint8_t)a0;
                g_s.profile[p].layer_arg[l][slot][1] = (uint8_t)a1;
            }
            for (unsigned i = 0; i < no; i++) {
                unsigned p, term, one, pol;
                if (fscanf(f, "%u %u %u %u", &p, &term, &one, &pol) != 4) return 11;
                g_s.profile[p].layer_opts.tap_term_ms = (uint16_t)term;
                g_s.profile[p].layer_opts.oneshot_ms = (uint16_t)one;
                g_s.profile[p].layer_opts.tap_policy = (uint8_t)pol;
            }
            unsigned n = km_settings_encode(&g_s, g_buf, sizeof g_buf);
            printf("R %s ", name);
            if (n) print_hex(g_buf, n); else printf("REJECT");
            printf("\n");
        } else if (!strcmp(op, "ENTRY")) {
            unsigned code, a0, a1;
            if (fscanf(f, "%63s %u %u %u", name, &code, &a0, &a1) != 4) return 12;
            printf("R %s %d%d\n", name, km_settings_entry_valid((uint8_t)code, (uint8_t)a0, (uint8_t)a1) ? 1 : 0,
                   km_keymap_entry_valid_v2((uint8_t)code, (uint8_t)a0, (uint8_t)a1) ? 1 : 0);
        } else if (!strcmp(op, "OPTS")) {
            unsigned term, one, pol;
            if (fscanf(f, "%63s %u %u %u", name, &term, &one, &pol) != 4) return 13;
            km_layer_opts_t o = { (uint16_t)term, (uint16_t)one, (uint8_t)pol };
            printf("R %s %d\n", name, km_settings_opts_valid(&o) ? 1 : 0);
        } else if (!strcmp(op, "MIG")) {
            static char hex[2 * (KM_SETTINGS_V3_MAX_SIZE + 64) + 4];
            if (fscanf(f, "%63s %s", name, hex) != 2) return 14;
            unsigned len = (unsigned)strlen(hex) / 2;
            for (unsigned i = 0; i < len; i++) g_buf[i] = (uint8_t)(hexval(hex[2 * i]) * 16 + hexval(hex[2 * i + 1]));
            km_settings_t out;
            memset(&out, 0, sizeof out);
            printf("R %s ", name);
            if (km_settings_decode(&out, g_buf, len)) {
                static uint8_t again[KM_SETTINGS_V3_MAX_SIZE];
                unsigned n = km_settings_encode(&out, again, sizeof again);
                if (n) print_hex(again, n); else printf("REJECT");
            } else {
                printf("REJECT");
            }
            printf("\n");
        } else if (!strcmp(op, "DEFAULT3")) {
            km_settings_default(&g_s);
            unsigned n = km_settings_encode_v3(&g_s, g_buf, sizeof g_buf);
            printf("R DEFAULT3 ");
            if (n) print_hex(g_buf, n); else printf("REJECT");
            printf("\n");
        } else if (!strcmp(op, "KEYSLOTS")) {
            printf("R KEYSLOTS ");
            for (unsigned i = 0; i < KM_KEYMAP_SLOTS; i++) printf("%d", km_keymap_slot_is_key(i) ? 1 : 0);
            printf("\n");
        } else if (!strcmp(op, "ENTRYMAP")) {
            unsigned code;
            if (fscanf(f, "%u", &code) != 1) return 21;
            for (int v2 = 0; v2 < 2; v2++) {
                printf("R EMAP%d_%u ", v2, code);
                for (unsigned a0 = 0; a0 < 256; a0++) {
                    for (unsigned nib = 0; nib < 64; nib++) {
                        unsigned x = 0;
                        for (unsigned k = 0; k < 4; k++) {
                            const unsigned a1 = nib * 4 + k;
                            const int ok = v2 ? km_keymap_entry_valid_v2((uint8_t)code, (uint8_t)a0, (uint8_t)a1)
                                              : km_settings_entry_valid((uint8_t)code, (uint8_t)a0, (uint8_t)a1);
                            x |= (unsigned)ok << k;
                        }
                        printf("%x", x);
                    }
                }
                printf("\n");
            }
        } else if (!strcmp(op, "CASEX")) {
            /* a scenario with the M6 members: commands until END, see build_spec() */
            unsigned active;
            char c[16];
            if (fscanf(f, "%63s %u", name, &active) != 2) return 20;
            km_settings_default(&g_s);
            g_s.active = (uint8_t)active;
            while (fscanf(f, "%15s", c) == 1 && strcmp(c, "END") != 0) {
                unsigned v[16];
                unsigned want = !strcmp(c, "cfg") ? 10 : !strcmp(c, "ent") ? 6 : !strcmp(c, "opt") ? 4 : !strcmp(c, "dks") ? 14 : !strcmp(c, "dksn") ? 2
                    : !strcmp(c, "morph") ? 6 : !strcmp(c, "morphn") ? 2 : !strcmp(c, "socd") ? 6 : !strcmp(c, "socdn") ? 2 : !strcmp(c, "pad") ? 6
                    : !strcmp(c, "padopt") ? 5 : !strcmp(c, "dev") ? 3 : 99;
                if (want > 16) return 22;
                for (unsigned i = 0; i < want; i++) if (fscanf(f, "%u", &v[i]) != 1) return 23;
                km_profile_t *q = &g_s.profile[v[0] & 3u];
                if (!strcmp(c, "cfg")) {
                    km_engine_key_cfg_t *k = &q->key[v[1]];
                    k->press_mm100 = (uint16_t)v[2]; k->release_mm100 = (uint16_t)v[3]; k->mode = (uint8_t)v[4]; k->continuous = (uint8_t)v[5];
                    k->rt_down_mm100 = (uint16_t)v[6]; k->rt_up_mm100 = (uint16_t)v[7]; k->top_dead_mm100 = (uint16_t)v[8]; k->bottom_dead_mm100 = (uint16_t)v[9];
                } else if (!strcmp(c, "ent")) {
                    q->layer_usage[v[1]][v[2]] = (uint8_t)v[3]; q->layer_arg[v[1]][v[2]][0] = (uint8_t)v[4]; q->layer_arg[v[1]][v[2]][1] = (uint8_t)v[5];
                } else if (!strcmp(c, "opt")) {
                    q->layer_opts.tap_term_ms = (uint16_t)v[1]; q->layer_opts.oneshot_ms = (uint16_t)v[2]; q->layer_opts.tap_policy = (uint8_t)v[3];
                } else if (!strcmp(c, "dks")) {
                    km_dks_rec_t *r = &q->dks[v[1] & 7u];
                    r->t_act_mm100 = (uint16_t)v[2]; r->t_bot_mm100 = (uint16_t)v[3]; r->flags = (uint8_t)v[4];
                    for (unsigned a = 0; a < 4; a++) { r->act[a].usage = (uint8_t)v[5 + 2 * a]; r->act[a].ev = (uint8_t)v[6 + 2 * a]; }
                    r->rsvd[0] = (uint8_t)v[13];
                    if (q->dks_n < v[1] + 1u) q->dks_n = (uint8_t)(v[1] + 1u);
                } else if (!strcmp(c, "dksn")) {
                    q->dks_n = (uint8_t)v[1];
                } else if (!strcmp(c, "morph")) {
                    km_morph_rec_t *r = &q->morph[v[1] & 7u];
                    r->mod_mask = (uint8_t)v[2]; r->usage = (uint8_t)v[3]; r->flags = (uint8_t)v[4]; r->rsvd = (uint8_t)v[5];
                    if (q->morph_n < v[1] + 1u) q->morph_n = (uint8_t)(v[1] + 1u);
                } else if (!strcmp(c, "morphn")) {
                    q->morph_n = (uint8_t)v[1];
                } else if (!strcmp(c, "socd")) {
                    km_socd_pair_t *r = &q->socd[v[1] & 3u];
                    r->slot_a = (uint8_t)v[2]; r->slot_b = (uint8_t)v[3]; r->mode = (uint8_t)v[4]; r->flags = (uint8_t)v[5];
                    if (q->socd_n < v[1] + 1u) q->socd_n = (uint8_t)(v[1] + 1u);
                } else if (!strcmp(c, "socdn")) {
                    q->socd_n = (uint8_t)v[1];
                } else if (!strcmp(c, "pad")) {
                    km_pad_curve_t *r = &q->pad_curve[v[1] & 3u];
                    r->start_mm100_5 = (uint8_t)v[2]; r->end_mm100_5 = (uint8_t)v[3]; r->shape = (uint8_t)v[4]; r->flags = (uint8_t)v[5];
                } else if (!strcmp(c, "padopt")) {
                    q->pad_opts.stick_deadzone = (uint8_t)v[1]; q->pad_opts.circular = (uint8_t)v[2]; q->pad_opts.smooth = (uint8_t)v[3];
                    memset(q->pad_opts.rsvd, (int)v[4], sizeof q->pad_opts.rsvd);
                } else if (!strcmp(c, "dev")) {
                    g_s.device.pad_mode = (uint8_t)v[0]; g_s.device.filter_mode = (uint8_t)v[1]; memset(g_s.device.rsvd, (int)v[2], sizeof g_s.device.rsvd);
                } else {
                    return 24;
                }
            }
            unsigned n = km_settings_encode(&g_s, g_buf, sizeof g_buf);
            printf("R %s ", name);
            if (n) print_hex(g_buf, n); else printf("REJECT");
            printf("\n");
        } else if (!strcmp(op, "USAGE")) {
            printf("R USAGE ");
            for (unsigned u = 0; u < 256; u++) printf("%d", km_settings_usage_valid((uint8_t)u) ? 1 : 0);
            printf("\n");
        } else if (!strcmp(op, "CFG")) {
            unsigned pr, rl, mode, cont, rd, ru, td, bd;
            if (fscanf(f, "%63s %u %u %u %u %u %u %u %u", name, &pr, &rl, &mode, &cont, &rd, &ru, &td, &bd) != 9) return 7;
            km_engine_key_cfg_t c;
            memset(&c, 0, sizeof c);
            c.press_mm100 = (uint16_t)pr; c.release_mm100 = (uint16_t)rl; c.mode = (uint8_t)mode; c.continuous = (uint8_t)cont;
            c.rt_down_mm100 = (uint16_t)rd; c.rt_up_mm100 = (uint16_t)ru; c.top_dead_mm100 = (uint16_t)td; c.bottom_dead_mm100 = (uint16_t)bd;
            printf("R %s %d\n", name, km_settings_cfg_valid(&c) ? 1 : 0);
        } else if (!strcmp(op, "DEC")) {
            static char hex[2 * (KM_SETTINGS_V3_MAX_SIZE + 64) + 4];
            if (fscanf(f, "%63s %s", name, hex) != 2) return 8;
            unsigned len = (unsigned)strlen(hex) / 2;
            if (!strcmp(hex, "-")) len = 0;
            for (unsigned i = 0; i < len; i++) g_buf[i] = (uint8_t)(hexval(hex[2 * i]) * 16 + hexval(hex[2 * i + 1]));
            km_settings_t out;
            memset(&out, 0, sizeof out);
            printf("R %s %s\n", name, km_settings_decode(&out, g_buf, len) ? "ACCEPT" : "REJECT");
        } else if (!strcmp(op, "CRC")) {
            static char hex[1024];
            if (fscanf(f, "%s", hex) != 1) return 9;
            unsigned len = (unsigned)strlen(hex) / 2;
            for (unsigned i = 0; i < len; i++) g_buf[i] = (uint8_t)(hexval(hex[2 * i]) * 16 + hexval(hex[2 * i + 1]));
            printf("R CRC %08x\n", km_crc32(0, g_buf, len));
        } else {
            return 10;
        }
    }
    fclose(f);
    return 0;
}
'''


def u6(x):
    """A layer-entry edit (profile, layer, slot, code[, arg0, arg1]) as the six numbers the harness reads."""
    return list(x) + [0] * (6 - len(x))


def cfg(**kw):
    c = dict(press=120, release=80, mode=0, continuous=0, rtDown=40, rtUp=40, topDead=0, bottomDead=0)
    c.update(kw)
    return c


def scenarios():
    """Non-trivial settings, as edits on top of km_settings_default(). Edits use the JS raw model's field names."""
    all_slots = list(range(SLOTS))
    out = []

    def case(name, active=0, cfgs=(), usages=(), opts=()):
        out.append({"name": name, "active": active, "cfgs": [list(x) for x in cfgs], "usages": [u6(x) for x in usages], "opts": [list(x) for x in opts]})

    case("active_only", active=3)
    case("rapid_every_slot", active=1,
         cfgs=[(0, s, cfg(press=150, release=100, mode=1, continuous=1, rtDown=25, rtUp=30, topDead=20, bottomDead=40)) for s in all_slots])
    case("profiles_differ", active=2, cfgs=[
        (0, 5, cfg(press=340, release=10)),
        (1, 18, cfg(press=61, release=60, mode=1, rtDown=10, rtUp=340)),
        (2, 75, cfg(press=200, release=199, topDead=169, bottomDead=170)),       # dead zones sum 339: the largest accepted
        (3, 0, cfg(press=11, release=10, mode=1, continuous=1, rtDown=340, rtUp=10, topDead=0, bottomDead=0)),
    ])
    function_codes = [0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA]
    case("layers_functions", usages=[(3, 3, 10 + i, c) for i, c in enumerate(function_codes)] + [
        (0, 0, 0, 0x04), (0, 0, 1, 0xE7), (1, 2, 20, 0xE0), (2, 1, 30, 0x00), (0, 1, 14, 0xF0),
    ])
    case("layers_every_slot", usages=[(p, l, s, 0x04 + ((p * 31 + l * 17 + s * 3) % 0xE4)) for p in range(PROFILES) for l in range(LAYERS) for s in all_slots])
    case("layers_hold_L2_L3", usages=[(0, 1, 7, 0xFB), (0, 2, 7, 0xFC)])
    case("layers_transparent", usages=[(2, 3, 70, 0xFF)])
    case("layers_reserved_FD_FE", usages=[(1, 1, 9, 0xFD), (1, 2, 9, 0xFE)])
    # version 2: toggle layer, tap-hold, one-shot modifier (code + two argument bytes), and the per-profile layer options
    case("toggle_layers", usages=[(0, 0, 18, 0xE8, 1), (0, 1, 18, 0xE8, 2), (1, 3, 10, 0xE8, 3), (2, 2, 11, 0xE8, 2)])
    case("tap_hold_layer", usages=[(0, 0, 63, 0xE9, 0x2C, 0xF0), (1, 0, 63, 0xE9, 0x2C, 0xFB), (2, 0, 63, 0xE9, 0x2C, 0xFC)])
    case("tap_hold_modifier", usages=[(0, 0, 5, 0xE9, 0x29, 0xE0), (0, 0, 0, 0xE9, 0x2A, 0xE1), (3, 3, 75, 0xE9, 0xE7, 0xE7)])
    case("tap_hold_media_tap", usages=[(0, 1, 30, 0xE9, 0xF5, 0xF0), (0, 1, 31, 0xE9, 0xFA, 0xE2)])
    case("one_shot_all_modifiers", usages=[(0, 0, i, 0xEA, 0xE0 + i) for i in range(8)])   # slots 0..7 all carry a key
    case("layer_opts_custom", opts=[(0, 30, 0, 0), (1, 1000, 10000, 2), (2, 200, 100, 1), (3, 777, 2468, 0)])
    case("everything_v2", active=2, cfgs=[(1, 3, cfg(press=150, release=100, mode=1, rtDown=25, rtUp=30))], usages=[
        (1, 0, 18, 0xE8, 2), (1, 2, 18, 0xE8, 1), (1, 0, 63, 0xE9, 0x2C, 0xF0), (1, 0, 5, 0xE9, 0x29, 0xE0), (1, 0, 0, 0xEA, 0xE1),
        (2, 1, 54, 0x04)], opts=[(1, 150, 1500, 1)])
    # refused by the firmware: each of these must come back REJECT and the JS codec must refuse them too
    case("reject_toggle_no_layer", usages=[(0, 0, 18, 0xE8, 0, 0)])
    case("reject_toggle_layer_4", usages=[(0, 0, 18, 0xE8, 4, 0)])
    case("reject_toggle_arg1", usages=[(0, 0, 18, 0xE8, 1, 1)])
    case("reject_tap_hold_no_hold", usages=[(0, 0, 63, 0xE9, 0x2C, 0)])
    case("reject_tap_hold_hold_is_key", usages=[(0, 0, 63, 0xE9, 0x2C, 0x04)])
    case("reject_tap_hold_tap_is_layer", usages=[(0, 0, 63, 0xE9, 0xF0, 0xE0)])
    case("reject_tap_hold_tap_is_profile", usages=[(0, 0, 63, 0xE9, 0xF1, 0xE0)])
    case("reject_tap_hold_tap_error_code", usages=[(0, 0, 63, 0xE9, 0x03, 0xE0)])
    case("reject_one_shot_letter", usages=[(0, 0, 0, 0xEA, 0x04)])
    case("reject_one_shot_arg1", usages=[(0, 0, 0, 0xEA, 0xE1, 1)])
    case("reject_arg_on_plain_key", usages=[(0, 0, 73, 0x04, 1, 0)])
    case("reject_arg_on_none", usages=[(0, 2, 73, 0x00, 0, 1)])
    case("reject_code_EB", usages=[(0, 0, 73, 0xEB)])
    case("reject_opts_term_29", opts=[(0, 29, 2000, 0)])
    case("reject_opts_term_1001", opts=[(3, 1001, 2000, 0)])
    case("reject_opts_oneshot_99", opts=[(0, 200, 99, 0)])
    case("reject_opts_oneshot_10001", opts=[(0, 200, 10001, 0)])
    case("reject_opts_policy_3", opts=[(0, 200, 2000, 3)])
    case("reject_usage_01", usages=[(1, 0, 3, 0x01)])
    case("reject_usage_E8_without_args", usages=[(1, 0, 3, 0xE8)])
    case("reject_release_ge_press", cfgs=[(0, 4, cfg(press=100, release=100))])
    case("reject_dead_sum", cfgs=[(0, 4, cfg(topDead=170, bottomDead=170))])
    case("reject_rt_zero", cfgs=[(1, 9, cfg(rtDown=9))])
    case("reject_active", active=4)
    return out


def entry_probes():
    """(name, code, arg0, arg1): km_settings_entry_valid."""
    out = []
    for code in range(256):
        out.append((f"plain_{code:02x}", code, 0, 0))
    for name, code, a0, a1 in [
        ("toggle_1", 0xE8, 1, 0), ("toggle_3", 0xE8, 3, 0), ("toggle_0", 0xE8, 0, 0), ("toggle_4", 0xE8, 4, 0), ("toggle_arg1", 0xE8, 1, 1),
        ("th_key_layer", 0xE9, 0x04, 0xF0), ("th_key_l2", 0xE9, 0x04, 0xFB), ("th_key_l3", 0xE9, 0x04, 0xFC), ("th_key_mod", 0xE9, 0x04, 0xE0),
        ("th_key_mod_last", 0xE9, 0x04, 0xE7), ("th_hold_E8", 0xE9, 0x04, 0xE8), ("th_hold_none", 0xE9, 0x04, 0x00),
        ("th_hold_key", 0xE9, 0x04, 0x05), ("th_hold_profile", 0xE9, 0x04, 0xF1), ("th_hold_media", 0xE9, 0x04, 0xF5), ("th_hold_reserved", 0xE9, 0x04, 0xFD),
        ("th_tap_mod", 0xE9, 0xE7, 0xF0), ("th_tap_E8", 0xE9, 0xE8, 0xF0), ("th_tap_none", 0xE9, 0x00, 0xF0), ("th_tap_03", 0xE9, 0x03, 0xF0),
        ("th_tap_vol_down", 0xE9, 0xF5, 0xF0), ("th_tap_next", 0xE9, 0xFA, 0xF0), ("th_tap_hold_layer", 0xE9, 0xFB, 0xF0), ("th_tap_profile", 0xE9, 0xF4, 0xF0),
        ("th_tap_transparent", 0xE9, 0xFF, 0xF0), ("th_tap_reserved", 0xE9, 0xFD, 0xF0),
        ("os_ctrl", 0xEA, 0xE0, 0), ("os_rgui", 0xEA, 0xE7, 0), ("os_letter", 0xEA, 0x04, 0), ("os_E8", 0xEA, 0xE8, 0), ("os_arg1", 0xEA, 0xE0, 1), ("os_none", 0xEA, 0, 0),
        ("hold_arg0", 0xF0, 1, 0), ("transparent_arg", 0xFF, 0, 1), ("reserved_arg", 0xFD, 5, 0), ("usage_arg1", 0x2C, 0, 9),
    ]:
        out.append((name, code, a0, a1))
    return out


def opts_probes():
    """(name, tap_term, oneshot, policy): km_settings_opts_valid."""
    return [(f"{t}_{o}_{p}", t, o, p) for t, o, p in [
        (200, 2000, 0), (30, 0, 0), (29, 0, 0), (1000, 10000, 2), (1001, 0, 0), (0, 0, 0), (65535, 0, 0), (200, 99, 0), (200, 100, 0), (200, 10001, 0),
        (200, 1, 0), (200, 65535, 0), (200, 0, 1), (200, 0, 2), (200, 0, 3), (200, 0, 255), (500, 5000, 1),
    ]]


def cfg_probes():
    base = cfg()
    probes = []
    for name, change in [
        ("base", {}), ("press_340", dict(press=340)), ("press_341", dict(press=341)), ("release_10", dict(release=10)),
        ("release_9", dict(release=9)), ("release_eq_press", dict(release=120)), ("release_above_press", dict(release=121)),
        ("rtdown_9", dict(rtDown=9)), ("rtdown_10", dict(rtDown=10)), ("rtdown_340", dict(rtDown=340)), ("rtdown_341", dict(rtDown=341)),
        ("rtup_9", dict(rtUp=9)), ("rtup_340", dict(rtUp=340)), ("rtup_341", dict(rtUp=341)),
        ("top_340", dict(topDead=340)), ("top_341", dict(topDead=341)), ("dead_sum_339", dict(topDead=100, bottomDead=239)),
        ("dead_sum_340", dict(topDead=100, bottomDead=240)), ("bottom_339", dict(bottomDead=339)), ("bottom_340", dict(bottomDead=340)),
        ("mode_2", dict(mode=2)), ("mode_255", dict(mode=255)), ("cont_2", dict(continuous=2)), ("rapid_cont", dict(mode=1, continuous=1)),
        ("press_0", dict(press=0, release=0)), ("press_10_release_10", dict(press=10, release=10)),
        # RAPID (mode 1): the engine's rules (km_engine_cfg_valid): distances 20..340, dead zones 0 or 20..340, press 20..340 unless continuous,
        # rt_down <= 340 - bottom, max(top, 20) + bottom + 20 <= 340
        ("rapid_base", dict(mode=1)), ("rapid_rtdown_19", dict(mode=1, rtDown=19)), ("rapid_rtdown_20", dict(mode=1, rtDown=20)),
        ("rapid_rtdown_341", dict(mode=1, rtDown=341)), ("rapid_rtup_19", dict(mode=1, rtUp=19)), ("rapid_rtup_20", dict(mode=1, rtUp=20)),
        ("rapid_rtup_341", dict(mode=1, rtUp=341)), ("rapid_top_19", dict(mode=1, topDead=19)), ("rapid_top_20", dict(mode=1, topDead=20)),
        ("rapid_bottom_19", dict(mode=1, bottomDead=19)), ("rapid_bottom_20", dict(mode=1, bottomDead=20)),
        ("rapid_cont_2", dict(mode=1, continuous=2)), ("rapid_cont_1", dict(mode=1, continuous=1)),
        ("rapid_press_19", dict(mode=1, press=19, release=10)), ("rapid_press_19_cont", dict(mode=1, continuous=1, press=19, release=10)),
        ("rapid_press_0_cont", dict(mode=1, continuous=1, press=0, release=0)), ("rapid_press_341", dict(mode=1, press=341)),
        ("rapid_reach_ok", dict(mode=1, rtDown=240, bottomDead=100)), ("rapid_reach_bad", dict(mode=1, rtDown=241, bottomDead=100)),
        ("rapid_zones_ok", dict(mode=1, topDead=100, bottomDead=220)), ("rapid_zones_bad", dict(mode=1, topDead=100, bottomDead=221)),
        ("rapid_zones_top0_ok", dict(mode=1, topDead=0, bottomDead=300)), ("rapid_zones_top0_bad", dict(mode=1, topDead=0, bottomDead=301)),
        ("fixed_ignores_rapid_fields", dict(mode=0, rtDown=0, rtUp=0, topDead=999, bottomDead=999, continuous=7)),
    ]:
        c = dict(base)
        c.update(change)
        probes.append({"name": name, "cfg": c})
    return probes


def blob_mutations():
    """Corruptions of the default blob: patches [(offset, hex)], optional truncate/extend, and whether the CRC is repaired."""
    return [
        {"name": "intact", "patches": [], "recrc": False},
        {"name": "bad_magic", "patches": [(0, "58")], "recrc": True},
        {"name": "bit_flip_no_crc_fix", "patches": [(100, "ff")], "recrc": False},
        {"name": "version_0", "patches": [(4, "0000")], "recrc": True},
        {"name": "version_1_label_on_v2_body", "patches": [(4, "0100")], "recrc": True},
        {"name": "version_3", "patches": [(4, "0300")], "recrc": True},
        {"name": "length_field_wrong", "patches": [(6, "8f15")], "recrc": True},
        {"name": "profiles_3", "patches": [(9, "03")], "recrc": True},
        {"name": "layers_5", "patches": [(10, "05")], "recrc": True},
        {"name": "slots_77", "patches": [(11, "4d")], "recrc": True},
        {"name": "active_4", "patches": [(8, "04")], "recrc": True},
        {"name": "active_255", "patches": [(8, "ff")], "recrc": True},
        {"name": "mode_2", "patches": [(12 + 4, "02")], "recrc": True},
        {"name": "continuous_2", "patches": [(12 + 5, "02")], "recrc": True},
        {"name": "release_ge_press", "patches": [(12 + 2, "7800")], "recrc": True},          # release 120 = press 120
        {"name": "usage_01_layer0", "patches": [(code_off(0, 0, 0), "01")], "recrc": True},
        {"name": "usage_FF_layer3", "patches": [(code_off(0, 3, 5), "ff")], "recrc": True},
        {"name": "usage_FA_ok", "patches": [(code_off(0, 3, 5), "fa")], "recrc": True},
        {"name": "usage_E8_without_args", "patches": [(code_off(1, 2, 7), "e8")], "recrc": True},
        {"name": "usage_E8_toggle_ok", "patches": [(code_off(1, 2, 7), "e8"), (arg_off(1, 2, 7, 0), "02")], "recrc": True},
        {"name": "usage_E9_tap_hold_ok", "patches": [(code_off(3, 0, 63), "e9"), (arg_off(3, 0, 63, 0), "2c"), (arg_off(3, 0, 63, 1), "f0")], "recrc": True},
        {"name": "usage_EA_one_shot_ok", "patches": [(code_off(2, 1, 0), "ea"), (arg_off(2, 1, 0, 0), "e1")], "recrc": True},
        {"name": "usage_EB", "patches": [(code_off(0, 0, 0), "eb")], "recrc": True},
        {"name": "v2_blob_toggle_key_entry", "patches": [(code_off(0, 0, 0), "eb"), (arg_off(0, 0, 0, 0), "39")], "recrc": True},
        {"name": "v2_blob_dks_entry", "patches": [(code_off(0, 0, 0), "ed")], "recrc": True},
        {"name": "v2_blob_pad_button_entry", "patches": [(code_off(1, 1, 5), "ee"), (arg_off(1, 1, 5, 0), "03")], "recrc": True},
        {"name": "v2_blob_light_action_entry", "patches": [(code_off(0, 0, 0), "fd"), (arg_off(0, 0, 0, 0), "03")], "recrc": True},
        {"name": "v2_blob_light_action_zero_ok", "patches": [(code_off(0, 0, 0), "fd")], "recrc": True},
        {"name": "v2_blob_tap_hold_plain_hold", "patches": [(code_off(3, 0, 63), "e9"), (arg_off(3, 0, 63, 0), "2c"), (arg_off(3, 0, 63, 1), "05")], "recrc": True},
        {"name": "arg_on_plain_key", "patches": [(arg_off(0, 0, 0, 0), "01")], "recrc": True},
        {"name": "arg_on_plain_key_last", "patches": [(arg_off(3, 3, 75, 1), "01")], "recrc": True},
        {"name": "opts_default_tap_term_ok", "patches": [(opts_off(0), "3200")], "recrc": True},
        {"name": "opts_tap_term_29", "patches": [(opts_off(0), "1d00")], "recrc": True},
        {"name": "opts_tap_term_1001", "patches": [(opts_off(2), "e903")], "recrc": True},
        {"name": "opts_oneshot_zero_ok", "patches": [(opts_off(1) + 2, "0000")], "recrc": True},
        {"name": "opts_oneshot_99", "patches": [(opts_off(1) + 2, "6300")], "recrc": True},
        {"name": "opts_oneshot_10001", "patches": [(opts_off(1) + 2, "1127")], "recrc": True},
        {"name": "opts_policy_2_ok", "patches": [(opts_off(3) + 4, "02")], "recrc": True},
        {"name": "opts_policy_3", "patches": [(opts_off(3) + 4, "03")], "recrc": True},
        {"name": "opts_reserved_1", "patches": [(opts_off(0) + 5, "01")], "recrc": True},
        {"name": "truncated_by_one", "patches": [], "recrc": False, "truncate": SIZE - 1},
        {"name": "truncated_to_15", "patches": [], "recrc": False, "truncate": 15},
        {"name": "empty", "patches": [], "recrc": False, "truncate": 0},
        {"name": "one_byte_extra", "patches": [], "recrc": False, "extend": 1},
        {"name": "all_ff", "patches": [], "recrc": False, "fill": "ff"},
        {"name": "all_zero", "patches": [], "recrc": False, "fill": "00"},
    ]


def apply_mutation(base: bytes, m: dict) -> bytes:
    b = bytearray(base)
    if "fill" in m:
        b = bytearray(bytes.fromhex(m["fill"]) * len(base))
    for off, hx in m["patches"]:
        raw = bytes.fromhex(hx)
        b[off:off + len(raw)] = raw
    if m.get("recrc"):
        b[-4:] = zlib.crc32(bytes(b[:-4])).to_bytes(4, "little")
    if "truncate" in m:
        b = b[:m["truncate"]]
        if m.get("fixlen"):
            if len(b) >= 8:
                b[6:8] = len(b).to_bytes(2, "little")
            if len(b) >= 4:
                b[-4:] = zlib.crc32(bytes(b[:-4])).to_bytes(4, "little")
    if "extend" in m:
        b += b"\x00" * m["extend"]
    return bytes(b)


def v1_blobs(default_hex):
    """Version 1 blobs the firmware must still read: (name, bytes). The expected answers come from the firmware, as everywhere else."""
    base = to_v1(bytes.fromhex(default_hex))

    def patched(patches, recrc=True, size=None):
        b = bytearray(base)
        for off, hx in patches:
            raw = bytes.fromhex(hx)
            b[off:off + len(raw)] = raw
        if recrc:
            b[-4:] = zlib.crc32(bytes(b[:-4])).to_bytes(4, "little")
        return bytes(b)

    def v1_code_off(p, l, slot):
        return 12 + p * V1_PROFILE + SLOTS * CFG_BYTES + l * SLOTS + slot

    return [
        ("v1_default", base),
        ("v1_reserved_codes_FD_FE", patched([(v1_code_off(0, 1, 5), "fd"), (v1_code_off(2, 3, 70), "fe")])),
        ("v1_hold_and_transparent", patched([(v1_code_off(1, 0, 7), "fb"), (v1_code_off(1, 3, 7), "fc"), (v1_code_off(3, 2, 9), "ff")])),
        ("v1_active_3", patched([(8, "03")])),
        ("v1_code_E8_refused", patched([(v1_code_off(0, 0, 0), "e8")])),
        ("v1_code_EF_refused", patched([(v1_code_off(0, 0, 0), "ef")])),
        ("v1_code_01_refused", patched([(v1_code_off(0, 0, 0), "01")])),
        ("v1_bad_crc", patched([(100, "ff")], recrc=False)),
        ("v1_trailing_byte", base + b"\x00"),
        ("v1_truncated", base[:-1]),
        ("v1_version_label_2", patched([(4, "0200")])),
        ("v1_profiles_3", patched([(9, "03")])),
    ]


def build_spec(cases, probes, base_hex, muts, migs=(), entries=(), optsp=(), extra=()):
    lines = ["DEFAULT", "FLATDEFAULT", "TABLEDEFAULT", "USAGE", "CRC 313233343536373839"]
    for name, code, a0, a1 in entries:
        lines.append(f"ENTRY {name} {code} {a0} {a1}")
    for name, t, o, pol in optsp:
        lines.append(f"OPTS {name} {t} {o} {pol}")
    for name, blob in migs:
        lines.append(f"MIG {name} {blob.hex()}")
    for c in cases:
        lines.append(f"CASE {c['name']} {c['active']} {len(c['cfgs'])} {len(c['usages'])} {len(c['opts'])}")
        for p, slot, k in c["cfgs"]:
            lines.append(" ".join(str(x) for x in (p, slot, k["press"], k["release"], k["mode"], k["continuous"], k["rtDown"], k["rtUp"], k["topDead"], k["bottomDead"])))
        for u in c["usages"]:
            lines.append(" ".join(str(x) for x in u))
        for o in c["opts"]:
            lines.append(" ".join(str(x) for x in o))
    for pr in probes:
        k = pr["cfg"]
        lines.append(f"CFG {pr['name']} {k['press']} {k['release']} {k['mode']} {k['continuous']} {k['rtDown']} {k['rtUp']} {k['topDead']} {k['bottomDead']}")
    for m in muts:
        lines.append(f"DEC {m['name']} {m['hex'] or '-'}")
    lines += list(extra)
    return "\n".join(lines) + "\n"


# ======================================================================== settings format version 3 (M6 task P0)

V3_ENTRY_CODES = [0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xFD, 0xFE]
V3_ENTRY_V2_CODES = [0xE9, 0xFD]      # the version 2 tables are only dumped for the two codes whose rule changed


def ev(*events):
    """Four 2-bit DKS events packed into the events byte (0 none, 1 press, 2 release, 3 tap)."""
    return sum((e & 3) << (2 * i) for i, e in enumerate(events))


def dks(p, i, t_act=120, t_bot=0, flags=0, acts=((0x04, 1), (0, 0), (0, 0), (0, 0)), rsvd=0):
    return ["dks", p, i, t_act, t_bot, flags] + [x for a in acts for x in a] + [rsvd]


def v3_cases():
    """(name, active, [commands]) for the harness op CASEX. Commands (all numbers):
    cfg p slot press release mode cont rd ru td bd | ent p layer slot code a0 a1 | opt p term oneshot policy | dks p i ... | dksn p n |
    morph p i mask usage flags rsvd | morphn p n | socd p i a b mode flags | socdn p n | pad p i start end shape flags | padopt p dz circ smooth rsvd |
    dev pad filter rsvd. A record command makes the list at least i+1 long, the *n command sets the count itself."""
    ks = KEY_SLOTS
    out = []

    def case(name, cmds, active=0):
        out.append((name, active, cmds))

    # ---- accepted by the writer; the version byte of the answer shows which version the writer chose
    case("v3_none_default", [])
    case("v3_light_action_zero_stays_v2", [["ent", 0, 0, ks[3], 0xFD, 0, 0]])
    case("v3_nonkey_slot_data_stays_v2", [["cfg", 0, 15, 222, 150, 0, 0, 30, 30, 0, 0], ["ent", 0, 2, 15, 0x1D, 0, 0], ["cfg", 1, ks[0], 222, 150, 0, 0, 30, 30, 0, 0]])
    case("v3_nonkey_slot_data_dropped", [["cfg", 0, 15, 222, 150, 0, 0, 30, 30, 0, 0], ["ent", 0, 2, 15, 0x1D, 0, 0], ["cfg", 1, ks[0], 222, 150, 0, 0, 30, 30, 0, 0], ["dev", 1, 0, 0]])
    case("v3_key_configs_by_slot", [["cfg", 0, ks[16], 150, 100, 0, 0, 30, 30, 0, 0], ["cfg", 2, ks[60], 200, 150, 1, 1, 25, 30, 20, 40],
                                    ["cfg", 3, ks[30], 340, 10, 0, 0, 30, 30, 0, 0], ["ent", 1, 2, ks[45], 0x1D, 0, 0], ["dev", 1, 0, 0]])
    case("v3_dks_one", [dks(0, 0)])
    case("v3_dks_with_key", [dks(1, 0, 60, 200, 1, ((4, ev(1, 0, 0, 2)), (0x16, ev(0, 1, 2, 3)), (0, 0), (0xE7, ev(3, 3, 3, 3)))), ["ent", 1, 0, ks[3], 0xED, 0, 0]])
    case("v3_dks_eight_in_profile_3", [dks(3, i, 40 + 10 * i, 100 + 30 * i if i < 7 else 340) for i in range(8)] + [["ent", 3, 3, ks[60], 0xED, 7, 0]])
    case("v3_dks_depth_limits", [dks(0, 0, 40, 80), dks(0, 1, 270, 0), dks(0, 2, 300, 340), dks(0, 3, 40, 340)])
    case("v3_morph", [["morph", 2, 0, 0x03, 0x4C, 1, 0], ["morph", 2, 1, 0x40, 0x35, 0, 0], ["ent", 2, 1, ks[8], 0xEC, 0x2A, 0], ["ent", 2, 0, ks[9], 0xEC, 0x29, 1]])
    case("v3_morph_eight", [["morph", 0, i, 1 << i, 0x04 + i, i & 1, 0] for i in range(8)] + [["ent", 0, 3, ks[2], 0xEC, 0x05, 7]])
    case("v3_socd_two_pairs", [["socd", 1, 0, ks[1], ks[2], 1, 0], ["socd", 1, 1, ks[3], ks[4], 4, 1]])
    case("v3_socd_four_modes", [["socd", 0, i, ks[2 * i], ks[2 * i + 1], i, i & 1] for i in range(4)])
    case("v3_socd_last_slots", [["socd", 0, 0, ks[60], ks[59], 2, 0]])
    case("v3_pad_curve", [["pad", 0, 2, 4, 64, 2, 0]])
    case("v3_pad_curves_all", [["pad", 3, i, 4 + i, 8 + 20 * i, i, i & 1] for i in range(4)] + [["padopt", 3, 0, 0, 0, 0]])
    case("v3_pad_limits", [["pad", 2, 0, 4, 8, 0, 0], ["pad", 2, 1, 64, 68, 3, 1], ["padopt", 2, 50, 1, 3, 0]])
    case("v3_pad_option", [["padopt", 1, 10, 1, 2, 0]])
    case("v3_device_pad", [["dev", 1, 0, 0]])
    case("v3_device_filter", [["dev", 0, 2, 0]])
    case("v3_device_xinput", [["dev", 2, 1, 0]], active=3)
    case("v3_code_toggle_key", [["ent", 0, 1, ks[7], 0xEB, 0x39, 0]])
    case("v3_code_toggle_key_modifier", [["ent", 0, 1, ks[7], 0xEB, 0xE1, 0]])
    case("v3_code_pad_btn", [["ent", 0, 0, ks[8], 0xEE, 20, 0], ["ent", 0, 0, ks[9], 0xEE, 1, 68], ["ent", 2, 3, ks[10], 0xEE, 16, 1]])
    case("v3_code_pad_axis", [["ent", 0, 3, ks[8], 0xEF, 0x1B, 3], ["ent", 0, 0, ks[9], 0xEF, 0x05, 0], ["ent", 1, 1, ks[10], 0xEF, 0x14, 2], ["ent", 1, 1, ks[11], 0xEF, 0x0B, 1]])
    case("v3_code_light", [["ent", 2, 0, ks[20], 0xFD, 1, 0], ["ent", 2, 1, ks[21], 0xFD, 7, 0]])
    case("v3_code_tap_hold_key_hold", [["ent", 1, 0, ks[21], 0xE9, 0x2C, 0x05], ["ent", 1, 0, ks[22], 0xE9, 0xF5, 0x2B], ["ent", 1, 0, ks[23], 0xE9, 0x04, 0xE7]])
    case("v3_tap_hold_modifier_same_stays_v2", [["ent", 1, 0, ks[21], 0xE9, 0xE0, 0xE0]])
    case("v3_layer_options_and_active", [["opt", 0, 30, 0, 0], ["opt", 1, 1000, 10000, 2], ["opt", 2, 123, 100, 1], ["dks", 0, 0, 120, 0, 0, 4, 1, 0, 0, 0, 0, 0, 0, 0], ["ent", 0, 0, ks[4], 0xED, 0, 0]], active=2)
    everything = []
    for p in range(4):
        everything += [dks(p, i, 60 + 10 * i, 200 + 10 * i, 0, ((0x04 + i, ev(1, 0, 0, 2)), (0, 0), (0, 0), (0, 0))) for i in range(8)]
        everything += [["morph", p, i, 1 << i, 0x04 + i, 0, 0] for i in range(8)]
        everything += [["socd", p, i, ks[2 * i], ks[2 * i + 1], 1 + i, 0] for i in range(4)]
        everything += [["pad", p, 1, 4, 64, 3, 0], ["padopt", p, 7, 1, 1, 0]]
        everything += [["ent", p, 0, ks[0], 0xED, 7, 0], ["ent", p, 1, ks[3], 0xEC, 0x04, 7]]
    everything += [["ent", 0, 2, ks[10], 0xEF, 0x0B, 0], ["dev", 1, 2, 0]]
    case("v3_everything", everything, active=1)

    # ---- refused by the writer (REJECT): every one is a rule of the format
    case("reject_dks_key_without_record", [["ent", 0, 0, ks[3], 0xED, 0, 0]])
    case("reject_dks_key_index_past_records", [dks(0, 0), ["ent", 0, 0, ks[3], 0xED, 1, 0]])
    case("reject_dks_key_index_8", [dks(0, 0), ["ent", 0, 0, ks[3], 0xED, 8, 0]])
    case("reject_dks_key_second_arg", [dks(0, 0), ["ent", 0, 0, ks[3], 0xED, 0, 1]])
    case("reject_morph_key_without_record", [["ent", 0, 0, ks[3], 0xEC, 0x04, 0]])
    case("reject_morph_key_index_past_records", [["morph", 0, 0, 3, 0x4C, 0, 0], ["ent", 0, 0, ks[3], 0xEC, 0x04, 1]])
    case("reject_morph_key_usage_3", [["morph", 0, 0, 3, 0x4C, 0, 0], ["ent", 0, 0, ks[3], 0xEC, 0x03, 0]])
    case("reject_dks_count_9", [dks(0, 0), ["dksn", 0, 9]])
    case("reject_dks_t_act_39", [dks(0, 0, 39, 100)])
    case("reject_dks_t_act_271_default_bottom", [dks(0, 0, 271, 0)])
    case("reject_dks_gap_39", [dks(0, 0, 100, 139)])
    case("reject_dks_bottom_341", [dks(0, 0, 100, 341)])
    case("reject_dks_flags_2", [dks(0, 0, flags=2)])
    case("reject_dks_reserved", [dks(0, 0, rsvd=1)])
    case("reject_dks_usage_3", [dks(0, 0, acts=((3, 1), (0, 0), (0, 0), (0, 0)))])
    case("reject_dks_usage_E8", [dks(0, 0, acts=((4, 1), (0xE8, 1), (0, 0), (0, 0)))])
    case("reject_dks_events_without_usage", [dks(0, 0, acts=((4, 1), (0, 1), (0, 0), (0, 0)))])
    case("reject_morph_count_9", [["morph", 0, 0, 3, 0x4C, 0, 0], ["morphn", 0, 9]])
    case("reject_morph_mask_0", [["morph", 0, 0, 0, 0x4C, 0, 0]])
    case("reject_morph_usage_E8", [["morph", 0, 0, 1, 0xE8, 0, 0]])
    case("reject_morph_flags_2", [["morph", 0, 0, 1, 0x4C, 2, 0]])
    case("reject_morph_reserved", [["morph", 0, 0, 1, 0x4C, 0, 1]])
    case("reject_socd_count_5", [["socd", 0, 0, ks[0], ks[1], 1, 0], ["socdn", 0, 5]])
    case("reject_socd_same_slot", [["socd", 0, 0, ks[0], ks[0], 1, 0]])
    case("reject_socd_slot_without_key", [["socd", 0, 0, 15, ks[1], 1, 0]])
    case("reject_socd_slot_76", [["socd", 0, 0, ks[0], 76, 1, 0]])
    case("reject_socd_mode_5", [["socd", 0, 0, ks[0], ks[1], 5, 0]])
    case("reject_socd_flags_2", [["socd", 0, 0, ks[0], ks[1], 1, 2]])
    case("reject_socd_slot_in_two_pairs", [["socd", 0, 0, ks[0], ks[1], 1, 0], ["socd", 0, 1, ks[1], ks[2], 1, 0]])
    case("reject_socd_slot_in_two_pairs_b", [["socd", 0, 0, ks[0], ks[1], 1, 0], ["socd", 0, 1, ks[5], ks[0], 1, 0]])
    case("reject_pad_start_3", [["pad", 0, 0, 3, 64, 0, 0]])
    case("reject_pad_end_69", [["pad", 0, 0, 4, 69, 0, 0]])
    case("reject_pad_span_3", [["pad", 0, 0, 10, 13, 0, 0]])
    case("reject_pad_shape_4", [["pad", 0, 0, 4, 64, 4, 0]])
    case("reject_pad_flags_2", [["pad", 0, 0, 4, 64, 0, 2]])
    case("reject_pad_deadzone_51", [["padopt", 0, 51, 1, 0, 0]])
    case("reject_pad_circular_2", [["padopt", 0, 10, 2, 0, 0]])
    case("reject_pad_smooth_4", [["padopt", 0, 10, 1, 4, 0]])
    case("reject_pad_reserved", [["padopt", 0, 10, 1, 0, 1]])
    case("reject_device_pad_mode_3", [["dev", 3, 0, 0]])
    case("reject_device_filter_3", [["dev", 0, 3, 0]])
    case("reject_device_reserved", [["dev", 0, 0, 1]])
    case("reject_toggle_key_no_usage", [["ent", 0, 0, ks[3], 0xEB, 0, 0]])
    case("reject_toggle_key_usage_E8", [["ent", 0, 0, ks[3], 0xEB, 0xE8, 0]])
    case("reject_toggle_key_arg1", [["ent", 0, 0, ks[3], 0xEB, 0x04, 1]])
    case("reject_pad_btn_0", [["ent", 0, 0, ks[3], 0xEE, 0, 0]])
    case("reject_pad_btn_21", [["ent", 0, 0, ks[3], 0xEE, 21, 0]])
    case("reject_pad_btn_start_69", [["ent", 0, 0, ks[3], 0xEE, 1, 69]])
    case("reject_pad_axis_6", [["ent", 0, 0, ks[3], 0xEF, 6, 0]])
    case("reject_pad_axis_bit5", [["ent", 0, 0, ks[3], 0xEF, 0x20, 0]])
    case("reject_pad_axis_trigger_negative", [["ent", 0, 0, ks[3], 0xEF, 0x0C, 0]])
    case("reject_pad_axis_curve_4", [["ent", 0, 0, ks[3], 0xEF, 0x00, 4]])
    case("reject_light_action_8", [["ent", 0, 0, ks[3], 0xFD, 8, 0]])
    case("reject_light_arg1", [["ent", 0, 0, ks[3], 0xFD, 1, 1]])
    case("reject_reserved_FE_arg", [["ent", 0, 0, ks[3], 0xFE, 1, 0]])
    case("reject_tap_hold_same_key", [["ent", 0, 0, ks[21], 0xE9, 0x2C, 0x2C]])
    case("reject_tap_hold_hold_media", [["ent", 0, 0, ks[21], 0xE9, 0x2C, 0xF5]])
    case("reject_tap_hold_hold_E8", [["ent", 0, 0, ks[21], 0xE9, 0x2C, 0xE8]])
    return out


# ---- version 3 blobs: parsing and building in Python (independent of the firmware and of the JS codec)

def parse_v3(blob: bytes):
    """[(id, scope, payload_offset, payload_length)] of a version 3 blob."""
    p, out = 16, []
    for _ in range(blob[12]):
        length = int.from_bytes(blob[p + 2:p + 4], "little")
        out.append((blob[p], blob[p + 1], p + 4, length))
        p += 4 + length
    return out


def sec_off(blob: bytes, sid: int, scope: int) -> int:
    for i, s, off, ln in parse_v3(blob):
        if i == sid and s == scope:
            return off
    raise KeyError((sid, scope))


def build_v3(active: int, sections, n=None, header_patch=None, gap=b"") -> bytes:
    """A version 3 blob made of [(id, scope, payload)], with a correct length and CRC. n overrides the section count, gap is put before the CRC."""
    body = bytearray()
    for sid, scope, payload in sections:
        body += bytes([sid, scope]) + len(payload).to_bytes(2, "little") + payload
    out = bytearray(b"KMCS" + (3).to_bytes(2, "little") + b"\0\0" + bytes([active, 4, 4, KEYS, len(sections) if n is None else n, 0, 0, 0]))
    out += body + gap
    for off, val in (header_patch or []):
        out[off] = val
    out[6:8] = (len(out) + 4).to_bytes(2, "little")
    out += zlib.crc32(bytes(out)).to_bytes(4, "little")
    return bytes(out)


def sections_of(blob: bytes):
    return [(i, s, blob[off:off + ln]) for i, s, off, ln in parse_v3(blob)]


def v3_blobs(rich_hex: str, plain_hex: str):
    """(mutations of the rich blob [dicts like blob_mutations()], whole blobs [(name, bytes)]) for the firmware to read. The expected answers come from the firmware."""
    rich, plain = bytes.fromhex(rich_hex), bytes.fromhex(plain_hex)
    dks0, mor0, soc0, pad0 = (sec_off(rich, i, 0) for i in (0x10, 0x11, 0x12, 0x20))
    kc0, ly0, dev = sec_off(rich, 0x01, 0), sec_off(rich, 0x02, 0), sec_off(rich, 0x30, 0xFF)
    n = len(rich)
    M = []

    def mut(name, *patches, **kw):
        M.append({"name": name, "patches": [(o, f"{v:02x}" if isinstance(v, int) else v) for o, v in patches], "recrc": kw.pop("recrc", True), **kw})

    mut("v3_intact", recrc=False)
    mut("v3_bit_flip_no_crc_fix", (kc0 + 20, 0xFF), recrc=False)
    mut("v3_bad_magic", (0, 0x58))
    for v in (4, 2, 1, 0):
        mut(f"v3_version_{v}", (4, v))
    mut("v3_version_259", (5, 1))
    mut("v3_length_short", (6, (n & 0xFF) - 1))
    mut("v3_length_long_high", (7, (n >> 8) + 1))
    mut("v3_active_4", (8, 4))
    mut("v3_active_3_ok", (8, 3))
    for off, vals in ((9, (3, 5)), (10, (3, 5)), (11, (60, 62, 76))):
        for v in vals:
            mut(f"v3_header_{off}_{v}", (off, v))
    for v in (0, rich[12] - 1, rich[12] + 1, 64, 65, 255):
        mut(f"v3_section_count_{v}", (12, v))
    for off in (13, 14, 15):
        mut(f"v3_reserved_{off}", (off, 1))
    mut("v3_first_section_len_plus_1", (18, (854 & 0xFF) + 1))
    mut("v3_first_section_len_minus_1", (18, (854 & 0xFF) - 1))
    mut("v3_first_section_len_huge", (19, 0xFF))
    for v in (0x03, 0x40, 0x7F, 0x00, 0x80):
        mut(f"v3_first_section_id_{v:02x}", (16, v))
    for v in (4, 0xFE, 0xFF, 1):
        mut(f"v3_first_section_scope_{v:02x}", (17, v))
    mut("v3_dks_becomes_morph", (dks0 - 4, 0x11))
    mut("v3_dks_becomes_socd", (dks0 - 4, 0x12))
    mut("v3_dks_moves_to_profile_1", (dks0 - 3, 1))
    mut("v3_device_profile_scope_0", (dev - 3, 0))
    mut("v3_device_profile_scope_3", (dev - 3, 3))
    mut("v3_device_becomes_pad", (dev - 4, 0x20))
    mut("v3_device_len_15", (dev - 2, 15))
    mut("v3_device_len_17", (dev - 2, 17))
    for v in (128, 130, 0):
        mut(f"v3_dks_len_{v}", (dks0 - 2, v))
    for v in (31, 33):
        mut(f"v3_morph_len_{v}", (mor0 - 2, v))
    for v in (15, 20):
        mut(f"v3_socd_len_{v}", (soc0 - 2, v))
    for v in (23, 28):
        mut(f"v3_pad_len_{v}", (pad0 - 2, v))
    mut("v3_layers_len_737", (ly0 - 2, 0xE1))
    for v in (9, 7, 0):
        mut(f"v3_dks_count_{v}", (dks0, v))
    mut("v3_dks_retrigger_ok", (dks0 + 5, 1))
    mut("v3_dks_flags_2", (dks0 + 5, 2))
    mut("v3_dks_reserved_a", (dks0 + 6, 1))
    mut("v3_dks_reserved_c", (dks0 + 8, 1))
    mut("v3_dks_t_act_39", (dks0 + 1, 39))
    mut("v3_dks_t_act_316", (dks0 + 2, 1))
    mut("v3_dks_bottom_below_gap", (dks0 + 3, 99))
    mut("v3_dks_bottom_712", (dks0 + 4, 2))
    mut("v3_dks_usage_3", (dks0 + 9, 3))
    mut("v3_dks_usage_E8", (dks0 + 9, 0xE8))
    mut("v3_dks_usage_0_with_events", (dks0 + 9, 0))
    mut("v3_dks_events_without_usage", (dks0 + 12, 7))
    mut("v3_dks_events_cleared_ok", (dks0 + 10, 0))
    mut("v3_dks_last_record_events_without_usage", (dks0 + 1 + 16 * 7 + 8 + 7, 0xFF))
    mut("v3_morph_mask_0", (mor0, 0))
    mut("v3_morph_usage_3", (mor0 + 1, 3))
    mut("v3_morph_usage_E8", (mor0 + 1, 0xE8))
    mut("v3_morph_flags_2", (mor0 + 2, 2))
    mut("v3_morph_suppress_ok", (mor0 + 2, 1))
    mut("v3_morph_reserved", (mor0 + 3, 1))
    mut("v3_morph_last_reserved", (mor0 + 4 * 7 + 3, 0x80))
    mut("v3_socd_pair_of_one_slot", (soc0 + 1, rich[soc0]))
    mut("v3_socd_slot_15", (soc0, 15))
    mut("v3_socd_slot_76", (soc0, 76))
    mut("v3_socd_slot_255", (soc0 + 1, 255))
    mut("v3_socd_mode_5", (soc0 + 2, 5))
    mut("v3_socd_mode_0_ok", (soc0 + 2, 0))
    mut("v3_socd_flags_2", (soc0 + 3, 2))
    mut("v3_socd_layer0_flag_ok", (soc0 + 3, 1))
    mut("v3_socd_second_pair_shares_slot", (soc0 + 4, rich[soc0 + 1]))
    mut("v3_socd_last_pair_shares_slot", (soc0 + 13, rich[soc0]))
    mut("v3_pad_start_3", (pad0, 3))
    mut("v3_pad_end_69", (pad0 + 1, 69))
    mut("v3_pad_end_below_span", (pad0 + 1, 7))
    mut("v3_pad_shape_4", (pad0 + 2, 4))
    mut("v3_pad_shape_3_ok", (pad0 + 2, 3))
    mut("v3_pad_flags_2", (pad0 + 3, 2))
    mut("v3_pad_curve3_shape_200", (pad0 + 14, 200))
    mut("v3_pad_deadzone_51", (pad0 + 16, 51))
    mut("v3_pad_deadzone_50_ok", (pad0 + 16, 50))
    mut("v3_pad_circular_2", (pad0 + 17, 2))
    mut("v3_pad_smooth_4", (pad0 + 18, 4))
    mut("v3_pad_reserved", (pad0 + 19, 1))
    mut("v3_pad_last_reserved", (pad0 + 23, 1))
    mut("v3_device_pad_mode_3", (dev, 3))
    mut("v3_device_pad_mode_2_ok", (dev, 2))
    mut("v3_device_filter_3", (dev + 1, 3))
    mut("v3_device_reserved", (dev + 2, 1))
    mut("v3_device_last_reserved", (dev + 15, 1))
    mut("v3_key_mode_2", (kc0 + 4, 2))
    mut("v3_last_key_mode_255", (kc0 + 60 * 14 + 4, 0xFF))
    mut("v3_key_release_eq_press", (kc0 + 2, 120))
    mut("v3_key_press_over_travel", (kc0 + 1, 2))
    mut("v3_opts_reserved", (ly0 + 5, 1))
    mut("v3_opts_policy_3", (ly0 + 4, 3))
    mut("v3_opts_tap_term_29", (ly0, 29))
    mut("v3_opts_oneshot_12496", (ly0 + 3, 0x30))
    mut("v3_code_1", (ly0 + 6, 1))
    mut("v3_code_3_last", (ly0 + 6 + 3 * 61 + 60, 3))
    mut("v3_code_E8_without_args", (ly0 + 6 + 5, 0xE8))
    mut("v3_code_EE_without_args", (ly0 + 6 + 5, 0xEE))
    mut("v3_code_EB_without_args", (ly0 + 6 + 5, 0xEB))
    mut("v3_code_FD_zero_args_ok", (ly0 + 6 + 5, 0xFD))
    mut("v3_arg_on_plain_key", (ly0 + 6 + 244 + 2 * 5, 1))
    mut("v3_arg1_on_plain_key", (ly0 + 6 + 244 + 2 * 5 + 1, 1))
    dks_arg = ly0 + 6 + 244
    morph_arg1 = ly0 + 6 + 244 + 2 * (61 + 3) + 1
    axis_arg = ly0 + 6 + 244 + 2 * (2 * 61 + 10)
    for v in (0, 8, 255):
        mut(f"v3_dks_index_{v}", (dks_arg, v))
    mut("v3_dks_second_arg", (dks_arg + 1, 1))
    for v in (0, 8):
        mut(f"v3_morph_index_{v}", (morph_arg1, v))
    mut("v3_morph_usage_arg_3", (morph_arg1 - 1, 3))
    for v in (0x15, 0x0C, 6, 0x20):
        mut(f"v3_axis_arg_{v:02x}", (axis_arg, v))
    mut("v3_axis_curve_4", (axis_arg + 1, 4))
    for cut in (4, 15, 16, 19, 20, 21, 100, 1000, n - 5):
        mut(f"v3_truncate_{cut}", recrc=True, truncate=cut, fixlen=True)
    mut("v3_truncate_by_one_no_fix", recrc=False, truncate=n - 1)
    mut("v3_extra_byte", recrc=False, extend=1)
    mut("v3_all_ff", recrc=False, fill="ff")
    mut("v3_all_zero", recrc=False, fill="00")

    # whole blobs made from the sections of the rich / plain blob
    secs = sections_of(rich)
    prof = {(i, s): pl for i, s, pl in secs}
    core = [(i, s, pl) for i, s, pl in sections_of(plain) if i in (0x01, 0x02)]
    B = []
    B.append(("v3_reversed_order", build_v3(1, [(i, s, pl) for i, s, pl in reversed(secs)])))
    B.append(("v3_device_first", build_v3(1, [secs[-1]] + secs[:-1])))
    for p in (0, 3):
        for k in (0x01, 0x02):
            B.append((f"v3_missing_{k:02x}_profile_{p}", build_v3(1, [x for x in secs if (x[0], x[1]) != (k, p)])))
    B.append(("v3_only_required", build_v3(2, core)))
    B.append(("v3_header_only", build_v3(0, [])))
    B.append(("v3_empty_lists", build_v3(0, core + [(0x10, 2, b"\x00"), (0x11, 1, b""), (0x12, 3, b"")])))
    dpad = bytes([4, 64, 0, 0] * 4 + [10, 1, 0, 0, 0, 0, 0, 0])
    B.append(("v3_default_pad_and_device", build_v3(0, core + [(0x20, 0, dpad), (0x30, 0xFF, bytes(16))])))
    B.append(("v3_changed_pad", build_v3(0, core + [(0x20, 3, bytes([4, 64, 3, 0] + list(dpad[4:])))])))
    junk = bytes((i * 7 + 1) & 0xFF for i in range(300))
    B.append(("v3_unknown_optional_front", build_v3(0, [(0x80, 0xFF, junk)] + core)))
    B.append(("v3_unknown_optional_between", build_v3(0, [core[0], (0x81, 0, junk[:20]), core[1]] + core[2:] + [(0xFF, 0xFF, b"")])))
    B.append(("v3_unknown_optional_per_profile", build_v3(0, [x for c in range(4) for x in (core[2 * c], (0x82 + c, c, junk[:c * 20]), core[2 * c + 1])])))
    for v, sc in ((0x03, 0xFF), (0x7F, 0xFF), (0x31, 0xFF), (0x13, 0), (0x40, 1)):
        B.append((f"v3_unknown_required_{v:02x}", build_v3(0, core + [(v, sc, junk[:4])])))
    B.append(("v3_duplicate_optional", build_v3(0, core + [(0x90, 0xFF, junk[:3]), (0x90, 0xFF, junk[:3])])))
    B.append(("v3_optional_scope_4", build_v3(0, core + [(0x90, 4, junk[:3])])))
    B.append(("v3_optional_scope_7f", build_v3(0, core + [(0x90, 0x7F, junk[:3])])))
    B.append(("v3_duplicate_required", build_v3(0, core + [core[0]])))
    for total in (63, 64, 65):
        B.append((f"v3_{total}_sections", build_v3(0, core + [(0x80 + i, 0xFF, b"") for i in range(total - 8)])))
    base_len = len(build_v3(0, core))
    for over in (0, 1):
        room = V3_MAX + over - base_len - 4
        B.append((f"v3_size_{V3_MAX + over}", build_v3(0, core + [(0xA0, 0xFF, bytes([0x5A]) * room)])))
    B.append(("v3_gap_before_crc", build_v3(0, core, gap=b"\0\0")))
    B.append(("v3_section_count_too_big_for_data", build_v3(0, core, n=9)))
    B.append(("v3_section_count_too_small_for_data", build_v3(0, core, n=7)))
    B.append(("v3_required_dks_empty_record", build_v3(0, core + [(0x10, 0, bytes([1]) + bytes(16))])))
    good_dks = bytes([1]) + prof[(0x10, 0)][1:17]
    B.append(("v3_dks_one_record_ok", build_v3(0, core + [(0x10, 0, good_dks)])))
    B.append(("v3_dks_trailing_byte", build_v3(0, core + [(0x10, 0, good_dks + b"\0")])))
    B.append(("v3_morph_trailing_byte", build_v3(0, core + [(0x11, 0, prof[(0x11, 0)][:4] + b"\0")])))
    B.append(("v3_socd_trailing_byte", build_v3(0, core + [(0x12, 0, prof[(0x12, 0)][:4] + b"\0")])))
    B.append(("v3_pad_trailing_byte", build_v3(0, core + [(0x20, 0, prof[(0x20, 0)] + b"\0")])))
    B.append(("v3_device_trailing_byte", build_v3(0, core + [(0x30, 0xFF, prof[(0x30, 0xFF)] + b"\0")])))
    B.append(("v3_device_short", build_v3(0, core + [(0x30, 0xFF, prof[(0x30, 0xFF)][:15])])))
    B.append(("v3_keycfg_trailing_byte", build_v3(0, [(core[0][0], core[0][1], core[0][2] + b"\0")] + core[1:])))
    B.append(("v3_layers_trailing_byte", build_v3(0, [core[0], (core[1][0], core[1][1], core[1][2] + b"\0")] + core[2:])))
    B.append(("v3_keycfg_short", build_v3(0, [(core[0][0], core[0][1], core[0][2][:-1])] + core[1:])))
    return M, B


def casex_lines(cases3):
    lines = []
    for name, active, cmds in cases3:
        lines.append(f"CASEX {name} {active}")
        lines += [" ".join(str(x) for x in c) for c in cmds]
        lines.append("END")
    return lines


def parse_answers(stdout: str) -> dict:
    answers = {}
    for line in stdout.splitlines():
        parts = line.split(" ")
        if parts[0] == "R":
            answers[parts[1]] = parts[2] if len(parts) > 2 else ""
    return answers


def compile_harness(tmp: Path) -> Path:
    e = dict(os.environ)
    ucrt = Path("C:/msys64/ucrt64/bin")
    if os.name == "nt" and ucrt.exists():
        e["PATH"] = str(ucrt) + os.pathsep + e["PATH"]
    cc = shutil.which("gcc", path=e["PATH"])
    if not cc:
        sys.exit("gcc not found (on Windows install MSYS2 ucrt64)")
    src = tmp / "harness.c"
    src.write_text(HARNESS)
    exe = tmp / ("harness.exe" if os.name == "nt" else "harness")
    cmd = [cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-I" + str(FW / "core/include"), str(src),
           str(FW / "core/src/km_settings.c"), str(FW / "core/src/km_keymap.c"), str(FW / "core/src/km_engine.c"),
           str(FW / "core/src/km_depth_model.c"), "-o", str(exe)]
    r = subprocess.run(cmd, env=e)
    if r.returncode:
        sys.exit("harness build failed")
    return exe, e


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="do not write; fail if the committed vector file differs")
    args = ap.parse_args()

    cases, probes, mutations = scenarios(), cfg_probes(), blob_mutations()
    with tempfile.TemporaryDirectory(prefix="km_settings_vec_") as td:
        tmp = Path(td)
        exe, env = compile_harness(tmp)
        # pass 1: the default blob (needed as the base for the mutations)
        cases3 = v3_cases()
        spec = tmp / "spec1.txt"
        spec.write_text("\n".join(["DEFAULT", "DEFAULT3", "KEYSLOTS"] + casex_lines(cases3)) + "\n")
        res1 = subprocess.run([str(exe), str(spec)], env=env, capture_output=True, text=True)
        if res1.returncode:
            sys.exit(f"harness exited with {res1.returncode} in pass 1\n{res1.stderr}")
        ans1 = parse_answers(res1.stdout)
        default_hex = ans1["DEFAULT"]
        base = bytes.fromhex(default_hex)
        assert len(base) == SIZE, len(base)
        for m in mutations:
            m["hex"] = apply_mutation(base, m).hex()
        spec = tmp / "spec2.txt"
        migs = v1_blobs(default_hex)
        v3_muts, v3_built = v3_blobs(ans1["v3_everything"], ans1["DEFAULT3"])
        rich_bytes = bytes.fromhex(ans1["v3_everything"])
        for m in v3_muts:
            m["hex"] = apply_mutation(rich_bytes, m).hex()
        extra = [f"MIG {m['name']} {m['hex']}" for m in v3_muts] + [f"MIG {n} {b.hex()}" for n, b in v3_built]
        extra += [f"ENTRYMAP {c}" for c in V3_ENTRY_CODES]
        spec.write_text(build_spec(cases, probes, default_hex, mutations, migs, entry_probes(), opts_probes(), extra))
        res = subprocess.run([str(exe), str(spec)], env=env, capture_output=True, text=True)
        if res.returncode:
            sys.exit(f"harness exited with {res.returncode}\n{res.stderr}")

    answers = parse_answers(res.stdout)

    vec = {
        "about": "Generated by firmware/tools/gen_settings_vector.py from the real km_settings.c / km_keymap.c. Do not edit by hand.",
        "size": SIZE,
        "crc_check_123456789": answers["CRC"],
        "default": answers["DEFAULT"],
        "default_flattened": answers["FLATDEFAULT"],
        "default_table": answers["TABLEDEFAULT"],
        "usage_valid": [int(ch) for ch in answers["USAGE"]],
        "v1_size": V1_SIZE,
        "entry_probes": [{"name": n, "code": c, "arg0": a0, "arg1": a1, "valid": answers[n][0] == "1", "valid_v2": answers[n][1] == "1"} for n, c, a0, a1 in entry_probes()],
        "opts_probes": [{"name": n, "tapTerm": t, "oneshot": o, "policy": pol, "valid": answers[n] == "1"} for n, t, o, pol in opts_probes()],
        "migrations": [{"name": n, "v1": b.hex(), "migrated": None if answers[n] == "REJECT" else answers[n]} for n, b in migs],
        "cases": [],
        "cfg_probes": [],
        "mutations": [],
    }
    vec["v3"] = {
        "about": "Settings format version 3 (M6 task P0). cases: commands for the raw model, the answer is what the real writer produced (the version byte shows the version it chose). "
                 "mutations: patches of the 'v3_everything' blob; blobs: whole blobs; for both, reencoded is what the real reader+writer made of them (null = refused).",
        "max_size": V3_MAX, "keys": KEYS,
        "key_slots_flags": ans1["KEYSLOTS"],
        "default_v3": ans1["DEFAULT3"],
        "cases": [{"name": n, "active": a, "cmds": cm, "encoded": None if ans1[n] == "REJECT" else ans1[n]} for n, a, cm in cases3],
        "entry_maps_v3": {str(c): answers[f"EMAP0_{c}"] for c in V3_ENTRY_CODES},
        "entry_maps_v2": {str(c): answers[f"EMAP1_{c}"] for c in V3_ENTRY_V2_CODES},
        "mutations": [{"name": m["name"], "patches": [[o, h] for o, h in m["patches"]], "recrc": m["recrc"],
                       **{k: m[k] for k in ("truncate", "fixlen", "extend", "fill") if k in m},
                       "reencoded": None if answers[m["name"]] == "REJECT" else answers[m["name"]]} for m in v3_muts],
        "blobs": [{"name": n, "blob": b.hex(), "reencoded": None if answers[n] == "REJECT" else answers[n]} for n, b in v3_built],
    }
    for c in cases:
        hx = answers[c["name"]]
        vec["cases"].append({**c, "encoded": None if hx == "REJECT" else hx})
    for pr in probes:
        vec["cfg_probes"].append({**pr, "valid": answers[pr["name"]] == "1"})
    for m in mutations:
        vec["mutations"].append({
            "name": m["name"], "patches": [[o, h] for o, h in m["patches"]], "recrc": m["recrc"],
            **{k: m[k] for k in ("truncate", "extend", "fill") if k in m},
            "accepted": answers[m["name"]] == "ACCEPT",
        })

    text = json.dumps(vec, indent=1) + "\n"
    if args.check:
        if not OUT.exists() or OUT.read_text() != text:
            print("STALE:", OUT, "differs from what the firmware code produces now; run without --check", file=sys.stderr)
            return 1
        print("up to date:", OUT)
        return 0
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text)
    print("wrote", OUT, f"({len(text) // 1024} KB; {len(cases)} cases, {len(probes)} config probes, {len(mutations)} corrupted blobs, {len(migs)} version 1 blobs, "
          f"{len(cases3)} version 3 cases, {len(v3_muts)} corrupted + {len(v3_built)} built version 3 blobs)")
    print("firmware default blob:", default_hex[:32], "...", " ".join(f"{k}={v}" for k, v in
          [("accepted", sum(m['accepted'] for m in vec['mutations'])), ("refused", sum(not m['accepted'] for m in vec['mutations']))]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
