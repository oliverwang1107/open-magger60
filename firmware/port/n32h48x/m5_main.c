/*
 * M5 full-feature image (M5_PLAN.md ladder L3-1 .. L3-3): the L3-0 stability image (m5_stab_main.c) plus the features of milestone M5.
 *
 *   engine     per-key trigger settings (FIXED / RAPID, dead zones) from the active profile (km_engine_apply_key_cfg)
 *   layers     4 layers per profile, FN and other function keys, media keys, profile switch keys (km_layers); the consumer report
 *              (IF2, report id 2) goes through its own reconciled report path next to the keyboard ones
 *   settings   our own format (km_settings), loaded at boot; with KM_M5_PERSIST also stored in flash (km_store: two alternating
 *              sectors, power-fail safe). Without it the settings live in RAM only and are gone at the next reset
 *   protocol   km_config (vendor command 0xC6): read / stage / commit settings, select profile, recalibrate, live depth,
 *              save calibration (km_l33 only)
 *   calibration  km_l33 only (M5 task T-F, km_calmgr.h): the learned bottom of every key is kept in flash sector 0x08034000
 *              (km_calib.h / km_calstore.h, an append-only log). At boot a valid record replaces the 64-frame calibration wait
 *              (km_cal_restore: one live frame, checked against the record, else the normal calibration runs); while the keyboard
 *              is used the bottoms are saved in the background when they changed noticeably, or on the host's save_calibration command.
 *
 * Variants (one source, two targets):
 *   km_l31  KM_M5_PERSIST = 0  "KMFW-L31"  NO flash writes except the enter-bootloader flag. COMMIT applies the settings to the running
 *                              keyboard and keeps them in RAM. Use it to try the whole configurator on the real keyboard without risk.
 *   km_l33  KM_M5_PERSIST = 1  "KMFW-L33"  as L31, but COMMIT (and a profile switch) also saves to flash sector A/B (store_flash.c).
 *
 * Calibration saves (km_l33): decided once per second (km_cal_policy_due: some bottom at least 16 counts deeper than at the last save,
 * unchanged for 20 s, at least 10 minutes since the previous save). The write is a job of km_calstore: ONE flash operation per main loop
 * pass (an 8-byte program, or once in 24 saves the 8 KB sector erase), started only after the keyboard has been idle for 1 s (no key
 * down, no fault, no report waiting) and paused while a key is down or a fault is active. The one exception: after the sector erase
 * the job runs on without pausing, because until its last unit is programmed no valid record exists. After every flash operation the
 * scan chain is restarted (adc_scan_restart), because the CPU was stalled by the flash. Worst case per pass: the sector erase (bounded
 * by store_flash.c to about 140 ms, typically far less, UNVERIFIED on this chip), otherwise one 8-byte program (microseconds).
 * The IWDG is fed by store_flash.c while it waits and by the normal progress-based feed between the passes.
 *
 * Light configuration (km_l51, km_ledstore.h): effect, brightness, speed, colour and the idle-off time live in an append-only log in sector
 * 0x08036000 (256 saves per erase). They are loaded at boot (m5_led_persist_attach) and saved only on the host's request (LED_SET with the
 * save flag), coalesced (3 s after the last request), only after 1 s of quiet, at most one save start per 10 s, one flash operation per pass
 * and never in the same pass as a calibration flash operation.
 *
 * Flash saves only happen when no key is down. COMMIT answers BUSY while a key is down. A profile switch by key or by command marks
 * the active profile dirty; it is stored once the keyboard has been idle for 1 s. Saving blocks the main loop for the duration of the
 * flash erase/program (the IWDG is fed inside store_flash.c); typing is not possible during that moment and the scan restart logic
 * of the health monitor may count one scan stall afterwards.
 *
 * Scope snapshots, one every 10 ms in rotation (publish counter p, p % 10), as in m5_stab_main.c:
 *   even p              tag 0x0301  keys the keyboard is reporting as a bitmask + sequence
 *   p % 10 == 1         tag 0x0003  the 76 sixteen-point sums
 *   p % 10 == 3         tag 0x0100  ADC diagnostics
 *   p % 10 == 5         tag 0x0200  engine diagnostics (24 values; K3 layout plus):
 *                         [10] active profile  [11] persistent build (1) or RAM only (0)  [21] settings source at boot
 *                         (0 defaults, 1 loaded from flash, 2 flash held no usable record)  [22] saves ok  [23] saves failed
 *   p % 10 == 7         tag 0x0300  health counters and reset cause
 *   p % 10 == 9         tag 0x0201  the 76 key depths in 0.01 mm
 */
