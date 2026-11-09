/*
 * km_capture.h - raw sample capture for noise measurements (M6 task P12a, M6_PLAN.md section 6.3).
 * Original work. DIAGNOSTIC IMAGE ONLY: nothing in a daily-use image calls this file.
 *
 * What it does: records the RAW 12-bit ADC sample of up to four chosen slots, frame by frame, into a RAM buffer, so that
 * the PC can look at what the sensor really does between two frames (about 40 us apart): the standard deviation, the
 * spectrum, spikes, drift. The scope channel only shows 16-sample sums about ten times a second, which is blind to all
 * of that.
 *
 * Where it sits: the owner calls km_capture_push_frame() once per sensor frame in the main loop, right where it feeds
 * km_scan_push_phase() (the DMA words are still valid until adc_scan_release_frame()). It only READS the DMA words and
 * writes its own buffer: nothing here touches the scan chain, a register, the flash or the key path, and it costs a few
 * loads and stores per frame (<= 4 slots). It never blocks and never allocates.
 *
 * Buffer: KM_CAPTURE_SAMPLES u16 in total. With n slots chosen a capture holds KM_CAPTURE_SAMPLES / n frames
 * (4 slots: 4096 frames = about 0.17 s; 1 slot: 16384 frames = 0.7 s). The values are stored frame-major:
 * value[f * n + i] = slot i of frame f. A stored value is the 12-bit sample in bits 0..11 and a one-bit condition flag
 * in bit 15 (bits 12..14 are zero). The flag is whatever condition the caller wants to correlate noise with (the
 * diagnostic image sets it for a frame in which a USB packet was handled).
 *
 * Modes: ONE-SHOT records until the buffer is full and then stops (state DONE); RING keeps overwriting the oldest
 * frame until it is stopped (the readout then starts at the oldest frame). Reading is only allowed when the capture is
 * not running, so a read can never see a half-written frame.
 *
 * The host side is the vendor-channel scope command (km_scope.h, capture ops) and tools/magger60_noise.py.
 */
#ifndef KM_CAPTURE_H
#define KM_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

#define KM_CAPTURE_MAX_SEL     4u
#define KM_CAPTURE_SAMPLES     16384u      /* u16 values in the buffer (32 KB) */
#define KM_CAPTURE_NO_SLOT     0xFFu       /* "no slot" marker in a slot list */
#define KM_CAPTURE_NUM_SLOTS   76u         /* slots 0..75 (km_scan numbering) */
#define KM_CAPTURE_FLAG        0x8000u     /* the condition bit of a stored value */
#define KM_CAPTURE_VALUE_MASK  0x0FFFu

typedef enum {
    KM_CAPTURE_IDLE = 0,                   /* nothing recorded (power-up, or stopped before the first frame) */
    KM_CAPTURE_RUNNING = 1,                /* recording */
    KM_CAPTURE_DONE = 2                    /* stopped, the recording can be read */
} km_capture_state_t;

typedef enum {
    KM_CAPTURE_CTRL_STOP = 0,
    KM_CAPTURE_CTRL_ONESHOT = 1,
    KM_CAPTURE_CTRL_RING = 2
} km_capture_ctrl_t;

