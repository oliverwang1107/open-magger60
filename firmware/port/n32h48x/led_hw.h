/*
 * led_hw.h - LED output hardware: PB8 / SPI5 / DMA1 channel 5 (and PF5, a board line that goes with the lights). M5 task L5.
 * Register values are the ones that drive the LED chain on this keyboard (LED_NOTES.md sections 0 and 2; the evidence level of
 * each is stated there); the code is our own. Nothing in here blocks, waits, or uses interrupts: a frame is one
 * DMA transfer that the main loop starts with led_hw_send() and looks at with led_hw_poll(); a transfer that does not finish in
 * LED_HW_TIMEOUT_CYCLES is aborted by led_hw_poll(). led_hw_kill() gives every pin and the DMA channel back.
 *
 * Build levels (KM_LED_HW_LEVEL, per image):
 *   0  DRY      no register access at all (the functions only keep the books); the image exercises the whole LED pipeline
 *   1  NOPOWER  PB8 + SPI5 + DMA1 ch5; PF5 is NOT touched
 *   2  FULL     as 1, plus PF5 driven low (open drain) for the whole time the lights are in use
 */
#ifndef LED_HW_H
#define LED_HW_H

#include <stdbool.h>
#include <stdint.h>

#define LED_HW_DRY      0
#define LED_HW_NOPOWER  1
#define LED_HW_FULL     2
#ifndef KM_LED_HW_LEVEL
#define KM_LED_HW_LEVEL LED_HW_FULL
#endif

/* A frame (about 808 bytes, ~1.7..2 ms on the wire) must be done well before the next one (20 ms). */
#define LED_HW_TIMEOUT_CYCLES  (5u * 240000u)       /* 5 ms at 240 MHz */

typedef enum { LED_HW_IDLE = 0, LED_HW_BUSY, LED_HW_TIMEOUT } led_hw_state_t;

typedef struct {
    uint32_t sent;            /* frames started */
    uint32_t done;            /* frames that ended by themselves */
    uint32_t timeouts;        /* frames aborted */
    uint8_t  started;
    uint8_t  killed;
    uint8_t  level;
    /* read-backs for the scope diagnostics */
    uint32_t spi_ctrl1, spi_ctrl2, dma_cfg, dma_txnum, rcc_cfg, rcc_cfg2, rcc_apb2;
} led_hw_diag_t;

/* Bring-up: clocks, pins, SPI5, DMA channel 5, and the first frame (`dark`, `len` bytes: all lights off) is started. false = failed
 * (everything is released again). Not blocking. */
bool led_hw_start(const uint8_t *dark, unsigned len, uint32_t now_cycles);
/* Release PB8 (back to a floating input), PF5 (open drain released = high), disable DMA channel 5 and SPI5. Idempotent. */
void led_hw_kill(void);
/* Start sending a frame. false if the previous frame is still on its way (nothing is changed). `frame` must stay valid and unchanged
 * until led_hw_poll() says IDLE. */
bool led_hw_send(const uint8_t *frame, unsigned len, uint32_t now_cycles);
/* IDLE: nothing running. BUSY: a frame is on its way. TIMEOUT: the frame did not finish and was aborted just now (reported once). */
led_hw_state_t led_hw_poll(uint32_t now_cycles);
void led_hw_get_diag(led_hw_diag_t *d);

#endif /* LED_HW_H */
