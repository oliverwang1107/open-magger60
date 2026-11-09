/*
 * km_config.h - our own configuration protocol on the vendor-class HID channel (IF1, 64-byte reports), M5 task T-B.
 * Hardware independent: it is pure logic over a small table of callbacks (km_config_ops_t), so it is tested on the
 * host against fake backends. The byte-level format of every request and reply, the error codes and worked examples
 * are in CONFIG_PROTOCOL.md; this header is the C side of the same specification.
 *
 * Command id 0xC6 (next to cmd 3 and the scope command 0xC5, which are untouched). 0xC6 collides with no id of the
 * stock firmware's protocol (0x01..0x0E, 0xA0..0xAA, 0xB1, 0xDD, 0xDE, 0xE0, 0xE1, 0xEE, 0xF1, 0xF2) nor with the bootloader
 * commands (0x80..0x84). The host tools refuse to send it unless the cmd 3 build string says KMFW.
 *
 * The settings are an OPAQUE byte blob of 1..8192 bytes here (their format is km_settings's business). What this
 * module guarantees:
 *   - set_settings only ever writes the RAM staging buffer. Nothing reaches flash except through the one
 *     commit_settings callback, which is called by exactly one request (COMMIT) and only when
 *       (a) the request carries the confirmation word "SAVE",
 *       (b) the staging buffer is complete (every byte received, in order, no gaps),
 *       (c) the length and the CRC-32 in the request equal those of the staged bytes,
 *       (d) validate_settings (if any) accepts the bytes.
 *   - every other request that changes persistent or disruptive state (SELECT_PROFILE, RECALIBRATE_REQUEST,
 *     SAVE_CALIBRATION_REQUEST) likewise carries its own 4-byte confirmation word, different for each command.
 *   - a request with a wrong length, an unknown command, a bad field or an out-of-range value gets an error reply
 *     and has NO effect (no half-applied state): all checks happen before the first change.
 *   - paging is sequence-checked: every page of set_settings carries its absolute offset and is accepted only at
 *     the next expected offset; a repeat of the last page is acknowledged without effect; anything else is answered
 *     SEQUENCE together with the next expected offset. get_settings reads from a snapshot taken at offset 0, so all
 *     pages of one read are consistent; pages carry the snapshot id and the whole-blob CRC-32.
 *
 * Everything runs in the main loop (same context as km_vendor_handle). The callbacks may block briefly (a flash
 * write); the integration decides when it is safe (e.g. returns KM_CFG_ERR_BUSY while a key is down).
 */
#ifndef KM_CONFIG_H
#define KM_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "km/km_hid.h"

#define KM_CMD_CONFIG              0xC6u
#define KM_CONFIG_PROTO_VERSION    2u   /* 2 (M6 P8): + DEVICE_STATUS, SCENE_*, feature bits 7..10, two more GET_INFO bytes; every version-1 request is unchanged */
#define KM_CONFIG_MAX_SETTINGS     8192u   /* the real blob is 7944 bytes (km_settings.h, format version 2) or at most 7308 (format 3); the firmware main asserts it fits */
#define KM_CONFIG_MAX_SCENE        1024u   /* the light scene blob (km_lightscene.h: KMLS, at most 214 bytes), staged in its own small buffers */
#define KM_CONFIG_DEVICE_STATUS_BYTES 16u  /* payload of DEVICE_STATUS */
#define KM_CONFIG_GET_PAGE_BYTES   44u    /* most settings bytes in one get_settings reply */
#define KM_CONFIG_SET_PAGE_BYTES   52u    /* most settings bytes in one set_settings request */
#define KM_CONFIG_MAX_DEPTHS       25u    /* most values in one live_depth reply */
#define KM_CONFIG_MAX_SLOTS        128u   /* hard upper bound of the slot index space (the real count is the backend's) */

