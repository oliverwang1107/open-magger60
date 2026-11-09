/*
 * km_hid.h - the HID configuration protocol of the vendor-class channel (packet build/parse), shared by the
 * device firmware and host tools. This is the wire format the stock firmware speaks, and our own commands reuse it; the
 * frame layout is written out in CONFIG_PROTOCOL.md section 1. It is checked against hand-computed
 * packets in tests/test_hid.c and against replies recorded from a real keyboard (tools/magger60_info.py).
 *
 * Transport: report ID 0, 64-byte input and output reports on the vendor-class interface
 * (IF1 / MI_01, usage page 0x01 usage 0x00 on FB49).
 *
 *   [0]    header  'U' (0x55) normal, '_' (0x5F) raw (device skips XOR, checksum and length limit)
 *   [1]    cmd
 *   [2]    XOR key (0 = plain)
 *   [3]    checksum = (len + off0 + off1 + off2 + sum(data)) & 0xFF
 *   [4]    len (<= 56 for 'U')
 *   [5..6] offset, little endian 16 bit; [7] is 0 for the ordinary commands, or the third
 *          offset byte for 0xE0/0xE1 (LE24)
 *   [8..]  data
 *
 * Response: [0] = 0xAA on success (the firmware answers in place in the same 64-byte
 * buffer), 0xAB on a checksum error. Read data starts at [8].
 *
 * Bootloader-mode packets (cmd 0x80..0x84) use a different, header-less layout; see the
 * km_boot_* builders. Nothing in this library sends anything.
 */
#ifndef KM_HID_H
#define KM_HID_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define KM_HID_REPORT_LEN   64u
#define KM_HID_HDR_NORMAL   0x55u  /* 'U' */
#define KM_HID_HDR_RAW      0x5Fu  /* '_' */
#define KM_HID_MAX_DATA     56u    /* 64 - 8 */
#define KM_HID_DATA_OFFSET  8u

#define KM_HID_FLASH_SIZE   0x80000u /* 512 KB; cmd 0xE0 offset is relative to 0x08000000 */

#define KM_HID_RESP_OK      0xAAu
#define KM_HID_RESP_CSUM    0xABu

/* Command IDs of the stock firmware's protocol (frame layout: CONFIG_PROTOCOL.md section 1). */
typedef enum {
    KM_CMD_FAST_ON         = 0x01,  /* purpose unverified */
    KM_CMD_FAST_OFF        = 0x02,
    KM_CMD_GET_INFO        = 0x03,  /* version + build string, read-only */
    KM_CMD_GET_BASE        = 0x04,  /* 0x0802A000 */
    KM_CMD_GET_PROFILE     = 0x05,  /* 0x0802C000, 64 B per profile */
    KM_CMD_SET_PROFILE     = 0x06,  /* can set the boot flag and reboot */
    KM_CMD_GET_DEF_KEYMAP  = 0x07,
    KM_CMD_GET_KEYMAP      = 0x08,
    KM_CMD_SET_KEYMAP      = 0x09,
    KM_CMD_GET_COLORS      = 0x0A,
    KM_CMD_SET_COLORS      = 0x0B,
    KM_CMD_GET_MACRO       = 0x0C,
    KM_CMD_SET_MACRO       = 0x0D,
    KM_CMD_SWITCH_PROFILE  = 0x0E,
    KM_CMD_GET_TRIGGER     = 0xA0,
    KM_CMD_SET_TRIGGER     = 0xA1,
    KM_CMD_GET_DKS         = 0xA2,
    KM_CMD_SET_DKS         = 0xA3,
    KM_CMD_GET_MT          = 0xA4,
    KM_CMD_SET_MT          = 0xA5,
    KM_CMD_GET_TGL         = 0xA6,
    KM_CMD_SET_TGL         = 0xA7,
    KM_CMD_CAL_START       = 0xA8,
    KM_CMD_CAL_END         = 0xA9,
    KM_CMD_GET_CAL         = 0xAA,  /* 0x0802E000 */
    KM_CMD_RESET_CAL       = 0xB1,  /* resets ALL calibration to default */
    KM_CMD_LIGHT_WRITE     = 0xDD,
    KM_CMD_LIGHT_READ      = 0xDE,
    KM_CMD_FLASH_READ      = 0xE0,  /* any flash, 0x08000000 + LE24, no upper limit check */
    KM_CMD_FLASH_WRITE     = 0xE1,  /* any flash INCLUDING the bootloader: never use */
    KM_CMD_FACTORY_RESET   = 0xEE,
    KM_CMD_GET_APPDATA     = 0xF1,
    KM_CMD_SET_APPDATA     = 0xF2
} km_cmd_t;

/* Our own command ids on the vendor channel. None of them collides with the stock ids above or with the
 * bootloader commands below; each is defined next to the module that answers it, this list only keeps the numbers
 * in one place:
 *   0xC5  scope channel, read-only debug values      km_scope.h   (KM_CMD_SCOPE)
 *   0xC6  configuration protocol                     km_config.h  (KM_CMD_CONFIG)
 *   0xC7  light frame stream, PC to keyboard         km_stream.h  (STREAM_PROTOCOL.md), number defined here
 * Not yet assigned: 0xC8 and up. Anything else is answered "unknw" by km_vendor_handle. */
