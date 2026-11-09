/*
 * Noise-measurement DIAGNOSTIC image "KMFW-NOISE" (M6 task P12a, M6_PLAN.md section 6.3, core/NOISE_NOTES.md).
 * Built from m5_stab_main.c (the L3-0 stability image) by copying: scan -> key engine -> NKRO / boot reports, vendor cmd 3, scope
 * channel, enter-bootloader command, all fail-safes. Typing works as in L3-0, so "typing elsewhere" can be measured. DIAGNOSTIC ONLY:
 * it is never the daily image.
 *
 * What is added:
 *   - km_capture (core/include/km/km_capture.h): the RAW 12-bit samples of up to four slots, frame by frame, into a 32 KB RAM buffer,
 *     read back by tools/magger60_noise.py through the capture ops of the scope command (km_scope.h). The capture only READS the DMA
 *     words (g_phase_words / g_inj) in the main loop, in the place where they are fed to km_scan; it never writes them, touches no
 *     register and does not influence the key path.
 *   - The condition flag of each captured frame = "a USB packet was handled since the previous frame" (a vendor request, or a key
 *     report sent), so that the tool can compare noise with and without USB traffic.
 *
 * What is NOT here: no lights (no LED SPI/DMA, PF5 untouched), no settings, no calibration store, no flash write except the boot
 * flag of the enter-bootloader command that the tools use to flash back (km_flash_request_bootloader, the same as every image since D4).
 * Defaults only: trigger settings are the engine defaults (FIXED 1.2 / 0.8 mm).
 *
 * Scope snapshots are the same rotation as L3-0 (one every 10 ms; tags 0x0301, 0x0003, 0x0100, 0x0200, 0x0300, 0x0201); the capture
 * ops are separate (want_tag 0xC000..0xCFFF) and do not disturb them.
 */
#include <stdbool.h>
#include <string.h>

#include "adc_scan.h"
#include "flash_boot.h"
#include "km/km_capture.h"
#include "km/km_capture_scope.h"
#include "km/km_engine.h"
#include "km/km_health.h"
#include "km/km_keymap.h"
#include "km/km_report.h"
#include "km/km_report_sync.h"
#include "km/km_scan.h"
#include "km/km_scope.h"
#include "km/km_vendor.h"
#include "n32h48x_regs.h"
#include "reset_cause.h"
#include "sense_hw.h"
#include "usb_hs.h"

#ifndef KM_M5_NAME
#define KM_M5_NAME "KMFW-NOISE"
#endif

#define PUBLISH_CYCLES (10u * KM_MS_CYCLES)      /* 10 ms */
#define WARMUP_FRAMES  32u                       /* the 16-point sums are full after 16 frames */
#define CAL_FRAMES     64u                       /* length of the quiet stretch fed to the calibration */
#define NKRO_REPORT_LEN (1u + KM_REPORT_NKRO_LEN)

static const km_vendor_info_t k_info = { 0x0104, __DATE__ "," __TIME__ KM_M5_NAME };

static volatile bool g_enter_boot;
static uint32_t g_vendor_calls;                         /* vendor requests handled (the capture's "USB busy" condition) */

static bool vendor(const uint8_t in[64], uint8_t out[64])
{
    if (km_vendor_is_enter_boot_request(in)) {
        g_enter_boot = true;
        return false;
    }
    g_vendor_calls++;
    return km_vendor_handle(&k_info, in, out);
}

static uint16_t clamp16(uint32_t v)
{
    return v > 0xFFFFu ? 0xFFFFu : (uint16_t)v;
}

static km_scan_t g_scan;
static km_capture_t g_cap;                              /* 32 KB of raw samples, diagnostic image only */
static km_engine_t g_engine;
static km_health_t g_health;
static uint16_t g_counts[KM_ENGINE_SLOTS];

/* ---- report paths: IF2 NKRO (report id 1) and IF0 boot keyboard, exactly one is live at any time ---- */

static km_report_sync_t g_sync_nkro, g_sync_boot;

static bool bus_ok(void)
{
    return km_usbhs_configured() && !km_usbhs_suspended();
}

static bool nkro_link(void *c)
{
    (void)c;
    return bus_ok() && !km_usbhs_kbd_boot_protocol();
}