/* Sub-commands (data byte 0 of a request; echoed in the reply). */
typedef enum {
    KM_CFG_OP_GET_INFO                  = 0x01,
    KM_CFG_OP_GET_SETTINGS              = 0x02,
    KM_CFG_OP_SET_BEGIN                 = 0x03,
    KM_CFG_OP_SET_PAGE                  = 0x04,
    KM_CFG_OP_COMMIT                    = 0x05,
    KM_CFG_OP_SELECT_PROFILE            = 0x06,
    KM_CFG_OP_RECALIBRATE_REQUEST       = 0x07,
    KM_CFG_OP_SAVE_CALIBRATION_REQUEST  = 0x08,
    KM_CFG_OP_LIVE_DEPTH                = 0x09,
    /* protocol 2 (M6 P8): read-only device state (host lock lights, gamepad mode, USB fall-back, profile, layer, stream) */
    KM_CFG_OP_DEVICE_STATUS             = 0x0A,
    /* optional extension (L5 lights, LED_NOTES.md): RAM only, answered UNSUPPORTED unless the backend registered led ops */
    KM_CFG_OP_LED_GET                   = 0x20,
    KM_CFG_OP_LED_SET                   = 0x21,
    /* protocol 2: the light scene blob (km_lightscene.h) with the same paging rules as the settings: GET like GET_SETTINGS, BEGIN like
     * SET_BEGIN, PAGE like SET_PAGE, COMMIT like COMMIT (confirmation word "LITE"); separate small staging buffers */
    KM_CFG_OP_SCENE_GET                 = 0x30,
    KM_CFG_OP_SCENE_BEGIN               = 0x31,
    KM_CFG_OP_SCENE_PAGE                = 0x32,
    KM_CFG_OP_SCENE_COMMIT              = 0x33
} km_cfg_op_t;

/* Status byte of every reply (data byte 2). Also the return type of the backend callbacks. */
typedef enum {
    KM_CFG_OK = 0,
    KM_CFG_ERR_UNKNOWN_OP,     /* 1  sub-command not defined                                         */
    KM_CFG_ERR_BAD_LENGTH,     /* 2  request data length is not the one this sub-command needs        */
    KM_CFG_ERR_BAD_ARG,        /* 3  malformed field (non-zero offset/xor field, zero count, ...)     */
    KM_CFG_ERR_RANGE,          /* 4  value out of range (offset, index, slot)                         */
    KM_CFG_ERR_NEED_CONFIRM,   /* 5  confirmation word missing or wrong                               */
    KM_CFG_ERR_SEQUENCE,       /* 6  set page not at the next expected offset                         */
    KM_CFG_ERR_STALE,          /* 7  get page refers to a snapshot that is gone                       */
    KM_CFG_ERR_TOO_BIG,        /* 8  settings larger than KM_CONFIG_MAX_SETTINGS                      */
    KM_CFG_ERR_NOT_STAGED,     /* 9  nothing (complete) staged                                        */
    KM_CFG_ERR_BAD_CRC,        /* 10 commit length/CRC do not match the staged bytes                  */
    KM_CFG_ERR_INVALID,        /* 11 the validator rejected the staged settings                       */
    KM_CFG_ERR_UNSUPPORTED,    /* 12 this firmware build does not provide it                          */
    KM_CFG_ERR_BUSY,           /* 13 not possible right now (keys down, calibration running); retry   */
    KM_CFG_ERR_FAILED          /* 14 the backend tried and failed (e.g. flash verify)                 */
} km_cfg_status_t;

/* Confirmation words: four ASCII bytes inside the request, a different one for every sub-command. */
#define KM_CFG_CONFIRM_COMMIT       "SAVE"
#define KM_CFG_CONFIRM_PROFILE      "PROF"
#define KM_CFG_CONFIRM_RECALIBRATE  "RCAL"
#define KM_CFG_CONFIRM_SAVE_CAL     "SCAL"
#define KM_CFG_CONFIRM_SCENE        "LITE"

