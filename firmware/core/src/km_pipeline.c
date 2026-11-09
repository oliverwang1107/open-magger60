#include "km/km_pipeline.h"

#include <string.h>

static bool bit_get(const uint32_t w[3], unsigned slot)
{
    return slot < KM_KEYMAP_SLOTS && ((w[slot >> 5] >> (slot & 31u)) & 1u) != 0u;
}

static void bit_set(uint32_t w[3], unsigned slot)
{
    if (slot < KM_KEYMAP_SLOTS) {
        w[slot >> 5] |= 1u << (slot & 31u);
    }
}

static void bits_from_list(uint32_t w[3], const uint8_t *list, unsigned n)
{
    w[0] = w[1] = w[2] = 0;
    for (unsigned i = 0; i < n; i++) {
        bit_set(w, list[i]);
    }
}

/* Is any word non-zero? */
static bool bits_any(const uint32_t w[3])
{
    return (w[0] | w[1] | w[2]) != 0u;
}

static bool is_pad_code(uint8_t code)
{
    return code == KM_CODE_PAD_BTN || code == KM_CODE_PAD_AXIS;
}

void km_pipeline_init(km_pipeline_t *p)
{
    memset(p, 0, sizeof *p);
    km_socd_init(&p->socd);
    km_dks_init(&p->dks);
    km_pad_init(&p->pad);
}

void km_pipeline_reset(km_pipeline_t *p)
{
    km_layers_reset();
    km_socd_reset(&p->socd);
    km_dks_reset(&p->dks);
    km_pad_reset(&p->pad);
    p->light_down[0] = p->light_down[1] = p->light_down[2] = 0;
    p->holdover[0] = p->holdover[1] = p->holdover[2] = 0;
    p->prev_layer = 0;
    p->pad_poke = true;
    p->resets++;
}

void km_pipeline_hold_over(km_pipeline_t *p, const uint8_t *down, unsigned n)
{
    bits_from_list(p->holdover, down, n > KM_ENGINE_MAX_DOWN ? KM_ENGINE_MAX_DOWN : n);
}

void km_pipeline_set_profile(km_pipeline_t *p, const km_profile_t *prof, const uint8_t *down, unsigned n)
{
    (void)km_layers_set_table(prof->layer_usage, prof->layer_arg, &prof->layer_opts);
    km_layers_set_morph(prof->morph, prof->morph_n);
    (void)km_socd_set_pairs(&p->socd, prof->socd, prof->socd_n);
    (void)km_dks_set_records(&p->dks, prof->dks, prof->dks_n);
    (void)km_pad_set_config(&p->pad, prof->pad_curve, &prof->pad_opts);
    p->has_dks = p->has_pad = p->has_light = false;
    for (unsigned l = 0; l < KM_LAYERS; l++) {
        for (unsigned s = 0; s < KM_KEYMAP_SLOTS; s++) {
            const uint8_t c = prof->layer_usage[l][s];
            if (c == KM_CODE_DKS) {
                p->has_dks = true;
            } else if (is_pad_code(c)) {
                p->has_pad = true;
            } else if (c == KM_CODE_LIGHT) {
                p->has_light = true;
            }
        }
    }
    p->epoch++;
    p->pad_poke = true;
    p->light_down[0] = p->light_down[1] = p->light_down[2] = 0;
    p->prev_layer = 0;
    bits_from_list(p->holdover, down, n > KM_ENGINE_MAX_DOWN ? KM_ENGINE_MAX_DOWN : n);
}

bool km_pipeline_slot_cfg(const km_profile_t *prof, unsigned slot, km_engine_key_cfg_t *out)
{
    *out = prof->key[slot];
    if (slot >= KM_KEYMAP_SLOTS) {
        return false;
    }
    for (unsigned l = 0; l < KM_LAYERS; l++) {
        const uint8_t code = prof->layer_usage[l][slot];
        if (code == KM_CODE_DKS) {
            const unsigned rec = prof->layer_arg[l][slot][0];
            uint16_t press = 0, release = 0;
            if (rec < prof->dks_n && km_dks_engine_points(&prof->dks[rec], &press, &release)) {
                memset(out, 0, sizeof *out);
                out->mode = KM_ENGINE_TRIG_FIXED;
                out->press_mm100 = press;
                out->release_mm100 = release;
                return true;
            }
        } else if (is_pad_code(code)) {
            memset(out, 0, sizeof *out);
            out->mode = KM_ENGINE_TRIG_FIXED;
            out->press_mm100 = KM_PAD_ENGINE_PRESS_MM100;
            out->release_mm100 = KM_PAD_ENGINE_RELEASE_MM100;
            return true;
        }
    }
    return false;
}

static void tick_clock(km_pipeline_t *p, uint32_t now, uint32_t cpm)
{
    if (cpm == 0u) {
        p->ms++;                                    /* no clock: one millisecond per pass */
        return;
    }
    if (!p->clk_init) {
        p->clk_prev = now;
        p->clk_init = true;
    }
    p->clk_acc += (uint32_t)(now - p->clk_prev);
    p->clk_prev = now;
    const uint32_t d = p->clk_acc / cpm;
    p->clk_acc -= d * cpm;
    p->ms += d;
}