static bool boot_link(void *c)
{
    (void)c;
    return bus_ok() && km_usbhs_kbd_boot_protocol();
}

static bool nkro_ready(void *c)
{
    (void)c;
    return km_usbhs_nkro_ready();
}

static bool boot_ready(void *c)
{
    (void)c;
    return km_usbhs_kbd_ready();
}

static bool nkro_send(void *c, const uint8_t *r, unsigned len)
{
    (void)c;
    return km_usbhs_send_nkro(r, len);
}

static bool boot_send(void *c, const uint8_t *r, unsigned len)
{
    (void)c;
    return len == KM_REPORT_BOOT_LEN && km_usbhs_send_kbd(r);
}

static void ep_recover(void *c)
{
    (void)c;
    km_usbhs_in_recover();
}

static const km_sync_io_t k_io_nkro = { 0, nkro_link, nkro_ready, nkro_send, ep_recover };
static const km_sync_io_t k_io_boot = { 0, boot_link, boot_ready, boot_send, ep_recover };

/* The pressed slots in press order -> both report formats; pump() decides what goes out. */
static unsigned g_reported_n;
static uint8_t g_reported[KM_ENGINE_MAX_DOWN];

static void pump_reports(uint32_t now, bool allowed)
{
    unsigned n = 0;
    const uint8_t *down = km_engine_down_slots(&g_engine, &n);
    if (n > KM_ENGINE_MAX_DOWN) {
        n = KM_ENGINE_MAX_DOWN;
    }
    memcpy(g_reported, down, n);
    g_reported_n = n;

    uint8_t nkro[NKRO_REPORT_LEN];
    nkro[0] = 0x01u;                                    /* report id 1: keyboard bitmap (km_usb_reports.h) */
    km_report_build_nkro(down, n, &nkro[1]);
    uint8_t boot[KM_REPORT_BOOT_LEN];
    km_report_build_boot(down, n, boot);
    km_report_sync_pump(&g_sync_nkro, &k_io_nkro, nkro, allowed, now);
    km_report_sync_pump(&g_sync_boot, &k_io_boot, boot, allowed, now);
}

/* ---- fail-safe plumbing ---- */

static km_usbhs_events_t g_ev_seen;
static uint32_t g_dma_err_total;

static void poll_usb_events(void)
{
    km_usbhs_events_t ev;
    km_usbhs_get_events(&ev);
    while (g_ev_seen.resets != ev.resets) {
        g_ev_seen.resets++;
        km_health_on_usb(&g_health, KM_USB_EV_RESET);       /* the sync layer notices the dropped configuration by itself */
    }
    g_ev_seen = ev;
    km_health_set_usb_suspended(&g_health, km_usbhs_suspended());
}

