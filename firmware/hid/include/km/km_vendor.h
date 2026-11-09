/*
 * km_vendor.h - device-side handling of the vendor-class HID channel (IF1, 64-byte reports), hardware independent.
 *
 * M2-D3 implements ONE command: 0x03 (version + build string), answered in the stock firmware's frame layout
 * (CONFIG_PROTOCOL.md section 1; hand-computed in tests/test_vendor.c). Every other command is answered with the "unknw" reply the
 * stock protocol uses for "not supported" and has no effect. In particular none of the commands that write flash, reset
 * calibration or reboot (0x06, 0x09, 0x0B, 0x0D, 0xA1, 0xA3, 0xA5, 0xA7, 0xB1, 0xE1, 0xEE, 0xF2, '_' packets)
 * exists in this firmware. Besides cmd 3 there are our own commands: the read-only scope channel (0xC5, km_scope.h)
 * and, only when km_vendor_set_config() was called, the configuration protocol (0xC6, km_config.h), and, only when
 * km_vendor_set_stream() was called, the light frame stream (0xC7, km_stream.h).
 */
#ifndef KM_VENDOR_H
#define KM_VENDOR_H

#include <stdbool.h>
#include <stdint.h>

#include "km/km_config.h"
#include "km/km_hid.h"
#include "km/km_stream.h"

typedef struct {
    uint16_t    bcd_version;   /* e.g. 0x0104, sent little endian */
    const char *build;         /* e.g. __DATE__ "," __TIME__ "KMFW-K1" (at most 54 characters) */
} km_vendor_info_t;

/* True only for the exact 64-byte packet the stock firmware accepts as "enter the bootloader"
 * ('_' 06 00 52 01 00 00 00 51, rest zero). Every other raw packet, and any variation of this one, is false. */
bool km_vendor_is_enter_boot_request(const uint8_t in[KM_HID_REPORT_LEN]);

/* Register the configuration protocol handler (km_config.h, command 0xC6). Until a handler is registered, 0xC6 is
 * answered "unknw" like every other unknown command, so a firmware that does not use km_config is unchanged.
 * NULL unregisters. */
void km_vendor_set_config(km_config_t *cfg);

/* Register the light frame stream (km_stream.h, command 0xC7). Until one is registered 0xC7 is answered "unknw", so a
 * firmware that does not use the stream is unchanged. NULL unregisters. The caller owns the object, calls km_stream_tick()
 * every main-loop pass and hands km_stream_frame() to the light core. */
void km_vendor_set_stream(km_stream_t *stream);

/* Process one received 64-byte report. Returns true and fills `out` when a reply must be sent. */
bool km_vendor_handle(const km_vendor_info_t *info, const uint8_t in[KM_HID_REPORT_LEN],
                      uint8_t out[KM_HID_REPORT_LEN]);

#endif