#define KM_CMD_STREAM       0xC7u

/* Bootloader-mode commands (header byte = command, no checksum). */
typedef enum {
    KM_BOOT_WRITE  = 0x80,
    KM_BOOT_ERASE  = 0x81,
    KM_BOOT_VERIFY = 0x82,
    KM_BOOT_END    = 0x83,
    KM_BOOT_DONE   = 0x84
} km_boot_cmd_t;

typedef enum {
    KM_HID_OK = 0,
    KM_HID_ERR_ARG,
    KM_HID_ERR_HEADER,    /* first byte is neither 'U' nor '_' */
    KM_HID_ERR_LEN,       /* data longer than 56 */
    KM_HID_ERR_CHECKSUM,
    KM_HID_ERR_RESPONSE   /* response byte 0 is not 0xAA */
} km_hid_status_t;

typedef struct {
    uint8_t  header;
    uint8_t  cmd;
    uint8_t  xor_key;
    uint8_t  len;
    uint32_t offset;                       /* LE16 for ordinary commands, LE24 for 0xE0/0xE1 */
    uint8_t  data[KM_HID_MAX_DATA];
} km_hid_req_t;

/* Checksum over a plain (not XORed) packet: len + off0 + off1 + off2 + sum(data).
 * data may be NULL (read requests: the length field is set but no data follows). */
uint8_t km_hid_checksum(uint8_t len, uint32_t offset, const uint8_t *data);

/*
 * Build a 64-byte output report (zero padded). 'U' packets get a checksum and, if
 * xor_key != 0, bytes [3..7+len] are XORed with it; '_' packets are never XORed.
 * offset is truncated to 16 bits unless wide_offset is set (0xE0/0xE1).
 * Returns KM_HID_OK, KM_HID_ERR_LEN or KM_HID_ERR_ARG.
 */
km_hid_status_t km_hid_build(uint8_t out[KM_HID_REPORT_LEN], uint8_t header, uint8_t cmd,
                             uint8_t xor_key, uint32_t offset, bool wide_offset,
                             const uint8_t *data, uint8_t len);

/* Convenience builders for the common patterns of the stock firmware's protocol. */
km_hid_status_t km_hid_build_read(uint8_t out[KM_HID_REPORT_LEN], uint8_t cmd, uint16_t offset, uint8_t len);
km_hid_status_t km_hid_build_write(uint8_t out[KM_HID_REPORT_LEN], uint8_t cmd, uint16_t offset,
                                   const uint8_t *data, uint8_t len);
/* Read request for cmd 0xE0. Rejects flash_offset + len beyond the 512 KB flash (ERR_ARG). */
km_hid_status_t km_hid_build_flash_read(uint8_t out[KM_HID_REPORT_LEN], uint32_t flash_offset, uint8_t len);
/* The raw packet the stock firmware understands as "set the boot flag and reboot" (its "enter boot mode"). */
km_hid_status_t km_hid_build_enter_boot(uint8_t out[KM_HID_REPORT_LEN]);

/* Device side: validate and decode a received report. */
km_hid_status_t km_hid_parse(const uint8_t in[KM_HID_REPORT_LEN], km_hid_req_t *req);

/* Device side: build a response report. header is KM_HID_RESP_OK (0xAA) or KM_HID_RESP_CSUM (0xAB); layout as the
 * the keyboard answers (recorded from a real keyboard, cmd 3): [1] cmd, [2] 0, [3] checksum = len + offsets + sum(data),
 * [4] len, [5..7] offset, data from [8]. */
km_hid_status_t km_hid_build_response(uint8_t out[KM_HID_REPORT_LEN], uint8_t header, uint8_t cmd, uint32_t offset,
                                      const uint8_t *data, uint8_t len);

/* Host side: check a response report; on success *data points at byte 8 of the report. */
km_hid_status_t km_hid_check_response(const uint8_t in[KM_HID_REPORT_LEN], const uint8_t **data);

/* True for commands that only read (safe for a backup tool); false for anything else. */
bool km_cmd_is_read_only(uint8_t cmd);

/* ---- bootloader-mode builders (cmd 0x80..0x84), 64-byte zero padded ---- */
km_hid_status_t km_boot_build_erase(uint8_t out[KM_HID_REPORT_LEN], uint16_t vid, uint16_t pid);
/* chunk is at most 32 bytes; file_offset is the byte offset in the update image. */
km_hid_status_t km_boot_build_chunk(uint8_t out[KM_HID_REPORT_LEN], km_boot_cmd_t cmd,
                                    uint32_t file_offset, const uint8_t *chunk, uint8_t len);
km_hid_status_t km_boot_build_finish(uint8_t out[KM_HID_REPORT_LEN], km_boot_cmd_t cmd);

#endif /* KM_HID_H */
