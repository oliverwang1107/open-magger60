/*
 * M4 image (M4_PLAN.md hardware ladder K2 / K3): the S3 scan feeding the key engine, which feeds the keyboard reports.
 *
 *   scan   GTIM1 -> ADC1..4 -> DMA, four mux phases per frame (adc_scan.c, proven in S3), 16-point sums per slot (km_scan)
 *   engine km_engine: boot calibration on a quiet stretch (retried until no key is held), then one update per frame
 *   output NKRO bitmap on IF2 (report id 1); the boot report on IF0 only while the host selected the boot protocol.
 *          IF0 and IF2 are both keyboards: exactly one of them is used at any time, never both.
 *
 * Variants (one source, two targets):
 *   km_k2  KM_M4_ALL_KEYS = 0: only ESC may produce a report. Every other key is seen by the engine and shown on the scope
 *          channel but never sent. This is the safety gate for the first typing test.
 *   km_k3  KM_M4_ALL_KEYS = 1: every key of the keymap.
 *
 * Kept from the earlier images: vendor cmd 3 ("KMFW-K2" / "KMFW-K3"), the read-only scope channel, and the
 * enter-bootloader command (the only flash write, see flash_boot.c).
 *
 * Scope snapshots, one every 10 ms in rotation:
 *   tag 0x0003  the 76 sixteen-point sums (7 of 10)
 *   tag 0x0100  ADC diagnostics, same layout as S3 (1 of 10)
 *   tag 0x0200  engine diagnostics (24 values, 1 of 10):
 *     [0] calibration result (km_engine_cal_result_t, 0xFFFF = not finished yet)  [1] calibration attempts
 *     [2] keys down  [3..4] glitch count (lo, hi)  [5] last engine update in CPU cycles  [6] longest one
 *     [7..8] reports sent (lo, hi)  [9] 1 = boot protocol selected on IF0  [10..11] key-downs held back by the ESC-only gate
 *     [12..17] first six pressed slots (0xFF = none)  [18] 1 = engine armed  [19..20] frames evaluated by the engine (lo, hi)
 *     [21..23] 0
 *   tag 0x0201  the 76 key depths in 0.01 mm
 */
#include <stdbool.h>
#include <string.h>

#include "adc_scan.h"
#include "flash_boot.h"
#include "km/km_engine.h"
#include "km/km_keymap.h"
#include "km/km_report.h"
#include "km/km_scan.h"
#include "km/km_scope.h"
#include "km/km_vendor.h"
#include "n32h48x_regs.h"
#include "sense_hw.h"
#include "usb_hs.h"

#ifndef KM_M4_ALL_KEYS
#define KM_M4_ALL_KEYS 0
#endif
#ifndef KM_M4_NAME
#define KM_M4_NAME "KMFW-K2"
#endif

#define PUBLISH_CYCLES (10u * 1000u * 240u)      /* 10 ms */
#define WARMUP_FRAMES  32u                       /* the 16-point sums are full after 16 frames */
#define CAL_FRAMES     64u                       /* length of the quiet stretch fed to the calibration */
#define USAGE_ESC      0x29u
#define NKRO_REPORT_LEN (1u + KM_REPORT_NKRO_LEN)

static const km_vendor_info_t k_info = { 0x0104, __DATE__ "," __TIME__ KM_M4_NAME };

static volatile bool g_enter_boot;

static bool vendor(const uint8_t in[64], uint8_t out[64])
{
    if (km_vendor_is_enter_boot_request(in)) {
        g_enter_boot = true;
        return false;
    }
    return km_vendor_handle(&k_info, in, out);
}

static uint16_t clamp16(uint32_t v)
{
    return v > 0xFFFFu ? 0xFFFFu : (uint16_t)v;
}

static km_scan_t g_scan;
static km_engine_t g_engine;
static uint16_t g_counts[KM_ENGINE_SLOTS];

/* ---- reports ---- */

