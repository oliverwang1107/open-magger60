/*
 * km_lightscene.h - the light SCENE (M6 package P6b, M6_PLAN.md sections 2.3 and 4): the part of the lights that is more than the five
 * numbers of km_ledstore (effect, brightness, speed, colour, idle time): indicator rules (a light that shows the host's Caps Lock, the
 * active layer, ...), one colour per key, and the animation settings. Original work, pure logic: no hardware, no time.
 *
 * Where it lives: the scene is GLOBAL (not per profile) and has its own two flash sectors, KM_FLASH_SCENE_A / KM_FLASH_SCENE_B (km_flash_guard.h),
 * written alternately by the existing km_store (one record per sector, power-fail safe, tested at every erase/program step). It is not
 * part of the key settings blob (km_settings) and does not touch the light configuration record (km_ledstore, 0x08036000).
 *
 * ---- The blob "KMLS" version 1 (all multi-byte fields little endian) ----
 * This is what km_lightscene_encode() writes, what the host sends with the scene commands of the configuration protocol (P8) and what
 * the store keeps as its payload.
 *   0   4  magic "KMLS"
 *   4   2  version = 1
 *   6   2  total length in bytes, including the checksum
 *   8   1  number of sections n (0..8)
 *   9   7  reserved, 0
 *   16  .. n sections, each:  [id u8][flags u8 = 0][len u16][len bytes of payload]
 *               ids must be STRICTLY INCREASING (so a section can not appear twice); id < 0x80 = required (a decoder that does not know
 *               it must refuse the whole blob), id >= 0x80 = optional (a decoder that does not know it skips it)
 *   end 4  CRC-32 (IEEE, as zlib.crc32 / km_crc32) of everything before it
 * Sections (a section that is missing means "defaults"; the encoder always writes all three, in this order):
 *   0x01 RULES   1 + 8 * c bytes  (c = 0..KM_LS_MAX_RULES): [c] then c rules of 8 bytes:
 *                  source u8   KM_LS_SRC_* (1..8)
 *                  param  u8   LAYER / PROFILE: 0..3, every other source: 0
 *                  led    u8   0..60, the light that shows it (km_led order)
 *                  flags  u8   bit0-1 what the light does while the condition is FALSE (KM_LS_OFF_KEEP / BLACK / DIM), bit2 blink while TRUE;
 *                              bits 3..7 must be 0
 *                  r g b  u8   colour while the condition is TRUE (before brightness and the three hard limits)
 *                  dim    u8   KM_LS_OFF_DIM only: the base colour is scaled by dim/255; otherwise 0
 *   0x02 KEYS    109 bytes: 16 palette colours (r g b) then, for each of the 61 lights in km_led order, a palette index 0..15
 *   0x03 ANIM    8 bytes: [flags u8: bit0 start by itself, bit1 loop; others 0][clip u8][offset_ms i32, |offset| <= 3 600 000][reserved u16 = 0]
 * Largest blob: 16 + (4 + 1 + 64) + (4 + 109) + (4 + 8) + 4 = 214 bytes (KM_LS_MAX_ENCODED); a decoder accepts up to KM_LS_MAX_DECODE.
 *
 * The decoder is strict: any deviation (length, checksum, version, reserved bytes, ordering, ranges, an unknown required section) refuses
 * the whole blob and leaves the output untouched. Nothing here can raise the brightness limits of km_led: colours are PRE-limit values.
 */
#ifndef KM_LIGHTSCENE_H
#define KM_LIGHTSCENE_H

#include <stdbool.h>
#include <stdint.h>

#include "km/km_led.h"
#include "km/km_store.h"

#define KM_LS_VERSION         1u
#define KM_LS_HEADER_BYTES    16u
#define KM_LS_CRC_BYTES       4u
#define KM_LS_MAX_RULES       8u
#define KM_LS_RULE_BYTES      8u
#define KM_LS_PALETTE         16u
#define KM_LS_KEYS_BYTES      (KM_LS_PALETTE * 3u + KM_LED_COUNT)          /* 109 */
#define KM_LS_ANIM_BYTES      8u
#define KM_LS_MAX_ENCODED     214u
#define KM_LS_MAX_DECODE      1024u                                         /* the size of the protocol's staging buffer */
#define KM_LS_ANIM_OFFSET_MAX 3600000

