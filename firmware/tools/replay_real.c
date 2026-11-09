#include <stdlib.h>
/* replay_real.c - run real KMTR captures (tools/magger60_noise.py) through the REAL engine in every filter mode (FILTER_NOTES.md section 12).
 *   gcc -O2 -DKM_ENGINE_FILTER_MODES=1 -Ifirmware/core/include -Ifirmware/tests firmware/tools/replay_real.c firmware/tests/filter_lab.c \
 *       firmware/tests/filter_lab_engine.c firmware/core/src/{km_engine,km_keymap,km_depth_model,km_scan}.c firmware/core/gen/km_slot_data.c -o replay_real
 *   replay_real rt_mm100 file.kmtr [more files]     prints, per filter mode: columns replayed, frames, events (zero is what an idle capture must give), sd of f */
#include <stdio.h>
#include <string.h>

#include "filter_lab.h"
#include <km/km_keymap.h>

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: replay_real rt_mm100 file.kmtr...\n"); return 2; }
    const unsigned rt = (unsigned)atoi(argv[1]);
    for (unsigned mode = 0; mode <= KM_ENGINE_FMODE_LAST; mode++) {
        unsigned long frames = 0; unsigned cols = 0, events = 0; double sd2 = 0; unsigned nsd = 0;
        for (int a = 2; a < argc; a++) {
            lab_trace_t t;
            if (lab_trace_load(&t, argv[a]) != 0) { fprintf(stderr, "cannot load %s\n", argv[a]); continue; }
            lab_run_t r;
            memset(&r, 0, sizeof r);
            r.trace = &t; r.n_frames = t.n_frames; r.n_keys = t.n_cols < LAB_MAX_KEYS ? t.n_cols : LAB_MAX_KEYS;
            for (unsigned k = 0; k < r.n_keys; k++) {
                r.slot[k] = t.slots[k];
                km_engine_key_cfg_default(&r.cfg[k]);
                r.cfg[k].mode = KM_ENGINE_TRIG_RAPID; r.cfg[k].rt_down_mm100 = r.cfg[k].rt_up_mm100 = (uint16_t)rt;
                r.cfg[k].top_dead_mm100 = LAB_TD; r.cfg[k].continuous = 1; r.cfg[k].press_mm100 = 0;
            }
            r.use_real_engine = 1; r.real_filter_mode = mode; r.stat_from = (unsigned long)(0.05 * t.n_frames);
            lab_result_t res;
            if (lab_run(&r, &res) == 0) {
                events += res.n_ev; frames += t.n_frames; cols += r.n_keys;
                for (unsigned k = 0; k < r.n_keys; k++) { sd2 += lab_stat_sd(res.key[k].n, res.key[k].sum_f, res.key[k].sumsq_f); nsd++; }
                lab_result_free(&res);
            }
            lab_trace_free(&t);
        }
        printf("mode %u rt %u: %u columns, %lu frames, events %u, mean sd f %.2f\n", mode, rt, cols, frames, events, nsd ? sd2 / nsd : 0.0);
    }
    return 0;
}