static uint8_t g_last_boot[KM_REPORT_BOOT_LEN];     /* what the host has been told last, per path */
static uint8_t g_last_nkro[NKRO_REPORT_LEN];
static uint32_t g_reports_sent;
static uint32_t g_held_back;
static bool g_was_configured;
static bool g_was_boot_protocol;

static unsigned esc_slot(void)
{
    for (unsigned s = 0; s < KM_ENGINE_SLOTS; s++) {
        if (km_keymap_usage(0, s) == USAGE_ESC) {
            return s;
        }
    }
    return KM_ENGINE_SLOTS;
}

/* The pressed slots that may be reported (all of them in K3, only ESC in K2). */
static unsigned reportable(uint8_t out[KM_ENGINE_MAX_DOWN], unsigned esc)
{
    unsigned n = 0, m = 0;
    const uint8_t *down = km_engine_down_slots(&g_engine, &n);
    for (unsigned i = 0; i < n; i++) {
        if (KM_M4_ALL_KEYS != 0 || down[i] == esc) {
            out[m++] = down[i];
        } else {
            g_held_back++;
        }
    }
    return m;
}

/* Send the CURRENT key state if it differs from what the host last got and the endpoint is free; otherwise nothing,
 * and the next pass looks again (never a queue of old reports). */
static void send_reports(unsigned esc)
{
    if (!km_usbhs_configured()) {
        if (g_was_configured) {
            memset(g_last_boot, 0, sizeof g_last_boot);     /* a new enumeration starts from "nothing pressed" */
            memset(g_last_nkro, 0, sizeof g_last_nkro);
        }
        g_was_configured = false;
        return;
    }
    g_was_configured = true;

    uint8_t slots[KM_ENGINE_MAX_DOWN];
    const unsigned m = reportable(slots, esc);
    const bool boot = km_usbhs_kbd_boot_protocol();
    if (boot != g_was_boot_protocol) {
        /* the host switched protocol: the other path starts from "nothing pressed" */
        memset(g_last_boot, 0, sizeof g_last_boot);
        memset(g_last_nkro, 0, sizeof g_last_nkro);
        g_was_boot_protocol = boot;
    }
    if (boot) {
        uint8_t r[KM_REPORT_BOOT_LEN];
        km_report_build_boot(slots, m, r);
        if (memcmp(r, g_last_boot, sizeof r) != 0 && km_usbhs_kbd_ready() && km_usbhs_send_kbd(r)) {
            memcpy(g_last_boot, r, sizeof r);
            g_reports_sent++;
        }
    } else {
        uint8_t r[NKRO_REPORT_LEN];
        r[0] = 0x01u;                                       /* report id 1: keyboard bitmap (km_usb_reports.h) */
        km_report_build_nkro(slots, m, &r[1]);
        if (memcmp(r, g_last_nkro, sizeof r) != 0 && km_usbhs_nkro_ready() && km_usbhs_send_nkro(r, sizeof r)) {
            memcpy(g_last_nkro, r, sizeof r);
            g_reports_sent++;
        }
    }
}