#include <stdbool.h>
#include <string.h>

#include "adc_scan.h"
#include "flash_boot.h"
#include "km/km_calmgr.h"
#include "km/km_config.h"
#include "km/km_engine.h"
#include "km/km_health.h"
#include "km/km_keymap.h"
#include "km/km_layers.h"
#include "km/km_report.h"
#include "km/km_report_sync.h"
#include "km/km_scan.h"
#include "km/km_scope.h"
#include "km/km_settings.h"
#include "km/km_vendor.h"
#include "n32h48x_regs.h"
#include "reset_cause.h"
#include "sense_hw.h"
#include "usb_hs.h"

#ifndef KM_M5_PERSIST
#define KM_M5_PERSIST 0
#endif
#ifndef KM_M5_NAME
#define KM_M5_NAME "KMFW-L31"
#endif

#if KM_M5_PERSIST
#include "store_flash.h"
#endif

/* KM_M5_LED = 1: the lights (LED_NOTES.md). All of it lives in m5_led.c; this file only calls it, the key path never waits for it. */
#ifndef KM_M5_LED
#define KM_M5_LED 0
#endif
#if KM_M5_LED
#include "m5_led.h"
#endif

#define PUBLISH_CYCLES (10u * KM_MS_CYCLES)      /* 10 ms */
#define WARMUP_FRAMES  32u                       /* the 16-point sums are full after 16 frames */
#define CAL_FRAMES     64u                       /* length of the quiet stretch fed to the calibration */
#define NKRO_REPORT_LEN (1u + KM_REPORT_NKRO_LEN)
#define SAVE_IDLE_CYCLES (1000u * KM_MS_CYCLES)  /* a profile change is stored after this much time without a key down */

_Static_assert(KM_SETTINGS_ENCODED_SIZE <= KM_CONFIG_MAX_SETTINGS, "the configuration protocol buffer must hold a whole settings blob");

static const km_vendor_info_t k_info = { 0x0104, __DATE__ "," __TIME__ KM_M5_NAME };

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
static km_health_t g_health;
static uint16_t g_counts[KM_ENGINE_SLOTS];

/* ---- settings: the running copy, applied to the engine and the layers ---- */

static km_settings_t g_settings;
static km_settings_t g_staged;            /* scratch for validating / applying a committed blob */
static km_config_t g_cfg;

static uint8_t g_settings_source;         /* 0 defaults, 1 loaded from flash, 2 flash held no usable record */
static uint32_t g_saves_ok, g_saves_failed;
static bool g_active_dirty;               /* the active profile changed and is not stored yet (persistent build only) */
static uint32_t g_last_key_cycles;        /* last time a key was down */
static uint32_t g_down_now;               /* number of keys the engine reports down, updated every loop pass */
static uint32_t g_uptime_s;               /* seconds since boot, advanced by the main loop */

static void apply_profile(unsigned p)
{
    if (p >= KM_PROFILES) {
        p = 0;
    }
    const km_profile_t *prof = &g_settings.profile[p];
    for (unsigned slot = 0; slot < KM_ENGINE_SLOTS; slot++) {
        if (km_keymap_slot_is_key(slot)) {
            (void)km_engine_apply_key_cfg(&g_engine, slot, &prof->key[slot]);   /* an invalid config falls back to FIXED defaults */
        }
    }
    (void)km_layers_set_table(prof->layer_usage, prof->layer_arg, &prof->layer_opts);
}

static km_cfg_status_t save_settings(void)
{
#if KM_M5_PERSIST
    if (km_store_save(&g_settings)) {
        g_saves_ok++;
        g_active_dirty = false;
        return KM_CFG_OK;
    }
    g_saves_failed++;
    return KM_CFG_ERR_FAILED;
#else
    g_active_dirty = false;
    return KM_CFG_OK;
#endif
}

/* ---- calibration persistence (km_l33): record in sector 0x08034000, restored at boot, saved in the background ---- */

#if KM_M5_PERSIST
#define CAL_IDLE_CYCLES (1000u * KM_MS_CYCLES)   /* quiet time (no key, no fault) before a background save may start */

static km_calmgr_t g_calmgr;

static void cal_after_flash(void *ctx)
{
    (void)ctx;
    adc_scan_restart(sense_cycles());           /* the flash stalled the CPU: start the scan chain cleanly again */
}