int main(void)
{
    const uint8_t cause = km_reset_cause_read_and_clear();
    km_breadcrumb_t crumb;
    km_breadcrumb_boot(&crumb);

    km_usbhs_set_vendor_handler(vendor);
    km_usbhs_init();
    km_scan_init(&g_scan);
    km_capture_init(&g_cap, (uint16_t)(KM_CPU_HZ / 1000000u));
    km_scope_set_capture(&km_capture_scope_ops, &g_cap);
    km_engine_init(&g_engine);
    const uint32_t init_err = adc_scan_init();
    adc_scan_start();
    km_usbhs_get_events(&g_ev_seen);

    uint32_t now = sense_cycles();
    km_health_init(&g_health, now, cause);
    static const uint8_t nkro_released[NKRO_REPORT_LEN] = { 0x01u };
    static const uint8_t boot_released[KM_REPORT_BOOT_LEN] = { 0 };
    km_report_sync_init(&g_sync_nkro, nkro_released, NKRO_REPORT_LEN, now);
    km_report_sync_init(&g_sync_boot, boot_released, KM_REPORT_BOOT_LEN, now);

    uint32_t pushed = 0, publishes = 0, evaluated = 0;
    uint32_t cal_frames = 0, cal_attempts = 0;
    uint16_t cal_result = 0xFFFFu;
    bool was_calibrated = false;
    uint32_t eng_last = 0, eng_max = 0, eng_total_max = 0, eng_irq_max = 0;
    uint8_t last_mask = 0;
    uint8_t crumb_flags = 0;
    uint16_t keymask_seq = 0;
    uint32_t uptime_s = 0, next_second = now + KM_CPU_HZ;
    uint32_t cond_vendor = 0, cond_sent = 0;
    uint32_t next = now;
    for (;;) {
        now = sense_cycles();
        km_usbhs_poll();
        if (!km_report_sync_stalled(&g_sync_nkro, now) && !km_report_sync_stalled(&g_sync_boot, now)) {
            km_health_progress(&g_health, KM_PROG_USB);     /* the poll ran AND the endpoint is taking what is due */
        }
        poll_usb_events();
        if (g_enter_boot) {
            adc_scan_stop();
            km_usbhs_disconnect();
            km_flash_request_bootloader();
        }

        if (adc_scan_frame_ready()) {
            const uint32_t dma_new = adc_scan_take_dma_errors();
            if (dma_new != 0u) {
                g_dma_err_total += dma_new;
                km_health_on_dma_error(&g_health, now, dma_new);
                adc_scan_release_frame();                   /* the frame may hold garbage: drop it, keys stay released 30 ms */
            } else {
                last_mask = adc_scan_phase_mask();
                for (unsigned p = 0; p < SENSE_NUM_PHASES; p++) {
                    km_scan_push_phase(&g_scan, p, g_phase_words[p]);
                }
                km_scan_push_injected(&g_scan, g_inj[0], g_inj[1]);
                {
                    const uint32_t sent = g_sync_nkro.sent + g_sync_boot.sent;
                    const bool busy = g_vendor_calls != cond_vendor || sent != cond_sent;
                    cond_vendor = g_vendor_calls;
                    cond_sent = sent;
                    (void)km_capture_push_frame(&g_cap, &g_phase_words[0][0], g_inj, now, busy);   /* read-only: the words are released below */
                }
                adc_scan_release_frame();
                pushed++;
                km_health_on_frame(&g_health);
                if (pushed > WARMUP_FRAMES) {
                    const uint16_t *sums = km_scan_sums(&g_scan);
                    for (unsigned i = 0; i < KM_ENGINE_SLOTS; i++) {
                        g_counts[i] = (uint16_t)(sums[i] >> 4);
                    }
                    if (!g_engine.calibrated) {
                        if (was_calibrated) {
                            km_health_cal_lost(&g_health);
                            was_calibrated = false;
                        }
                        if (cal_frames == 0) {
                            km_engine_calibrate_begin(&g_engine);
                        }
                        km_engine_calibrate_idle(&g_engine, g_counts);
                        if (++cal_frames >= CAL_FRAMES) {
                            cal_result = (uint16_t)km_engine_calibrate_finish(&g_engine);
                            cal_attempts++;
                            cal_frames = 0;         /* on failure the next stretch starts at once, until no key is held */
                            was_calibrated = g_engine.calibrated;
                            km_health_on_calibration(&g_health, g_engine.calibrated);
                        }
                    } else {
                        const uint32_t irq0 = adc_scan_irq_cycles();
                        const uint32_t t0 = sense_cycles();
                        km_engine_update(&g_engine, g_counts);
                        const uint32_t total = sense_cycles() - t0;
                        const uint32_t irq = adc_scan_irq_cycles() - irq0;
                        eng_last = total > irq ? total - irq : 0u;      /* the engine's own time */
                        if (eng_last > eng_max) {
                            eng_max = eng_last;
                        }
                        if (total > eng_total_max) {
                            eng_total_max = total;
                        }
                        if (irq > eng_irq_max) {
                            eng_irq_max = irq;
                        }
                        km_health_on_engine(&g_health, now, eng_last);
                        km_health_set_glitches(&g_health, km_engine_glitch_count(&g_engine));
                        evaluated++;
                    }
                }
            }
        } else if (adc_scan_frame_timeout(now)) {
            if (km_health_on_scan_timeout(&g_health, now)) {
                adc_scan_restart(now);
                const uint32_t e = adc_scan_take_dma_errors();
                g_dma_err_total += e;
                km_health_on_dma_error(&g_health, now, e);
            }
        }

        km_health_tick(&g_health, now);
        pump_reports(now, g_health.fault_flags == 0u);
        km_health_set_report_stats(&g_health, g_sync_nkro.retries + g_sync_boot.retries,
                                   g_sync_nkro.resyncs + g_sync_boot.resyncs);
        if (km_health_should_feed(&g_health, now)) {
            km_iwdg_feed();                                 /* the ONLY feed in the main loop */
        }
        if (g_health.fault_flags != crumb_flags) {
            crumb_flags = g_health.fault_flags;
            km_breadcrumb_update(crumb_flags);
        }
        if ((int32_t)(now - next_second) >= 0) {
            next_second += KM_CPU_HZ;
            uptime_s++;
        }

        if ((int32_t)(now - next) >= 0) {
            next += PUBLISH_CYCLES;
            publishes++;
            const uint32_t phase = publishes % 10u;
            if ((phase & 1u) == 0u) {
                uint16_t d[KM_KEYMASK_SCOPE_VALUES];
                const bool allowed = g_health.fault_flags == 0u;
                km_health_pack_keymask(g_reported, allowed ? g_reported_n : 0u, ++keymask_seq, g_health.fault_flags,
                                       g_sync_nkro.undelivered || g_sync_boot.undelivered, d);
                km_scope_publish(KM_SCOPE_TAG_KEYMASK, d, KM_KEYMASK_SCOPE_VALUES);
            } else if (phase == 3u) {
                adc_scan_stats_t st;
                adc_scan_get_stats(&st);
                const uint16_t d[24] = {
                    (uint16_t)st.phases, (uint16_t)(st.phases >> 16), (uint16_t)st.frames, (uint16_t)(st.frames >> 16),
                    clamp16(st.dma_errors), clamp16(st.overruns), (uint16_t)st.inj_events, (uint16_t)(st.inj_events >> 16),
                    clamp16(st.phase_cycles[0]), clamp16(st.phase_cycles[1]), clamp16(st.phase_cycles[2]), clamp16(st.phase_cycles[3]),
                    clamp16(st.frame_cycles), clamp16(st.frame_cycles_min), clamp16(st.frame_cycles_max), (uint16_t)init_err,
                    (uint16_t)pushed, (uint16_t)(pushed >> 16), clamp16(st.restarts), clamp16(eng_irq_max),
                    last_mask, 0, 0, 0,
                };
                km_scope_publish(0x0100u, d, 24);
            } else if (phase == 5u) {
                unsigned n = 0;
                const uint8_t *down = km_engine_down_slots(&g_engine, &n);
                uint16_t d[24] = {
                    cal_result, clamp16(cal_attempts), (uint16_t)n, (uint16_t)km_engine_glitch_count(&g_engine),
                    (uint16_t)(km_engine_glitch_count(&g_engine) >> 16), clamp16(eng_last), clamp16(eng_max),
                    (uint16_t)(g_sync_nkro.sent + g_sync_boot.sent), (uint16_t)((g_sync_nkro.sent + g_sync_boot.sent) >> 16),
                    km_usbhs_kbd_boot_protocol() ? 1u : 0u,
                    0, 0,
                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                    g_engine.calibrated ? 1u : 0u, (uint16_t)evaluated, (uint16_t)(evaluated >> 16), 0, 0, 0,
                };
                for (unsigned i = 0; i < 6u && i < n; i++) {
                    d[12 + i] = down[i];
                }
                km_scope_publish(0x0200u, d, 24);
            } else if (phase == 7u) {
                const uint16_t extra[7] = {
                    (uint16_t)(g_sync_nkro.sent + g_sync_boot.sent),
                    clamp16(g_sync_nkro.heartbeats + g_sync_boot.heartbeats),
                    clamp16(g_sync_nkro.recoveries + g_sync_boot.recoveries),
                    crumb.valid ? crumb.prev_faults : 0xFFFFu,
                    clamp16(crumb.boots),
                    clamp16(uptime_s),
                    clamp16(eng_total_max),
                };
                uint16_t d[KM_HEALTH_SCOPE_VALUES];
                km_health_pack_scope(&g_health, extra, d);
                km_scope_publish(KM_SCOPE_TAG_HEALTH, d, KM_HEALTH_SCOPE_VALUES);
            } else if (phase == 9u) {
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