/* The status block (km_capture_status), u16 words; the host tool decodes exactly this. */
#define KM_CAPTURE_STATUS_VERSION 1u
#define KM_CAPTURE_STATUS_WORDS   20u
enum {
    KM_CAP_ST_VERSION = 0,                 /* KM_CAPTURE_STATUS_VERSION */
    KM_CAP_ST_STATE = 1,                   /* km_capture_state_t */
    KM_CAP_ST_NSEL = 2,                    /* number of slots selected (0..4) */
    KM_CAP_ST_SLOT0 = 3,                   /* 4 words: slot of column 0..3, 0xFF = unused */
    KM_CAP_ST_RUN = 7,                     /* run counter: +1 at every arm (1.., wraps 0xFFFF -> 1; 0 = never armed) */
    KM_CAP_ST_CAP_FRAMES = 8,              /* frames the buffer holds with the current selection (0 when nothing is selected) */
    KM_CAP_ST_FRAMES_LO = 9,               /* frames recorded since the arm (can exceed CAP_FRAMES in ring mode) */
    KM_CAP_ST_FRAMES_HI = 10,
    KM_CAP_ST_AVAIL = 11,                  /* frames that can be read = min(frames recorded, CAP_FRAMES) */
    KM_CAP_ST_T_FIRST_LO = 12,             /* cycle counter value given with the first frame of the run */
    KM_CAP_ST_T_FIRST_HI = 13,
    KM_CAP_ST_T_LAST_LO = 14,              /* ... and with the latest frame */
    KM_CAP_ST_T_LAST_HI = 15,
    KM_CAP_ST_COND_LO = 16,                /* frames recorded with the condition flag set */
    KM_CAP_ST_COND_HI = 17,
    KM_CAP_ST_RING = 18,                   /* 1 = the run is a ring run */
    KM_CAP_ST_CPU_MHZ = 19                 /* cycle counter ticks per microsecond (km_capture_init argument) */
};

typedef struct km_capture {
    uint16_t buf[KM_CAPTURE_SAMPLES];
    uint8_t  sel[KM_CAPTURE_MAX_SEL];      /* chosen slots, KM_CAPTURE_NO_SLOT = unused */
    uint8_t  n_sel;                        /* leading entries of sel[] that are slots */
    uint8_t  state;
    uint8_t  ring;
    uint16_t run;
    uint16_t cpu_mhz;
    uint32_t frames;
    uint32_t cond_frames;
    uint32_t t_first;
    uint32_t t_last;
} km_capture_t;

/* Nothing selected, nothing recorded. cpu_mhz = cycle counter ticks per microsecond (240 on the keyboard), only reported. */
void km_capture_init(km_capture_t *c, uint16_t cpu_mhz);

/* Set column `idx` (0..3) of the selection to `slot` (0..75), or to KM_CAPTURE_NO_SLOT to end the list there.
 * false (and nothing changed) while a run is going, for idx >= 4 or for a slot that is neither 0..75 nor NO_SLOT. */
bool km_capture_select(km_capture_t *c, unsigned idx, unsigned slot);

/* Select a whole list (n = 0..4 distinct-or-not slots). Same rules. */
bool km_capture_set_slots(km_capture_t *c, const uint8_t *slots, unsigned n);

/* STOP, or arm a ONE-SHOT / RING run. Arming needs at least one slot selected, throws away the old recording and
 * counts a new run. STOP on a running capture keeps what was recorded (DONE); STOP while not running changes nothing.
 * Returns false for an unknown command or an arm without slots. */
bool km_capture_control(km_capture_t *c, unsigned ctrl);

/*
 * One sensor frame. phase_words = the 36 DMA words of the four mux phases in order (g_phase_words, phase * 9 + k),
 * inj = the two injected words (g_inj). Slot s lives in word s / 2 (low half for even s, high half for odd s) of the
 * 38 words phase_words[0..35], inj[0], inj[1]. `now` = the cycle counter, `cond` = the condition flag of this frame.
 * Does nothing unless the capture is RUNNING. Returns true when this call recorded a frame.
 */
bool km_capture_push_frame(km_capture_t *c, const uint32_t *phase_words, const uint32_t inj[2], uint32_t now, bool cond);

/* How many frames a run holds with the current selection (0 when nothing is selected). */
unsigned km_capture_capacity_frames(const km_capture_t *c);

/* Values that can be read now: avail frames * n_sel, 0 while RUNNING (a running capture is never readable). */
unsigned km_capture_value_count(const km_capture_t *c);

/* Copy up to n values starting at value index `first` of the readable recording (oldest frame first) into out[].
 * Returns the number copied (0 while RUNNING or past the end). */
unsigned km_capture_read(const km_capture_t *c, unsigned first, unsigned n, uint16_t *out);

/* Fill the status block (KM_CAPTURE_STATUS_WORDS u16). */
void km_capture_status(const km_capture_t *c, uint16_t out[KM_CAPTURE_STATUS_WORDS]);

#endif /* KM_CAPTURE_H */