#define KM_LS_SEC_RULES       0x01u
#define KM_LS_SEC_KEYS        0x02u
#define KM_LS_SEC_ANIM        0x03u

/* indicator sources */
#define KM_LS_SRC_NONE        0u    /* never valid in a rule (a rule is either there or not) */
#define KM_LS_SRC_HOST_NUM    1u    /* the host's Num Lock */
#define KM_LS_SRC_HOST_CAPS   2u    /* Caps Lock */
#define KM_LS_SRC_HOST_SCROLL 3u    /* Scroll Lock */
#define KM_LS_SRC_LAYER       4u    /* the active layer is `param` */
#define KM_LS_SRC_PROFILE     5u    /* the active profile is `param` */
#define KM_LS_SRC_PAD         6u    /* the gamepad output is on */
#define KM_LS_SRC_TOGGLE      7u    /* at least one Toggle key is on */
#define KM_LS_SRC_EXTERNAL    8u    /* an external frame (stream or animation) is being shown */
#define KM_LS_SRC_LAST        8u

/* what the light does while its condition is false */
#define KM_LS_OFF_KEEP        0u    /* the colour of the layers below */
#define KM_LS_OFF_BLACK       1u
#define KM_LS_OFF_DIM         2u    /* the colour of the layers below, scaled by `dim`/255 */
#define KM_LS_FLAG_OFF_MASK   0x03u
#define KM_LS_FLAG_BLINK      0x04u
#define KM_LS_FLAG_MASK       0x07u

#define KM_LS_ANIM_AUTOSTART  0x01u
#define KM_LS_ANIM_LOOP       0x02u
#define KM_LS_ANIM_MASK       0x03u

typedef struct {
    uint8_t      source;
    uint8_t      param;
    uint8_t      led;
    uint8_t      flags;
    km_led_rgb_t on;
    uint8_t      dim;
} km_ls_rule_t;

typedef struct km_lightscene {
    uint8_t      n_rules;
    km_ls_rule_t rule[KM_LS_MAX_RULES];
    km_led_rgb_t palette[KM_LS_PALETTE];
    uint8_t      key_idx[KM_LED_COUNT];            /* palette index of every light (effect CUSTOM) */
    uint8_t      anim_flags;                       /* KM_LS_ANIM_* */
    uint8_t      anim_clip;
    int32_t      anim_offset_ms;
} km_lightscene_t;

/* No rules; palette 0 = the default light colour (#0096FF), the others dark; every light on palette entry 0; animation: no autostart,
 * no loop, clip 0, offset 0. */
void km_lightscene_default(km_lightscene_t *s);
bool km_ls_rule_valid(const km_ls_rule_t *r);
bool km_lightscene_valid(const km_lightscene_t *s);

/* Serialise into buf (cap bytes). Returns the length, 0 = invalid scene or cap too small. */
unsigned km_lightscene_encode(const km_lightscene_t *s, uint8_t *buf, unsigned cap);
/* Parse a blob strictly. false = refused, *s untouched. Sections that are missing leave the defaults of km_lightscene_default(). */
bool km_lightscene_decode(km_lightscene_t *s, const uint8_t *buf, unsigned len);

/* ---- on the two-sector store ---- */
/* The newest valid record of the store, decoded. false = none, damaged or not a scene (the caller keeps the defaults); *s untouched then. */
bool km_lightscene_load(const km_store_t *st, km_lightscene_t *s);
/* Encode, then km_store_write: one erase of the sector that does not hold the newest record plus about 30 8-byte programs, read back and
 * compared; the old record stays the valid one until the new one is complete. KM_STORE_ERR_ARG for an invalid scene; KM_STORE_UNCHANGED
 * when the newest record is byte-identical. SYNCHRONOUS: the caller picks the moment (no key down, USB quiet) and restarts the scan chain
 * afterwards, as for the key settings. */
km_store_result_t km_lightscene_save(const km_store_t *st, const km_lightscene_t *s);

#endif /* KM_LIGHTSCENE_H */