/* The host's save_calibration command ("SCAL"): to the end, now. BUSY while a key is down or a fault is active. */
static km_cfg_status_t cfg_save_calibration(void *ctx)
{
    (void)ctx;
    switch (km_calmgr_save_request(&g_calmgr, g_uptime_s, g_down_now == 0u && g_health.fault_flags == 0u)) {
    case KM_CALMGR_OK:
        return KM_CFG_OK;
    case KM_CALMGR_BUSY:
        return KM_CFG_ERR_BUSY;
    default:
        return KM_CFG_ERR_FAILED;
    }
}
#define CFG_SAVE_CAL cfg_save_calibration
#else
#define CFG_SAVE_CAL 0
#endif

/* ---- the configuration protocol backend (km_config_ops_t) ---- */

static unsigned cfg_read(void *ctx, uint8_t *buf, unsigned cap)
{
    (void)ctx;
    return km_settings_encode(&g_settings, buf, cap);
}

static km_cfg_status_t cfg_validate(void *ctx, const uint8_t *blob, unsigned len)
{
    (void)ctx;
    return km_settings_decode(&g_staged, blob, len) ? KM_CFG_OK : KM_CFG_ERR_INVALID;
}

static km_cfg_status_t cfg_commit(void *ctx, const uint8_t *blob, unsigned len)
{
    (void)ctx;
    if (g_down_now != 0u) {
        return KM_CFG_ERR_BUSY;                 /* never touch flash or the key tables while a key is down: the host retries */
    }
    if (!km_settings_decode(&g_staged, blob, len)) {
        return KM_CFG_ERR_INVALID;
    }
    g_settings = g_staged;
    apply_profile(g_settings.active);
    return save_settings();
}

static km_cfg_status_t cfg_select_profile(void *ctx, unsigned index)
{
    (void)ctx;
    if (index >= KM_PROFILES) {
        return KM_CFG_ERR_RANGE;
    }
    g_settings.active = (uint8_t)index;
    apply_profile(index);
    g_active_dirty = (KM_M5_PERSIST != 0);
    return KM_CFG_OK;
}

static unsigned cfg_active_profile(void *ctx)
{
    (void)ctx;
    return g_settings.active;
}

static bool g_recal_request;

static km_cfg_status_t cfg_recalibrate(void *ctx)
{
    (void)ctx;
    if (g_down_now != 0u) {
        return KM_CFG_ERR_BUSY;
    }
    g_recal_request = true;                     /* the main loop starts the calibration at the next frame */
    return KM_CFG_OK;
}

static km_cfg_status_t cfg_live_depth(void *ctx, unsigned first, unsigned count, uint16_t *out)
{
    (void)ctx;
    if (first >= KM_ENGINE_SLOTS || count > KM_ENGINE_SLOTS - first) {
        return KM_CFG_ERR_RANGE;
    }
    for (unsigned i = 0; i < count; i++) {
        out[i] = km_engine_depth(&g_engine, first + i);
    }
    return KM_CFG_OK;
}

/* save_calibration exists in the persistent build only (km_l31 has no flash to put it in: the protocol answers UNSUPPORTED). */
static const km_config_ops_t k_cfg_ops = {
    0, KM_PROFILES, cfg_read, cfg_validate, cfg_commit, cfg_select_profile, cfg_active_profile, cfg_recalibrate, CFG_SAVE_CAL, cfg_live_depth,
};

/* ---- report paths: IF2 NKRO (id 1) + IF2 consumer (id 2) + IF0 boot keyboard; the boot path is live only in boot protocol ---- */

static km_report_sync_t g_sync_nkro, g_sync_boot, g_sync_cons;

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
/* the consumer report is a second report of the same IF2 endpoint: same link / ready / send functions */
static const km_sync_io_t k_io_cons = { 0, nkro_link, nkro_ready, nkro_send, ep_recover };

/* The pressed slots in press order, kept for the scope key mask (what the keyboard is reporting). */
static unsigned g_reported_n;
static uint8_t g_reported[KM_ENGINE_MAX_DOWN];