/* Feature bits in the get_info reply: set when the backend provides the callback. */
#define KM_CFG_FEAT_READ_SETTINGS   0x0001u
#define KM_CFG_FEAT_WRITE_SETTINGS  0x0002u
#define KM_CFG_FEAT_SELECT_PROFILE  0x0004u
#define KM_CFG_FEAT_RECALIBRATE     0x0008u
#define KM_CFG_FEAT_SAVE_CAL        0x0010u
#define KM_CFG_FEAT_LIVE_DEPTH      0x0020u
#define KM_CFG_FEAT_LED             0x0040u
#define KM_CFG_FEAT_DEVICE_STATUS   0x0080u   /* protocol 2: set when the backend registered device-status ops */
#define KM_CFG_FEAT_SCENE           0x0100u   /* protocol 2: set when the backend registered scene ops (read + commit) */
#define KM_CFG_FEAT_STREAM          0x0200u   /* protocol 2: command 0xC7 (km_stream) exists in this firmware (km_config_set_extra_features) */
#define KM_CFG_FEAT_HOST_LEDS       0x0400u   /* protocol 2: this firmware captures the host's lock-light state (km_config_set_extra_features) */
#define KM_CFG_FEAT_LED_PROFILE     0x0800u   /* protocol 2 extension: LED_GET / LED_SET take a profile argument (the lights are stored per key profile); set when the extended light ops carry get_p / set_p */
#define KM_CFG_FEAT_EXTRA_MASK      (KM_CFG_FEAT_STREAM | KM_CFG_FEAT_HOST_LEDS)

/* Staging state reported by get_info. */
#define KM_CFG_STAGE_EMPTY      0u
#define KM_CFG_STAGE_RECEIVING  1u
#define KM_CFG_STAGE_COMPLETE   2u

/* Backend: what the firmware (or a test fake) provides. Any callback may be NULL = "not supported" (answered
 * UNSUPPORTED). Callbacks return KM_CFG_OK or an error that is passed on to the host (values above FAILED become FAILED). */
typedef struct {
    void *ctx;
    uint8_t profile_count;     /* valid profile indices are 0..profile_count-1 */

    /* Serialize the CURRENT settings into buf (at most cap bytes); returns the length, 0 = failure. */
    unsigned (*read_settings)(void *ctx, uint8_t *buf, unsigned cap);
    /* Optional format check of staged bytes (magic, version, CRC of the blob itself). NULL = accept any bytes. */
    km_cfg_status_t (*validate_settings)(void *ctx, const uint8_t *blob, unsigned len);
    /* THE ONLY PATH TO FLASH: apply the validated blob and save it. Called only by a complete, confirmed COMMIT. */
    km_cfg_status_t (*commit_settings)(void *ctx, const uint8_t *blob, unsigned len);
    km_cfg_status_t (*select_profile)(void *ctx, unsigned index);
    unsigned (*active_profile)(void *ctx);                       /* 0..profile_count-1 */
    km_cfg_status_t (*recalibrate)(void *ctx);                   /* start a calibration; BUSY if a key is down */
    km_cfg_status_t (*save_calibration)(void *ctx);              /* persist the learned calibration */
    /* Read-only: out[i] = depth (0.01 mm) of slot first+i. RANGE if beyond the real slot count. */
    km_cfg_status_t (*live_depth)(void *ctx, unsigned first, unsigned count, uint16_t *out);
} km_config_ops_t;

/* The optional light settings (ops LED_GET / LED_SET). The six bytes are effect, brightness, speed, red, green, blue (km_led_cfg_t);
 * their meaning and validation belong to the backend, which answers RANGE for a value it does not accept (and then changes nothing).
 * The six-byte form lives in RAM only and is gone at the next reset.
 *
 * Extended form (LED_GET with one argument byte 1, LED_SET with 9 argument bytes), only when the backend registered the extended ops
 * (km_config_set_led_ex_ops; a build without them answers BAD_LENGTH to these two forms, exactly as before they existed):
 *   LED_SET args  [0..5] the six light bytes, [6..7] idle-off time in seconds (u16 little endian, 0 = never), [8] flags
 *                 (KM_CFG_LED_FLAG_SAVE: also keep this configuration in flash; the write happens later, in the background)
 *   reply payload (LED_GET, and LED_SET after it applied) 12 bytes: [0..5] light bytes, [6..7] idle-off seconds, [8] status bits
 *                 (KM_CFG_LED_ST_*), [9] result code of the latest save, [10] saves done, [11] saves failed (both saturate at 255)
 * Per-profile form (only when the backend registered get_p / set_p, feature bit KM_CFG_FEAT_LED_PROFILE; a build without them answers BAD_LENGTH):
 *   LED_GET args  [0] = 1, [1] profile (0..profile_count-1, or KM_CFG_LED_PROFILE_ACTIVE)   -> the 12 byte reply of that profile's lights
 *                 (its own entry, else the global fallback; status bit KM_CFG_LED_ST_PROFILE_OWN tells which)
 *   LED_SET args  the nine bytes above + [9] profile (same encoding). The active profile: exactly the nine-byte LED_SET. Another profile: nothing is
 *                 applied to the lights now, the entry is stored (the SAVE flag is required, else RANGE) and shows when that profile is selected.
 * The protocol layer checks lengths and the flag bits (unknown bits: BAD_ARG, before anything is applied); the backend answers
 * RANGE for bad values and UNSUPPORTED for a save request it can not serve (a build without flash for the lights), changing nothing. */