int main(void)
{
    km_usbhs_set_vendor_handler(vendor);
    km_usbhs_init();
    km_scan_init(&g_scan);
    km_engine_init(&g_engine);
    const unsigned esc = esc_slot();
    const uint32_t init_err = adc_scan_init();
    adc_scan_start();

    uint32_t pushed = 0, publishes = 0, evaluated = 0;
    uint32_t cal_frames = 0, cal_attempts = 0;
    uint16_t cal_result = 0xFFFFu;
    uint32_t eng_last = 0, eng_max = 0;
    uint8_t last_mask = 0;
    uint32_t next = sense_cycles();
    for (;;) {
        km_iwdg_feed();
        km_usbhs_poll();
        if (g_enter_boot) {
            adc_scan_stop();
            km_usbhs_disconnect();
            km_flash_request_bootloader();
        }
        if (adc_scan_frame_ready()) {
            last_mask = adc_scan_phase_mask();
            for (unsigned p = 0; p < SENSE_NUM_PHASES; p++) {
                km_scan_push_phase(&g_scan, p, g_phase_words[p]);
            }
            km_scan_push_injected(&g_scan, g_inj[0], g_inj[1]);
            adc_scan_release_frame();
            pushed++;
            if (pushed > WARMUP_FRAMES) {
                const uint16_t *sums = km_scan_sums(&g_scan);
                for (unsigned i = 0; i < KM_ENGINE_SLOTS; i++) {
                    g_counts[i] = (uint16_t)(sums[i] >> 4);
                }
                if (!g_engine.calibrated) {
                    if (cal_frames == 0) {
                        km_engine_calibrate_begin(&g_engine);
                    }
                    km_engine_calibrate_idle(&g_engine, g_counts);
                    if (++cal_frames >= CAL_FRAMES) {
                        cal_result = (uint16_t)km_engine_calibrate_finish(&g_engine);
                        cal_attempts++;
                        cal_frames = 0;             /* on failure the next stretch starts at once, until no key is held */
                    }
                } else {
                    const uint32_t t0 = sense_cycles();
                    km_engine_update(&g_engine, g_counts);
                    eng_last = sense_cycles() - t0;
                    if (eng_last > eng_max) {
                        eng_max = eng_last;
                    }
                    evaluated++;
                }
            }
        }
        if (g_engine.calibrated) {
            send_reports(esc);
        }
        const uint32_t now = sense_cycles();
        if ((int32_t)(now - next) >= 0) {
            next += PUBLISH_CYCLES;
            publishes++;
            const uint32_t phase = publishes % 10u;
            if (phase == 0u) {
                adc_scan_stats_t st;
                adc_scan_get_stats(&st);
                const uint16_t d[24] = {
                    (uint16_t)st.phases, (uint16_t)(st.phases >> 16), (uint16_t)st.frames, (uint16_t)(st.frames >> 16),
                    clamp16(st.dma_errors), clamp16(st.overruns), (uint16_t)st.inj_events, (uint16_t)(st.inj_events >> 16),
                    clamp16(st.phase_cycles[0]), clamp16(st.phase_cycles[1]), clamp16(st.phase_cycles[2]), clamp16(st.phase_cycles[3]),
                    clamp16(st.frame_cycles), clamp16(st.frame_cycles_min), clamp16(st.frame_cycles_max), (uint16_t)init_err,
                    (uint16_t)pushed, (uint16_t)(pushed >> 16), 0, 0,
                    last_mask, 0, 0, 0,
                };
                km_scope_publish(0x0100u, d, 24);
            } else if (phase == 5u) {
                unsigned n = 0;
                const uint8_t *down = km_engine_down_slots(&g_engine, &n);
                uint16_t d[24] = {
                    cal_result, clamp16(cal_attempts), (uint16_t)n, (uint16_t)km_engine_glitch_count(&g_engine),
                    (uint16_t)(km_engine_glitch_count(&g_engine) >> 16), clamp16(eng_last), clamp16(eng_max),
                    (uint16_t)g_reports_sent, (uint16_t)(g_reports_sent >> 16), km_usbhs_kbd_boot_protocol() ? 1u : 0u,
                    (uint16_t)g_held_back, (uint16_t)(g_held_back >> 16),
                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                    g_engine.calibrated ? 1u : 0u, (uint16_t)evaluated, (uint16_t)(evaluated >> 16), 0, 0, 0,
                };
                for (unsigned i = 0; i < 6u && i < n; i++) {
                    d[12 + i] = down[i];
                }
                km_scope_publish(0x0200u, d, 24);
            } else if (phase == 2u) {
                uint16_t depth[KM_ENGINE_SLOTS];
                for (unsigned i = 0; i < KM_ENGINE_SLOTS; i++) {
                    depth[i] = km_engine_depth(&g_engine, i);
                }
                km_scope_publish(0x0201u, depth, KM_ENGINE_SLOTS);
            } else {
                km_scope_publish(0x0003u, km_scan_sums(&g_scan), 76);
            }
        }
    }
}
