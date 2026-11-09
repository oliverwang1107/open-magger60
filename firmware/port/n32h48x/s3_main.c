/*
 * S3 image (M3_PLAN.md section 3, step S3): the full scan. Stock-style setup (adc_scan.c): GTIM1 trigger -> ADC1+2 and
 * ADC3+4 sequences -> DMA1 ch1/ch3, four mux phases per frame, injected WASD channels by interrupt. The main loop
 * hands each completed frame to the scan core (km_scan_push_phase x4, km_scan_push_injected) and releases it, which
 * starts the next frame, so the sums advance once per main-loop pass that finds a frame ready.
 *
 * Every ~10 ms the main loop publishes:
 *   km_scope_publish tag 0x0003: the 76 sixteen-point sums (a few of every ten snapshots are tag 0x0100 instead)
 *   tag 0x0100 diagnostics (24 values):
 *     [0..1] phases completed  [2..3] frames completed (lo, hi)   [4] DMA errors   [5] overruns (unpaired DMA completions)
 *     [6..7] injected interrupts (lo, hi)
 *     [8..11] last duration of phase 0..3 in CPU cycles (clamped to 65535)   [12] last frame cycles  [13] min  [14] max
 *     [15] ADC init error bits    [16..17] frames pushed to the scan core (lo, hi)
 *     [18] frames pushed in the last window   [19] window length in ms (cycle counter at the assumed 240 MHz)
 *     [20] phase-done mask (4 bits) as seen by the main loop   [21..23] 0
 */
#include <stdbool.h>

#include "adc_scan.h"
#include "flash_boot.h"
#include "km/km_scan.h"
#include "km/km_scope.h"
#include "km/km_vendor.h"
#include "n32h48x_regs.h"
#include "sense_hw.h"
#include "usb_hs.h"

#define PUBLISH_CYCLES (10u * 1000u * 240u)      /* 10 ms */
#define WINDOW_CYCLES  (1000u * 1000u * 240u)    /* 1 s */

static const km_vendor_info_t k_info = { 0x0104, __DATE__ "," __TIME__ "KMFW-S3" };

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

int main(void)
{
    km_usbhs_set_vendor_handler(vendor);
    km_usbhs_init();
    km_scan_init(&g_scan);
    const uint32_t init_err = adc_scan_init();
    adc_scan_start();

    uint32_t pushed = 0, win_pushed = 0, win_count = 0, win_ms = 0;
    uint32_t publishes = 0;
    uint8_t last_mask = 0;
    uint32_t next = sense_cycles(), win_start = next;
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
            win_count++;
        }
        const uint32_t now = sense_cycles();
        if ((uint32_t)(now - win_start) >= WINDOW_CYCLES) {
            win_pushed = win_count;
            win_ms = (now - win_start) / 240000u;
            win_count = 0;
            win_start = now;
        }
        if ((int32_t)(now - next) >= 0) {
            next += PUBLISH_CYCLES;
            publishes++;
            if (publishes % 10u != 0u) {
                km_scope_publish(0x0003u, km_scan_sums(&g_scan), 76);
            } else {
                adc_scan_stats_t st;
                adc_scan_get_stats(&st);
                const uint16_t d[24] = {
                    (uint16_t)st.phases, (uint16_t)(st.phases >> 16), (uint16_t)st.frames, (uint16_t)(st.frames >> 16),
                    clamp16(st.dma_errors), clamp16(st.overruns), (uint16_t)st.inj_events, (uint16_t)(st.inj_events >> 16),
                    clamp16(st.phase_cycles[0]), clamp16(st.phase_cycles[1]), clamp16(st.phase_cycles[2]), clamp16(st.phase_cycles[3]),
                    clamp16(st.frame_cycles), clamp16(st.frame_cycles_min), clamp16(st.frame_cycles_max), (uint16_t)init_err,
                    (uint16_t)pushed, (uint16_t)(pushed >> 16), clamp16(win_pushed), clamp16(win_ms),
                    last_mask, 0, 0, 0,
                };
                km_scope_publish(0x0100u, d, 24);
            }
        }
    }
}