#define KM_CONFIG_LED_BYTES        6u
#define KM_CONFIG_LED_SET_EX_BYTES 9u
#define KM_CONFIG_LED_GET_EX_BYTES 12u
#define KM_CFG_LED_FLAG_SAVE       0x01u
#define KM_CFG_LED_ST_SAVE_PENDING 0x01u    /* a requested save has not reached flash yet */
#define KM_CFG_LED_ST_SAVE_FAILED  0x02u    /* the latest save failed (or gave up after retries) */
#define KM_CFG_LED_ST_LOADED       0x04u    /* the configuration in force came from flash at boot */
#define KM_CFG_LED_ST_IDLE_OFF     0x08u    /* the lights are faded out (or fading) because the idle time ran out */
#define KM_CFG_LED_ST_PERSISTENT   0x10u    /* this build can keep the configuration in flash */
#define KM_CFG_LED_ST_PROFILE_OWN  0x20u    /* per-profile reply: this profile has its own stored lights (clear = it follows the global fallback) */
#define KM_CONFIG_LED_SET_PROF_BYTES 10u
#define KM_CONFIG_LED_GET_PROF_ARGS  2u
#define KM_CFG_LED_PROFILE_ACTIVE  0xFFu    /* profile argument: the profile in force */
typedef struct {
    void *ctx;
    km_cfg_status_t (*get)(void *ctx, uint8_t v[KM_CONFIG_LED_BYTES]);
    km_cfg_status_t (*set)(void *ctx, const uint8_t v[KM_CONFIG_LED_BYTES]);
} km_config_led_ops_t;
typedef struct {
    void *ctx;
    km_cfg_status_t (*get)(void *ctx, uint8_t v[KM_CONFIG_LED_GET_EX_BYTES]);
    km_cfg_status_t (*set)(void *ctx, const uint8_t v[KM_CONFIG_LED_SET_EX_BYTES]);
    /* optional, per-profile form; both or none. profile is 0..3 or KM_CFG_LED_PROFILE_ACTIVE (the protocol layer has checked the range against profile_count). */
    km_cfg_status_t (*get_p)(void *ctx, unsigned profile, uint8_t v[KM_CONFIG_LED_GET_EX_BYTES]);
    km_cfg_status_t (*set_p)(void *ctx, unsigned profile, const uint8_t v[KM_CONFIG_LED_SET_EX_BYTES]);
} km_config_led_ex_ops_t;

/* DEVICE_STATUS (protocol 2, read-only, no arguments): 16 payload bytes filled by the backend (the protocol layer passes them on untouched):
 *   [0] host lock lights (bit0 Num, bit1 Caps, bit2 Scroll; 0 = off or not told yet)
 *   [1] gamepad mode in the settings (0 off, 1 HID, 2 XInput)      [2] gamepad mode in force (0 none, 1 HID, 2 XInput)
 *   [3] flags: bit0 USB fell back to the plain descriptor, bit1 stream active, bit2 animation playing, bit3 host lock lights known (a report arrived)
 *   [4] active profile   [5] active layer   [6] stream state (km_stream_state_t)   [7] 0
 *   [8..9] stream frames shown (u16, saturates)   [10..11] stream frames lost or rejected (u16, saturates)
 *   [12..13] host LED reports received (u16, saturates)   [14..15] 0 */
#define KM_CFG_DEVST_FALLBACK     0x01u
#define KM_CFG_DEVST_STREAM       0x02u
#define KM_CFG_DEVST_ANIM         0x04u
#define KM_CFG_DEVST_LEDS_KNOWN   0x08u
typedef struct {
    void *ctx;
    km_cfg_status_t (*get)(void *ctx, uint8_t v[KM_CONFIG_DEVICE_STATUS_BYTES]);
} km_config_device_ops_t;