/* Slots -> layers -> usages -> the three report formats; pump() decides what goes out. */
static void pump_reports(uint32_t now, bool allowed)
{
    unsigned n = 0;
    const uint8_t *down = km_engine_down_slots(&g_engine, &n);
    if (n > KM_ENGINE_MAX_DOWN) {
        n = KM_ENGINE_MAX_DOWN;
    }
    memcpy(g_reported, down, n);
    g_reported_n = n;
    g_down_now = n;

    km_layers_out_t lo;
    km_layers_update_cycles(down, n, now, KM_MS_CYCLES, &lo);     /* tap-hold and one-shot keys need the time */
    if (lo.profile_request != 0u && allowed) {
        const unsigned idx = (unsigned)lo.profile_request - 1u;
        if (idx < KM_PROFILES && idx != g_settings.active) {
            (void)cfg_select_profile(0, idx);       /* the latched codes of keys still down stay as they are */
        }
    }

    uint8_t nkro[NKRO_REPORT_LEN];
    nkro[0] = 0x01u;                                    /* report id 1: keyboard bitmap (km_usb_reports.h) */
    km_report_build_nkro_usages(lo.kbd, lo.kbd_n, &nkro[1]);
    uint8_t boot[KM_REPORT_BOOT_LEN];
    km_report_build_boot_usages(lo.kbd, lo.kbd_n, boot);
    uint8_t cons[KM_REPORT_CONSUMER_LEN];
    km_report_build_consumer(lo.consumer, cons);
    km_report_sync_pump(&g_sync_nkro, &k_io_nkro, nkro, allowed, now);
    km_report_sync_pump(&g_sync_boot, &k_io_boot, boot, allowed, now);
    km_report_sync_pump(&g_sync_cons, &k_io_cons, cons, allowed, now);
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
    km_engine_init(&g_engine);

    /* settings: defaults, then whatever the flash holds (persistent build) */
    km_settings_default(&g_settings);
#if KM_M5_PERSIST
    km_store_bind(km_store_flash());
    g_settings_source = km_store_load(&g_settings) ? 1u : 2u;
    if (g_settings_source == 2u) {
        km_settings_default(&g_settings);               /* a failed load leaves *s untouched, but be explicit */
    }
#endif
    apply_profile(g_settings.active);
#if KM_M5_PERSIST
    km_calmgr_init(&g_calmgr, km_calstore_flash(), &g_engine, cal_after_flash, 0);
#endif
    km_layers_reset();
    km_config_init(&g_cfg, &k_cfg_ops);
#if KM_M5_LED
    km_config_set_led_ops(&g_cfg, m5_led_cfg_ops());
    km_config_set_led_ex_ops(&g_cfg, m5_led_cfg_ex_ops());
#endif
    km_vendor_set_config(&g_cfg);

    const uint32_t init_err = adc_scan_init();
    adc_scan_start();
    km_usbhs_get_events(&g_ev_seen);

    uint32_t now = sense_cycles();
    km_health_init(&g_health, now, cause);
    static const uint8_t nkro_released[NKRO_REPORT_LEN] = { 0x01u };
    static const uint8_t boot_released[KM_REPORT_BOOT_LEN] = { 0 };
    static const uint8_t cons_released[KM_REPORT_CONSUMER_LEN] = { KM_REPORT_CONSUMER_ID, 0, 0 };
    km_report_sync_init(&g_sync_nkro, nkro_released, NKRO_REPORT_LEN, now);
    km_report_sync_init(&g_sync_boot, boot_released, KM_REPORT_BOOT_LEN, now);
    km_report_sync_init(&g_sync_cons, cons_released, KM_REPORT_CONSUMER_LEN, now);
    g_last_key_cycles = now;
#if KM_M5_LED
    m5_led_init();
#if KM_M5_PERSIST
    m5_led_persist_attach(km_ledstore_flash(), cal_after_flash, 0);   /* the stored light configuration, if there is a valid one */
#endif
#endif

    uint32_t pushed = 0, publishes = 0, evaluated = 0;
    uint32_t cal_frames = 0, cal_attempts = 0;
    uint16_t cal_result = 0xFFFFu;
    bool was_calibrated = false;
    uint32_t eng_last = 0, eng_max = 0, eng_total_max = 0, eng_irq_max = 0;
    uint8_t last_mask = 0;
    uint8_t crumb_flags = 0;
    uint16_t keymask_seq = 0;
    uint32_t next_second = now + KM_CPU_HZ;
    uint32_t next = now;
    for (;;) {
        now = sense_cycles();
        km_usbhs_poll();
        if (!km_report_sync_stalled(&g_sync_nkro, now) && !km_report_sync_stalled(&g_sync_boot, now) &&
            !km_report_sync_stalled(&g_sync_cons, now)) {
            km_health_progress(&g_health, KM_PROG_USB);     /* the poll ran AND the endpoint is taking what is due */
        }
        poll_usb_events();
        if (g_enter_boot) {
            adc_scan_stop();
            km_usbhs_disconnect();
            km_flash_request_bootloader();
        }

        if (g_recal_request) {                              /* user asked for a recalibration (keys were up when asked) */
            g_recal_request = false;
#if KM_M5_PERSIST
            km_calmgr_keep_learned(&g_calmgr);              /* what the keys have learned survives the recalibration */
#endif
            g_engine.calibrated = false;
            cal_frames = 0;
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
#if KM_M5_PERSIST
                        /* a record from flash: ONE live frame instead of the quiet stretch, if the sensor still looks like the record */
                        if (cal_frames == 0 && km_calmgr_try_restore(&g_calmgr, g_counts)) {
                            cal_result = 0;
                            cal_attempts++;
                            was_calibrated = true;
                            km_health_on_calibration(&g_health, true);
                        }
#endif
                        if (!g_engine.calibrated) {
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
#if KM_M5_PERSIST
                                if (g_engine.calibrated) {
                                    km_calmgr_armed(&g_calmgr, true);   /* put back what was learned before (a recalibration), where the sensor agrees */
                                }
#endif
                            }
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
        km_health_set_report_stats(&g_health, g_sync_nkro.retries + g_sync_boot.retries + g_sync_cons.retries,
                                   g_sync_nkro.resyncs + g_sync_boot.resyncs + g_sync_cons.resyncs);
#if KM_M5_LED
        m5_led_service(now, &g_engine, g_reported, g_reported_n, g_health.fault_flags);   /* after everything the keys need */
#endif

        /* a profile change (key or command) is stored once the keyboard has been idle for a while */
        if (g_down_now != 0u) {
            g_last_key_cycles = now;
        } else if (g_active_dirty && (uint32_t)(now - g_last_key_cycles) >= SAVE_IDLE_CYCLES) {
            (void)save_settings();
            g_last_key_cycles = sense_cycles();
        }

#if KM_M5_PERSIST
        {   /* calibration save in the background: one flash operation per pass, only while nothing is happening */
            const bool idle_now = g_down_now == 0u && g_health.fault_flags == 0u;
            const bool idle_long = idle_now && !g_sync_nkro.undelivered && !g_sync_boot.undelivered && !g_sync_cons.undelivered &&
                                   (uint32_t)(now - g_last_key_cycles) >= CAL_IDLE_CYCLES;
            /* ONE flash operation per pass in all: a light-configuration save that has already erased its sector goes first (no record exists
             * until it is finished), then the calibration job, then a light-configuration save may use the pass if no calibration job is running */
            bool did_flash = false;
#if KM_M5_LED
            if (m5_led_persist_urgent()) {
                did_flash = m5_led_persist_pass(g_uptime_s, idle_now, idle_long);
            }
#endif
            if (!did_flash) {
                did_flash = km_calmgr_pass(&g_calmgr, g_uptime_s, idle_now, idle_long);
            }
#if KM_M5_LED
            if (!did_flash && !km_calmgr_job_active(&g_calmgr)) {
                did_flash = m5_led_persist_pass(g_uptime_s, idle_now, idle_long);
            }
#endif
            if (did_flash) {
                g_last_key_cycles = sense_cycles();         /* the stall must not count as quiet time for the settings save above */
            }
        }
#endif

        if (km_health_should_feed(&g_health, now)) {
            km_iwdg_feed();                                 /* the ONLY feed in the main loop */
        }
        if (g_health.fault_flags != crumb_flags) {
            crumb_flags = g_health.fault_flags;
            km_breadcrumb_update(crumb_flags);
        }
        if ((int32_t)(now - next_second) >= 0) {
            next_second += KM_CPU_HZ;
            g_uptime_s++;
#if KM_M5_PERSIST
            km_calmgr_second(&g_calmgr, g_uptime_s);
#endif
        }

        if ((int32_t)(now - next) >= 0) {
            next += PUBLISH_CYCLES;
            publishes++;
            const uint32_t phase = publishes % 10u;
            if ((phase & 1u) == 0u) {
                uint16_t d[KM_KEYMASK_SCOPE_VALUES];
                const bool allowed = g_health.fault_flags == 0u;
                km_health_pack_keymask(g_reported, allowed ? g_reported_n : 0u, ++keymask_seq, g_health.fault_flags,
                                       g_sync_nkro.undelivered || g_sync_boot.undelivered || g_sync_cons.undelivered, d);
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
                const uint32_t sent = g_sync_nkro.sent + g_sync_boot.sent + g_sync_cons.sent;
                uint16_t d[24] = {
                    cal_result, clamp16(cal_attempts), (uint16_t)n, (uint16_t)km_engine_glitch_count(&g_engine),
                    (uint16_t)(km_engine_glitch_count(&g_engine) >> 16), clamp16(eng_last), clamp16(eng_max),
                    (uint16_t)sent, (uint16_t)(sent >> 16),
                    km_usbhs_kbd_boot_protocol() ? 1u : 0u,
                    g_settings.active, KM_M5_PERSIST ? 1u : 0u,
                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                    g_engine.calibrated ? 1u : 0u, (uint16_t)evaluated, (uint16_t)(evaluated >> 16),
                    g_settings_source, clamp16(g_saves_ok), clamp16(g_saves_failed),
                };
                for (unsigned i = 0; i < 6u && i < n; i++) {
                    d[12 + i] = down[i];
                }
                km_scope_publish(0x0200u, d, 24);
            } else if (phase == 7u) {
                const uint32_t sent = g_sync_nkro.sent + g_sync_boot.sent + g_sync_cons.sent;
                const uint16_t extra[7] = {
                    (uint16_t)sent,
                    clamp16(g_sync_nkro.heartbeats + g_sync_boot.heartbeats + g_sync_cons.heartbeats),
                    clamp16(g_sync_nkro.recoveries + g_sync_boot.recoveries + g_sync_cons.recoveries),
                    crumb.valid ? crumb.prev_faults : 0xFFFFu,
                    clamp16(crumb.boots),
                    clamp16(g_uptime_s),
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
#if KM_M5_PERSIST
                if ((publishes % 100u) == 51u) {
                    /* tag 0x0202, calibration store: [0] bits 0-3 boot result (0 no record, 1 restored, 2 record disagreed, 3 frame refused),
                     * bit 4 record valid, bit 5 job running, bit 6 save due, bit 7 changed noticeably  [1] saves ok  [2] saves failed
                     * [3] last job result (km_calstore.h km_clog_result_t)  [4] strokes applied at boot  [5] log at boot: valid records
                     * | dirty slots << 8  [6] log at boot: next slot  [7] seconds since the last save (0xFFFF = none) */
                    const km_calmgr_t *cm = &g_calmgr;
                    const uint16_t d[8] = {
                        (uint16_t)(cm->boot | (cm->rec_valid ? 0x10u : 0u) | (km_calmgr_job_active(cm) ? 0x20u : 0u) |
                                   (cm->due ? 0x40u : 0u) | (km_calmgr_changed(cm) ? 0x80u : 0u)),
                        clamp16(cm->saves_ok), clamp16(cm->saves_failed), cm->last, cm->applied,
                        (uint16_t)(cm->scan0.used | (cm->scan0.dirty << 8)), (uint16_t)cm->scan0.next,
                        cm->pol.saved_once ? clamp16(g_uptime_s - cm->pol.last_save_s) : 0xFFFFu,
                    };
                    km_scope_publish(0x0202u, d, 8);
                }
#if KM_M5_LED
                else if ((publishes % 100u) == 21u) {        /* the lights' diagnostics (tag 0x0400), phase 1 like the calibration store's */
                    uint16_t d[M5_LED_DIAG_VALUES];
                    m5_led_diag(d);
                    km_scope_publish(M5_LED_SCOPE_TAG, d, M5_LED_DIAG_VALUES);
                }
#endif
                else {
                    uint16_t fd[5];
                    km_store_flash_diag(fd);
                    const uint16_t d[8] = { (uint16_t)km_store_last_result(), fd[0], fd[1], fd[2], fd[3], fd[4], clamp16(g_saves_ok), clamp16(g_saves_failed) };
                    km_scope_publish(0x0203u, d, 8);   /* flash diagnostics in place of the sums snapshot */
                }
#else
#if KM_M5_LED
                if (publishes % 80u == 1u) {                /* one in eight of the sums snapshots makes room for the LED diagnostics */
                    uint16_t d[M5_LED_DIAG_VALUES];
                    m5_led_diag(d);
                    km_scope_publish(M5_LED_SCOPE_TAG, d, M5_LED_DIAG_VALUES);
                } else
#endif
                km_scope_publish(0x0003u, km_scan_sums(&g_scan), 76);
#endif
            }
        }
    }
}
