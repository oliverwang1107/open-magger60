/*
 * m5_led.h - the LED service of the M5 image (M5 task L5): glue between the key engine, the health monitor, the portable LED core
 * (km_led.h), the kill-switch policy (km_led_guard.h) and the hardware (led_hw.h). m5_main.c calls it behind KM_M5_LED:
 *
 *   m5_led_init()                         once at boot, before the main loop
 *   m5_led_service(...)                   once per main-loop pass; the idle path costs a few dozen cycles, a pass that does work
 *                                         costs at most one slice (see LED_NOTES.md, timing budget)
 *   m5_led_diag()                         the 24 values of scope tag M5_LED_SCOPE_TAG
 *   m5_led_get_cfg() / m5_led_set_cfg()   run-time effect / brightness / speed / colour (RAM only, validated, can not exceed the limits)
 *   m5_led_persist_*()                    keep the light configuration and the idle-off time in flash (km_ledstore.h), km_l51 only
 *
 * Fail-safe design: the service never blocks, never loops without a bound, starts the LED hardware only after the keyboard has been
 * healthy (calibrated, no fault) for M5_LED_START_DELAY_MS, and switches the LED off for good (pins released) on any of the
 * conditions in km_led_guard.h. The key path does not call anything in here that can fail.
 */
#ifndef M5_LED_H
#define M5_LED_H

#include <stdbool.h>
#include <stdint.h>

#include "km/km_config.h"
#include "km/km_engine.h"
#include "km/km_calstore.h"
#include "km/km_led.h"
#include "km/km_lightscene.h"
#include "km/km_store.h"

#define M5_LED_SCOPE_TAG       0x0400u
#define M5_LED_DIAG_VALUES     24u
#define M5_LED_START_DELAY_MS  1500u      /* healthy for this long before the LED hardware is touched */
#define M5_LED_WORK_UNITS      8u         /* lights rendered or encoded per call: the size of one slice */

void m5_led_init(void);
/* now_cycles = DWT cycle counter; down/n_down = the engine's pressed slots; fault_flags = km_health fault flags. */
void m5_led_service(uint32_t now_cycles, const km_engine_t *eng, const uint8_t *down, unsigned n_down, uint8_t fault_flags);
void m5_led_diag(uint16_t d[M5_LED_DIAG_VALUES]);
void m5_led_get_cfg(km_led_cfg_t *cfg);
bool m5_led_set_cfg(const km_led_cfg_t *cfg);
/* The same two as the optional light ops of the configuration protocol (km_config_set_led_ops): RAM only, 6 bytes
 * (effect, brightness, speed, r, g, b), an invalid value is answered RANGE and changes nothing. */
const km_config_led_ops_t *m5_led_cfg_ops(void);
/* The extended form (km_config_set_led_ex_ops): the same six bytes plus the idle-off time, the save flag and the status (km_config.h). */
const km_config_led_ex_ops_t *m5_led_cfg_ex_ops(void);

/*
 * Layers above the base effect (M6 package P6c, LED_NOTES.md section 18). All of it is RAM state of the light core; nothing here reaches flash
 * except m5_led_scene_commit() with save = true.
 *
 *   m5_led_set_host_leds_source(fn)   fn() returns the host's keyboard LED bits (KM_LED_HOST_*; 0 = off / unknown). The service polls it once per
 *                                     pass. P5a (USB) provides the real source, km_usbhs_host_leds(); until then (and in the images that do not
 *                                     wire it) there is none and the bits stay 0. A NULL function detaches it.
 *   m5_led_set_inputs(in)             layer, profile and the PAD / TOGGLE flags for the indicator rules (host_leds in `in` is ignored: the
 *                                     source above owns it). Call it when they change or every pass; it is a few compares.
 *   m5_led_scene_* / m5_led_ext_*     the light scene (km_lightscene.h) and the external frame inputs (km_led.h, KM_LED_EXT_*)
 */
typedef uint8_t (*m5_led_host_leds_fn)(void);
void m5_led_set_host_leds_source(m5_led_host_leds_fn fn);
void m5_led_set_inputs(const km_led_inputs_t *in);

/* The scene in use (RAM copy). m5_led_scene_set applies a scene at once (false = invalid, nothing changes); get copies it out. */
bool m5_led_scene_set(const km_lightscene_t *s);
void m5_led_scene_get(km_lightscene_t *s);
/* Boot: read the newest scene from the two-sector store and apply it; false = none (the default scene stays). NULL store = no persistence. */
bool m5_led_scene_attach(const km_store_t *store);
/* Apply a scene and, with save = true, keep it in flash (km_lightscene_save: SYNCHRONOUS, one sector erase, the CPU stalls; call it when no key is
 * down and USB is quiet, then restart the scan chain). Returns KM_STORE_ERR_ARG for an invalid scene (nothing applied), KM_STORE_OK when nothing was
 * to be saved or the save worked, KM_STORE_UNCHANGED when flash already held it, and the store's error otherwise (the scene stays applied in RAM
 * but is NOT kept; the previous flash record is untouched). Without an attached store a save request is KM_STORE_ERR_ARG and applies nothing. */
km_store_result_t m5_led_scene_commit(const km_lightscene_t *s, bool save);

/* External frames (km_led.h): complete frames only. Time is the service's own millisecond clock. */
void m5_led_ext_rgb(unsigned source, const km_led_rgb_t px[KM_LED_COUNT]);
void m5_led_ext_gray(unsigned source, const uint8_t gray[KM_LED_COUNT]);
void m5_led_ext_end(unsigned source);

/*
 * Persistence (km_ledstore.h). Without m5_led_persist_attach() the build has none (km_l41*): a LED_SET with the save flag is answered
 * UNSUPPORTED and nothing changes.
 *   attach   call once at boot AFTER m5_led_init() and BEFORE the main loop: reads the newest valid record from `log` and applies it
 *            (effect, brightness, speed, colour, idle-off time); no record or an invalid one leaves the built-in defaults.
 *            after_flash is called after every flash operation (the CPU was stalled): restart the scan chain there. NULL log = detach.
 *   pass     once per main-loop pass, with the whole-second uptime; true = one flash operation was done in this pass
 *   urgent   true while a save that has already erased the sector must be given the next pass
 */
void m5_led_persist_attach(const km_clog_t *log, void (*after_flash)(void *ctx), void *ctx);
bool m5_led_persist_pass(uint32_t now_s, bool idle_now, bool idle_long);
/*
 * Lights per key profile (km_ledprof.h, LIGHT_PROFILE_NOTES.md). Active only when the log given to m5_led_persist_attach() is the 64-byte
 * table log (km_ledprof_flash()); with the 16-byte log of the older images there is one set of lights and these calls change nothing.
 *   set_legacy   before attach: the version-1 reader of the same sector (km_ledstore_flash()), for the boot migration of an old record
 *   set_profile  when the key profile in force changes (and once after attach at boot): the lights of that profile are applied. RAM only,
 *                never touches flash. A profile without its own stored lights follows the global entry; with neither it shows the built-in defaults.
 * The per-profile LED_GET / LED_SET of the configuration protocol (km_config.h) are served by m5_led_cfg_ex_ops().
 */
void m5_led_persist_set_legacy(const km_clog_t *legacy);
void m5_led_set_profile(unsigned profile);
bool m5_led_persist_urgent(void);

#endif /* M5_LED_H */
