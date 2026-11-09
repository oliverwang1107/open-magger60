/*
 * km_led_guard.h - kill switch policy for the LED hardware (M5 task L5). Original work, pure logic: no hardware, time in ms.
 *
 * The rule behind it: the lights are decoration, the key path is the product. Whatever goes wrong with the LED hardware or code,
 * typing must keep working. This module decides when the LED output is stopped:
 *
 *   boot loop   Before the LED hardware is touched the guard records "starting" in a small record that lives in RAM that
 *               survives a reset. If the previous boot started the hardware and never got to "stable" (the LED code hung, the
 *               watchdog reset the keyboard), that is a strike; KM_LED_GUARD_MAX_STRIKES strikes in a row switch the LED off for
 *               the whole boot, so the keyboard stops resetting and types again. The record carries the build id: a new image
 *               starts clean. A power cycle (RAM lost) starts clean too.
 *   faults      While the health monitor reports a fault (scan stall, DMA error, calibration lost, engine overrun) the LED is
 *               paused (black, no work). Each new fault episode after the hardware started counts; KM_LED_GUARD_MAX_EPISODES
 *               episodes switch the LED off for this boot. An episode count is forgiven after KM_LED_GUARD_FORGIVE_MS of calm.
 *   DMA         km_led_guard_dma_result(timeout) : KM_LED_GUARD_MAX_DMA_TIMEOUTS timeouts in a row switch the LED off.
 *
 * "Off" is a latch (tripped): the caller releases the pins (led_hw_kill) and never starts the LED again before the next boot.
 */
#ifndef KM_LED_GUARD_H
#define KM_LED_GUARD_H

#include <stdbool.h>
#include <stdint.h>

#define KM_LED_GUARD_MAX_STRIKES       2u
#define KM_LED_GUARD_MAX_EPISODES      3u
#define KM_LED_GUARD_MAX_DMA_TIMEOUTS  3u
#define KM_LED_GUARD_STABLE_MS         3000u     /* running this long without a fault = the start worked */
#define KM_LED_GUARD_FORGIVE_MS        60000u

/* why the LED is (or is not) off, published in the scope diagnostics */
#define KM_LED_WHY_NONE        0u
#define KM_LED_WHY_BOOT_LOOP   1u    /* two boots in a row did not get past the hardware start */
#define KM_LED_WHY_FAULTS      2u    /* too many fault episodes */
#define KM_LED_WHY_DMA         3u    /* the LED DMA kept timing out */
#define KM_LED_WHY_HW_START    4u    /* the hardware start reported failure */
#define KM_LED_WHY_MANUAL      5u

/* The record in RAM that survives a reset (the caller places it; 20 bytes). */
typedef struct {
    uint32_t magic;
    uint32_t build;          /* id of the running image */
    uint32_t strikes;
    uint32_t state;          /* KM_LED_REC_* */
    uint32_t check;
} km_led_rec_t;

#define KM_LED_REC_OK        0u
#define KM_LED_REC_STARTING  1u
#define KM_LED_REC_DISABLED  2u

typedef enum { KM_LED_RUN = 0, KM_LED_PAUSE, KM_LED_KILL } km_led_verdict_t;

typedef struct {
    km_led_rec_t *rec;       /* NULL = no persistence (the boot-loop check is skipped) */
    bool     boot_allowed;   /* the boot check did not disable the LED */
    bool     hw_started;
    bool     stable;
    bool     tripped;
    bool     in_fault;
    uint8_t  why;            /* KM_LED_WHY_* */
    uint8_t  episodes;
    uint8_t  dma_timeouts;   /* in a row */
    uint32_t strikes;        /* as read at boot */
    uint32_t start_ms;       /* when the hardware started, restarted by every fault episode */
    uint32_t calm_since_ms;
} km_led_guard_t;

/* At boot, before anything else. `rec` points at the RAM record (garbage after a power cycle is fine), `build` identifies the image. */
void km_led_guard_boot(km_led_guard_t *g, km_led_rec_t *rec, uint32_t build);
/* May the LED hardware be started at all in this boot? */
bool km_led_guard_may_start(const km_led_guard_t *g);
/* Call right BEFORE the first access to the LED hardware. */
void km_led_guard_hw_starting(km_led_guard_t *g, uint32_t now_ms);
/* The hardware start itself failed (reported by the driver): trips the guard. */
void km_led_guard_hw_failed(km_led_guard_t *g);
/* Every main-loop pass once the hardware is started. `faults` are the health fault flags that matter (USB suspend is not one). */
km_led_verdict_t km_led_guard_step(km_led_guard_t *g, uint32_t now_ms, uint8_t faults);
/* A frame transfer ended: timed_out = it had to be aborted. */
void km_led_guard_dma_result(km_led_guard_t *g, bool timed_out);
/* Trip by hand (tests, a future host command). */
void km_led_guard_trip(km_led_guard_t *g, uint8_t why);

/* 32-bit id of a build string such as __DATE__ __TIME__. */
uint32_t km_led_build_id(const char *s);

#endif /* KM_LED_GUARD_H */