/* The light scene (protocol 2): an opaque blob of 1..KM_CONFIG_MAX_SCENE bytes. The same guarantees as the settings: staging is RAM only;
 * commit_scene is called by exactly one request (SCENE_COMMIT with the word "LITE", a complete staging buffer, matching length and CRC-32,
 * and validate_scene accepting the bytes). read_scene is the live scene, serialised. */
typedef struct {
    void *ctx;
    unsigned (*read_scene)(void *ctx, uint8_t *buf, unsigned cap);
    km_cfg_status_t (*validate_scene)(void *ctx, const uint8_t *blob, unsigned len);   /* NULL = accept any bytes */
    km_cfg_status_t (*commit_scene)(void *ctx, const uint8_t *blob, unsigned len);
} km_config_scene_ops_t;

/* State of one paged blob (the settings, or the scene). */
typedef struct {
    uint32_t snap_crc;
    uint16_t stage_total;      /* declared length of the staged blob */
    uint16_t stage_next;       /* next expected offset */
    uint16_t last_off;         /* the last accepted page, to recognise a repeat */
    uint16_t last_n;
    uint16_t snap_len;         /* length of the snapshot, 0 = none */
    uint8_t  stage_state;      /* KM_CFG_STAGE_* */
    uint8_t  snap_id;          /* id of the latest snapshot, 1..255 (0 before the first); valid only while snap_len != 0 */
} km_cfg_blob_t;

/* State of the protocol handler. Allocate one statically (about 8.2 KB for the settings: staging buffer + read snapshot, plus 2 KB for the scene). */
typedef struct {
    const km_config_ops_t *ops;
    uint8_t  stage[KM_CONFIG_MAX_SETTINGS];
    uint8_t  snap[KM_CONFIG_MAX_SETTINGS];
    km_cfg_blob_t set;         /* paging state of the settings */
    uint32_t commits;          /* successful COMMITs since boot */
    const km_config_led_ops_t *led;   /* NULL = no light settings (km_config_set_led_ops) */
    const km_config_led_ex_ops_t *led_ex;   /* NULL = no extended light form (km_config_set_led_ex_ops) */
    const km_config_device_ops_t *dev;      /* NULL = no DEVICE_STATUS (km_config_set_device_ops) */
    const km_config_scene_ops_t *scene;     /* NULL = no scene ops (km_config_set_scene_ops) */
    uint16_t extra_feat;                    /* KM_CFG_FEAT_EXTRA_MASK bits announced in GET_INFO */
    uint32_t scene_commits;                 /* successful SCENE_COMMITs since boot */
    km_cfg_blob_t sc;                       /* paging state of the scene */
    uint8_t  sc_stage[KM_CONFIG_MAX_SCENE];
    uint8_t  sc_snap[KM_CONFIG_MAX_SCENE];
} km_config_t;

void km_config_init(km_config_t *c, const km_config_ops_t *ops);
/* Register the optional light settings backend (NULL removes it). */
void km_config_set_led_ops(km_config_t *c, const km_config_led_ops_t *led);
/* Register the extended light form (idle-off time, save flag, status); needs the plain ops as well. NULL removes it. */
void km_config_set_led_ex_ops(km_config_t *c, const km_config_led_ex_ops_t *ex);

/* Protocol 2 backends. NULL removes them. */
void km_config_set_device_ops(km_config_t *c, const km_config_device_ops_t *dev);
void km_config_set_scene_ops(km_config_t *c, const km_config_scene_ops_t *scene);
/* Announce firmware facts that are not a callback in GET_INFO: bits of KM_CFG_FEAT_EXTRA_MASK (other bits are ignored). */
void km_config_set_extra_features(km_config_t *c, uint16_t bits);

/* Answer a decoded KM_CMD_CONFIG request. Returns true and fills `out` (every well-formed 'U' packet with this
 * command id is answered, errors included). */
bool km_config_handle(km_config_t *c, const km_hid_req_t *req, uint8_t out[KM_HID_REPORT_LEN]);

/* CRC-32 (IEEE 802.3, reflected, init/xorout 0xFFFFFFFF); crc32("123456789") == 0xCBF43926. */
uint32_t km_config_crc32(const uint8_t *data, size_t len);

#endif
