/*
 * km_scope.h - "scope" channel: a read-only debug command on the vendor-class HID channel that publishes up to 128 u16 values (sensor readings,
 * timings, counters) from our own firmware to the PC. Hardware independent (M3_PLAN.md section 2).
 *
 * Producer (main loop, never an interrupt):   km_scope_publish(tag, values, count);
 * Consumer (host, via the vendor channel):    command KM_CMD_SCOPE, answered by km_vendor_handle -> km_scope_handle.
 * Both run in the same context (the main loop), so a reply is always one consistent snapshot. A snapshot that is
 * read in several pages can still change between two pages; every page carries `seq` so the host sees that and
 * re-reads.
 *
 * Command id 0xC5. Why it is safe:
 *   - the command ids of the stock firmware's protocol (km_hid.h) are 0x01..0x0E, 0xA0..0xAA, 0xB1, 0xDD, 0xDE, 0xE0,
 *     0xE1, 0xEE, 0xF1, 0xF2, and the bootloader commands are 0x80..0x84, so 0xC5 collides with none of them. The
 *     host tool refuses to send it unless the cmd-3 build string says our firmware (KMFW), so it is never aimed
 *     at a keyboard running other firmware.
 *   - the handler only reads RAM and writes the reply buffer. It writes no flash and no registers.
 *
 * Request  ('U' packet as for every read; len = most reply data bytes the host accepts, 0..56; data bytes are zero):
 *   offset (LE24) = start | (want_tag << 8)     start = index of the first value (0..255)
 *                                               want_tag = 0 for "whatever is published", else the tag the host expects
 * Reply    (0xAA, cmd 0xC5, layout of km_hid_build_response):
 *   offset field  = start (echo)
 *   data[0..1]    tag    LE16  tag of the published snapshot (0 = nothing published yet)
 *   data[2..3]    seq    LE16  publish counter, 1.. (wraps 0xFFFF -> 1; 0 = nothing published yet)
 *   data[4..5]    count  LE16  number of values in the snapshot (0..128)
 *   data[6..]     values LE16, value[start], value[start+1], ... as many as fit: min(25, (len-6)/2, count-start).
 *                 len < 8 (and len 0 = "peek") gives the header only. A want_tag that is not the published tag also
 *                 gives the header only (the header names the tag that IS published).
 * 76 values need 4 requests, 128 values need 6. The host must check that tag and seq are the same in every page.
 *
 * Capture ops (M6 task P12a, diagnostic images only; km_capture.h): want_tag 0xC000..0xCFFF is NOT a snapshot tag but an
 * operation on the raw-sample capture that the image registered with km_scope_set_capture(). Backward compatible in both
 * directions: the 16-bit tags that firmware publishes stay below 0xC000, a request for 0xC000.. to firmware without a
 * registered capture (or without this code) is answered like any tag that is not published (header only, the header names
 * the tag that IS published), and every request that was valid before is answered exactly as before.
 *
 *   want_tag    request                                             data the reply carries (after the 6-byte header)
 *   0xC000      status                                              the KM_CAPTURE_STATUS_WORDS status words
 *   0xC1ii      select: column ii (0..3) := slot `start` (0..75),   the status words (after the change)
 *               start 0xFF = "no slot here, the list ends"
 *   0xC200      control: start 0 = stop, 1 = arm one-shot,          the status words (after the change)
 *               2 = arm ring
 *   0xC3pp      page: page number = pp << 8 | start; the values     values [25 * page .. 25 * page + 24] of the recording
 *               are read 25 at a time
 * Header of a capture reply: tag = the want_tag asked for, seq = capture run counter (changes with every arm: a read that
 * sees it change must start again), count = number of values of this op (status: 20; page: all readable values, 0 while
 * the capture is running). The offset field echoes `start` as always. select and control are the only requests that change
 * anything, and only the RAM state of the capture; they are refused (state unchanged, reply tells) while a run is going
 * (select) or for an arm without slots (control). Nothing is ever written to flash or to a register.
 */
#ifndef KM_SCOPE_H
#define KM_SCOPE_H

#include <stdbool.h>
#include <stdint.h>

#include "km/km_hid.h"

#define KM_CMD_SCOPE            0xC5u
#define KM_SCOPE_MAX_VALUES     128u
#define KM_SCOPE_HEADER_BYTES   6u
#define KM_SCOPE_PAGE_VALUES    ((KM_HID_MAX_DATA - KM_SCOPE_HEADER_BYTES) / 2u)   /* 25 */

/* Well-known tags (M3_PLAN.md section 2). */
#define KM_SCOPE_TAG_S1_SINGLE  0x0001u   /* S1: single polled readings */
#define KM_SCOPE_TAG_S2_RAW     0x0002u   /* S2: 76 raw values of the polled scan */
#define KM_SCOPE_TAG_S3_SUMS    0x0003u   /* S3: 76 sixteen-point sums */
#define KM_SCOPE_TAG_DIAG_BASE  0x0100u   /* 0x0100.. diagnostics */

#define KM_SCOPE_CAP_FIRST      0xC000u   /* want_tag range of the capture ops (0xC000..0xCFFF) */
#define KM_SCOPE_CAP_STATUS     0xC000u
#define KM_SCOPE_CAP_SELECT     0xC100u   /* | column */
#define KM_SCOPE_CAP_CONTROL    0xC200u
#define KM_SCOPE_CAP_PAGE       0xC300u   /* | page >> 8 ; start = page & 0xFF */
#define KM_SCOPE_CAP_STATUS_MAX 25u       /* most status words (one page) */

/* The capture behind the ops, as plain callbacks so that this library needs no other (km_capture_scope.h has the standard set). */
typedef struct {
    bool (*select)(void *ctx, unsigned column, unsigned slot);
    bool (*control)(void *ctx, unsigned ctrl);
    unsigned (*status)(void *ctx, uint16_t out[KM_SCOPE_CAP_STATUS_MAX]);   /* returns the number of words written */
    unsigned (*value_count)(void *ctx);
    unsigned (*read)(void *ctx, unsigned first, unsigned n, uint16_t *out);
    unsigned (*run)(void *ctx);                                              /* capture run counter, goes in `seq` */
} km_scope_capture_t;

/* Register (or, with NULL, remove) the capture. ops must outlive the registration. Without one the capture ops do not exist. */
void km_scope_set_capture(const km_scope_capture_t *ops, void *ctx);

/* Replace the snapshot. count > 128 keeps the first 128 values; values == NULL with count > 0 is ignored.
 * tag 0 is reserved ("nothing published") and is ignored. count 0 is a valid, empty snapshot. */
void km_scope_publish(uint16_t tag, const uint16_t *values, uint16_t count);

/* Forget the snapshot (back to "nothing published", seq 0). Used by tests and at start-up. */
void km_scope_reset(void);

/* Answer a decoded KM_CMD_SCOPE request. Returns true and fills `out`. */
bool km_scope_handle(const km_hid_req_t *req, uint8_t out[KM_HID_REPORT_LEN]);

#endif