static void out_empty(km_pipe_out_t *out)
{
    memset(&out->lo, 0, sizeof out->lo);
    memset(&out->pad, 0, sizeof out->pad);
    out->pad_recomputed = false;
    out->light_n = 0;
    out->n_after_socd = 0;
}

void km_pipeline_run(km_pipeline_t *p, const km_pipe_in_t *in, km_pipe_out_t *out)
{
    p->passes++;
    tick_clock(p, in->now_cycles, in->cycles_per_ms);
    out_empty(out);

    if (!in->allowed) {
        /* a fault: every module to rest ONCE, then nothing is fed to them until the fault is over */
        if (!p->blocked) {
            km_pipeline_reset(p);
            p->blocked = true;
        }
        p->blocked_passes++;
        return;
    }
    unsigned nd = in->n > KM_ENGINE_MAX_DOWN ? KM_ENGINE_MAX_DOWN : in->n;
    if (p->blocked) {
        p->blocked = false;                         /* the fault is over: keys that are still down are held over for DKS / gamepad / light */
        bits_from_list(p->holdover, in->down, nd);
        p->epoch++;
        p->pad_poke = true;
    }

    nd = km_socd_filter(&p->socd, in->down, nd, p->prev_layer, in->depth, in->ctx, p->tmp);
    out->n_after_socd = (uint8_t)nd;
    km_layers_update_cycles(p->tmp, nd, in->now_cycles, in->cycles_per_ms, &out->lo);
    p->prev_layer = out->lo.layer;

    if (p->has_dks || p->has_pad || p->has_light) {
        km_dks_press_t dks[KM_DKS_KEYS];
        km_pad_key_t pad[KM_PAD_MAX_KEYS];
        unsigned n_dks = 0, n_pad = 0;
        uint32_t now_down[3];

        if (bits_any(p->holdover) || bits_any(p->light_down)) {
            bits_from_list(now_down, p->tmp, nd);
            for (unsigned w = 0; w < 3u; w++) {
                p->holdover[w] &= now_down[w];       /* a held-over key that has been released is a normal key again */
                p->light_down[w] &= now_down[w];
            }
        }
        for (unsigned i = 0; i < nd; i++) {
            const unsigned slot = p->tmp[i];
            km_layers_entry_t e;
            p->lookups++;
            if (!km_layers_locked_entry(slot, &e)) {
                continue;
            }
            if (e.code == KM_CODE_DKS) {
                if (p->has_dks && n_dks < KM_DKS_KEYS && e.arg0 < KM_DKS_MAX && !bit_get(p->holdover, slot)) {
                    dks[n_dks].slot = (uint8_t)slot;
                    dks[n_dks].rec = e.arg0;
                    n_dks++;
                }
            } else if (is_pad_code(e.code)) {
                if (n_pad < KM_PAD_MAX_KEYS) {
                    pad[n_pad].slot = (uint8_t)slot;
                    pad[n_pad].code = e.code;
                    pad[n_pad].arg0 = e.arg0;
                    pad[n_pad].arg1 = e.arg1;
                    n_pad++;
                }
            } else if (e.code == KM_CODE_LIGHT && e.arg0 >= 1u && e.arg0 <= KM_LIGHT_ACT_LAST) {
                if (!bit_get(p->light_down, slot)) {
                    bit_set(p->light_down, slot);
                    if (!bit_get(p->holdover, slot)) {
                        if (out->light_n < KM_PIPE_MAX_LIGHT) {
                            out->light[out->light_n++] = e.arg0;
                            p->light_actions++;
                        } else {
                            p->light_dropped++;
                        }
                    }
                }
            }
        }
        if (n_dks > p->dks_keys_max) {
            p->dks_keys_max = n_dks;
        }
        if (n_pad > p->pad_keys_max) {
            p->pad_keys_max = n_pad;
        }

        if (n_dks != 0u || km_dks_busy(&p->dks)) {
            km_dks_out_t d;
            (void)km_dks_update_cycles(&p->dks, dks, n_dks, in->now_cycles, in->cycles_per_ms, in->depth, in->ctx, &d);
            out->lo.kbd_n = (uint8_t)km_dks_merge(out->lo.kbd, out->lo.kbd_n, KM_KEYMAP_SLOTS, &d);
        }
        /* km_pad marks the keys it finds down at its FIRST update after a reset as held over: that update must be the pass right after the reset */
        if (n_pad != 0u || p->pad_poke || !km_pad_report_neutral(&p->pad.cur) || bits_any(p->pad.blocked)) {   /* a held-over key must be seen going up */
            p->pad_poke = false;
            const km_pad_in_t pin = { pad, n_pad, true, p->epoch * 8u + out->lo.layer, p->ms, in->depth, in->ctx };
            out->pad_recomputed = km_pad_update(&p->pad, &pin, &out->pad);
        }
    }
    /* no DKS / gamepad / light-action entry in the tables: set_profile() left both modules at rest, there is nothing to ask them */
}
