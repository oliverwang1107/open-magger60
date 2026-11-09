/*
 * km_settings.h - the keyboard's persistent settings: our own versioned binary format (M5 task T-D, version 2: task L4,
 * version 3: M6 task P0). Original work; the layout below is the whole specification. core/SETTINGS_NOTES.md explains the
 * decisions behind it in prose.
 *
 * Which version is written. km_settings_encode() writes the LOWEST version that can hold the settings: version 2 (the fixed
 * layout below) when nothing in the settings needs a version 3 feature, version 3 otherwise (km_settings_needs_v3()). So
 * somebody who never uses a new feature can go back to an older firmware without the settings being refused. Version 1 is only
 * read. All three versions are read.
 *
 * Version 2:
 *   offset  size  field
 *   0       4     magic "KMCS" (4B 4D 43 53)
 *   4       2     format version, little endian (2)
 *   6       2     total length of the blob including the CRC (7944 for version 2)
 *   8       1     active profile (0..KM_PROFILES-1)
 *   9       1     number of profiles   (4)   \  shape of the data, so a blob made for another shape is
 *   10      1     number of layers     (4)    > refused by name instead of being misread
 *   11      1     number of slots      (76)  /
 *   12      4 x 1982  the profiles, in order; each profile is
 *                 76 x key config, 14 bytes:  press u16, release u16, mode u8, continuous u8,
 *                                             rt_down u16, rt_up u16, top_dead u16, bottom_dead u16
 *                 layer options, 6 bytes:     tap_term_ms u16, oneshot_ms u16, tap_policy u8, reserved u8 (0)
 *                 4 x 76 layer code bytes (layer 0 first, slot order)
 *                 4 x 76 x 2 argument bytes (layer 0 first, slot order; arg0 then arg1 of each entry)
 *   7940    4     CRC32 (IEEE 802.3, zlib polynomial) of bytes 0..7939
 *
 * The code byte and its two argument bytes are one table entry; km_keymap.h says which codes use the arguments and what
 * they mean (toggle layer, tap-hold, one-shot modifier, the M6 codes), km_keymap_entry_valid() is the rule.
 *
 * Version 3 (M6): the same blob, the same "KMCS" magic, the same CRC and the same transport, but a list of sections (TLV)
 * instead of a fixed layout, and only the 61 slots that carry a key are stored:
 *   offset  size  field
 *   0       4     magic "KMCS"
 *   4       2     format version, little endian (3)
 *   6       2     total length of the blob including the CRC (at most KM_SETTINGS_V3_MAX_SIZE = 8176)
 *   8       1     active profile (0..KM_PROFILES-1)
 *   9       1     number of profiles   (4)   \
 *   10      1     number of layers     (4)    > shape of the data, refused by name when it differs
 *   11      1     number of keys       (61)  /   the slots with km_keymap_slot_is_key(), in slot order
 *   12      1     number of sections n (at most KM_SETTINGS_V3_MAX_SECTIONS)
 *   13      3     reserved, 0
 *   16      ...   n sections, each [id u8][scope u8][length u16 LE][length payload bytes]; scope 0xFF = global, 0..3 = that profile
 *   end-4   4     CRC32 of everything before it
 * Section ids below 0x80 must be understood (an unknown one makes the whole blob refused), ids from 0x80 up are optional and
 * are skipped when unknown. A (id, scope) pair may appear once. The sections this firmware knows (payload sizes are exact; a
 * profile's keys are listed in key order, layer-major for the layer tables):
 *   0x01 KEYCFG   profile  61 x 14 bytes, the same 14 bytes as version 2                                   required, all 4 profiles
 *   0x02 LAYERS   profile  layer options 6 (as version 2) + 4 x 61 code bytes + 4 x 61 x 2 argument bytes     required, all 4 profiles
 *   0x10 DKS      profile  [n u8] + n x 16 (n 0..KM_DKS_MAX): km_dks_rec_t                                   absent = no records
 *   0x11 MORPH    profile  n x 4 (0..KM_MORPH_MAX): km_morph_rec_t                                            absent = none
 *   0x12 SOCD     profile  n x 4 (0..KM_SOCD_MAX): km_socd_pair_t                                             absent = none (all off)
 *   0x20 PAD      profile  4 curves x 4 + 8 option bytes (km_pad_curve_t x 4, km_pad_opts_t)                  absent = the default curves/options
 *   0x30 DEVICE   global   16 bytes (km_device_opts_t)                                                        absent = defaults (gamepad off)
 * Record fields are written one by one in the order of their definition in km_m6_types.h, multi-byte values little endian,
 * reserved bytes 0. The writer emits, for p = 0..3: KEYCFG, LAYERS, then DKS, MORPH, SOCD, PAD if the profile has them (PAD
 * only when it differs from the default), then DEVICE if it differs from the default. The reader accepts any order (and an
 * empty record list or a default PAD / DEVICE section, which the writer would not produce).
 * A slot that carries no key is not stored in version 3: when a version 3 blob is read its key config, codes and arguments
 * are the factory defaults (the engine and km_layers ignore such slots anyway). Largest possible version 3 blob: 7308 bytes.
 *
 * Version 1 (the first format; still READ, never written): 5488 bytes, the same header with version 1, and per profile
 * only the key configs and the 4 x 76 code bytes (1368 bytes). Decoding a version 1 blob migrates it: the code bytes are
 * kept as they are (version 1 allowed 0x00, 0x04..0xE7 and 0xF0..0xFF, all still valid), all argument bytes become 0 and
 * the layer options are the defaults. A blob written by km_settings_encode() after that is version 2 (or 3).
 *
 * A version 2 blob is read with the version 2 rules of table entries (km_keymap_entry_valid_v2): the M6 codes 0xEB..0xEF and
 * the wider tap-hold are not accepted in a version 2 blob, exactly like the firmware that wrote version 2 before M6.
 *
 * All multi-byte values are little endian whatever the host is. Everything is validated on decode; anything that is
 * not exactly a blob we wrote (the stock firmware's settings, 0xFF-erased flash, zeros, random bytes, truncated
 * data, a newer or unknown version) is refused and the caller uses km_settings_default().
 */
#ifndef KM_SETTINGS_H
#define KM_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "km/km_engine.h"   /* km_engine_key_cfg_t: the engine owns the rules for one key config */
#include "km/km_keymap.h"   /* KM_LAYERS, KM_KEYMAP_SLOTS: the keymap owns the rules for one layer code */
#include "km/km_m6_types.h" /* the DKS / Mod-Morph / SOCD / PAD / DEVICE records */

#define KM_PROFILES            4u
#define KM_SETTINGS_VERSION    3u   /* the newest version = the shape of the in-memory km_settings_t */
#define KM_SETTINGS_VERSION_1  1u
#define KM_SETTINGS_VERSION_2  2u
#define KM_SETTINGS_VERSION_3  3u
#define KM_SETTINGS_MAGIC      0x53434D4Bu   /* bytes 4B 4D 43 53 "KMCS" read as a little-endian u32 */

#define KM_SETTINGS_KEYCFG_BYTES   14u
#define KM_SETTINGS_HEADER_BYTES   12u
#define KM_SETTINGS_OPTS_BYTES     6u
#define KM_SETTINGS_V1_PROFILE_BYTES (KM_KEYMAP_SLOTS * KM_SETTINGS_KEYCFG_BYTES + KM_LAYERS * KM_KEYMAP_SLOTS)
#define KM_SETTINGS_V1_SIZE        (KM_SETTINGS_HEADER_BYTES + KM_PROFILES * KM_SETTINGS_V1_PROFILE_BYTES + 4u)   /* 5488 */
#define KM_SETTINGS_PROFILE_BYTES  (KM_SETTINGS_V1_PROFILE_BYTES + KM_SETTINGS_OPTS_BYTES + KM_LAYERS * KM_KEYMAP_SLOTS * KM_KEYMAP_ARGS)
#define KM_SETTINGS_ENCODED_SIZE   (KM_SETTINGS_HEADER_BYTES + KM_PROFILES * KM_SETTINGS_PROFILE_BYTES + 4u)       /* 7944: the version 2 blob, and the largest blob km_settings_encode() writes */

/* version 3 */
#define KM_SETTINGS_V3_HEADER_BYTES   16u
#define KM_SETTINGS_V3_SECTION_HEADER 4u
#define KM_SETTINGS_V3_MAX_SECTIONS   64u
#define KM_SETTINGS_V3_MAX_SIZE       8176u   /* = KM_STORE_MAX_PAYLOAD: one sector minus the store header (asserted in km_settings.c) */
#define KM_SETTINGS_SCOPE_GLOBAL      0xFFu
#define KM_SETTINGS_SEC_KEYCFG        0x01u
#define KM_SETTINGS_SEC_LAYERS        0x02u
#define KM_SETTINGS_SEC_DKS           0x10u
#define KM_SETTINGS_SEC_MORPH         0x11u
#define KM_SETTINGS_SEC_SOCD          0x12u
#define KM_SETTINGS_SEC_PAD           0x20u
#define KM_SETTINGS_SEC_DEVICE        0x30u
#define KM_SETTINGS_SEC_OPTIONAL      0x80u   /* ids from here on may be skipped when unknown */
#define KM_SETTINGS_V3_KEYCFG_BYTES   (KM_KEYMAP_KEYS * KM_SETTINGS_KEYCFG_BYTES)                                   /* 854 */
#define KM_SETTINGS_V3_LAYERS_BYTES   (KM_SETTINGS_OPTS_BYTES + KM_LAYERS * KM_KEYMAP_KEYS * (1u + KM_KEYMAP_ARGS)) /* 738 */
#define KM_SETTINGS_DKS_BYTES         16u
#define KM_SETTINGS_MORPH_BYTES       4u
#define KM_SETTINGS_SOCD_BYTES        4u
#define KM_SETTINGS_PAD_BYTES         (KM_PAD_CURVES * 4u + 8u)                                                     /* 24 */
#define KM_SETTINGS_DEVICE_BYTES      16u
#define KM_SETTINGS_V3_MAX_ENCODED    7308u   /* every section of every profile present: the most the writer can produce */

typedef struct {
    km_engine_key_cfg_t key[KM_KEYMAP_SLOTS];                  /* indexed by slot; slots without a key are stored (version 2) or defaulted (version 3) but ignored */
    uint8_t             layer_usage[KM_LAYERS][KM_KEYMAP_SLOTS];  /* layer 0 = base; the code byte of every table entry */
    uint8_t             layer_arg[KM_LAYERS][KM_KEYMAP_SLOTS][KM_KEYMAP_ARGS];   /* the entry's argument bytes (0 unless the code uses them) */
    km_layer_opts_t     layer_opts;                               /* tap-hold term / policy, one-shot timeout */
    /* M6 (version 3). The count says how many records are in use; the records after it are ignored (decode leaves them 0). */
    km_dks_rec_t        dks[KM_DKS_MAX];       uint8_t dks_n;       /* KM_CODE_DKS entries refer to these by index (< dks_n) */
    km_morph_rec_t      morph[KM_MORPH_MAX];   uint8_t morph_n;     /* KM_CODE_MOD_MORPH entries refer to these by index (< morph_n) */
    km_socd_pair_t      socd[KM_SOCD_MAX];     uint8_t socd_n;
    km_pad_curve_t      pad_curve[KM_PAD_CURVES];   /* KM_CODE_PAD_AXIS entries refer to these by index; always all 4 */
    km_pad_opts_t       pad_opts;
} km_profile_t;

typedef struct {
    uint8_t      version;   /* KM_SETTINGS_VERSION after a decode or a default, whatever version the blob had */
    uint8_t      active;    /* 0..KM_PROFILES-1 */
    km_profile_t profile[KM_PROFILES];
    km_device_opts_t device;   /* global (version 3 section DEVICE) */
} km_settings_t;

/* Factory defaults: every profile identical (default trigger on every slot, the keymap's default table, no arguments,
 * default layer options, no DKS / Mod-Morph / SOCD records, the default gamepad curves and options, gamepad off). */
void km_settings_default(km_settings_t *s);

/* The default gamepad curves (0.20 mm .. 3.20 mm, linear) and options (dead zone 10 %, circular, smoothing 0). */
void km_settings_pad_default(km_pad_curve_t curve[KM_PAD_CURVES], km_pad_opts_t *opts);

/* The rule for one key config, one layer code byte, one whole table entry (code + arguments) and the layer options;
 * the same rules decode and encode apply. km_settings_entry_valid is the rule of the current firmware (version 3). */
bool km_settings_cfg_valid(const km_engine_key_cfg_t *cfg);
bool km_settings_usage_valid(uint8_t usage);
bool km_settings_entry_valid(uint8_t code, uint8_t arg0, uint8_t arg1);
bool km_settings_opts_valid(const km_layer_opts_t *opts);

/* The rules of the M6 records (the same ones decode and encode apply; the other modules may call them too):
 *   DKS     t_act 40..340 (KM_DKS_ACT_MIN_MM100); t_bot 0 (= 310) or t_act + 40 .. 340 (with t_bot 0, t_act <= 270); flags only
 *           bit0; rsvd 0; each of the 4 actions has a usage 0x04..0xE7, or usage 0 with ev 0
 *   MORPH   mod_mask not 0; usage 0x04..0xE7; flags only bit0; rsvd 0
 *   SOCD    two different slots that carry a key; mode 0..4; flags only bit0
 *   PAD     curve: start 4..64, end start + 4 .. 68, shape 0..3, flags only bit0; options: dead zone 0..50, circular 0/1,
 *           smooth 0..3, rsvd 0
 *   DEVICE  pad_mode 0..2, filter_mode 0..2, rsvd 0 */
bool km_settings_dks_valid(const km_dks_rec_t *r);
bool km_settings_morph_valid(const km_morph_rec_t *r);
bool km_settings_socd_valid(const km_socd_pair_t *p);
bool km_settings_pad_curve_valid(const km_pad_curve_t *c);
bool km_settings_pad_opts_valid(const km_pad_opts_t *o);
bool km_settings_device_valid(const km_device_opts_t *d);

/* True if every field of s is valid: every key config, every table entry (km_keymap_entry_valid, and a DKS / Mod-Morph index
 * must point at an existing record), the layer options, the counts, every record, every SOCD slot used in at most one pair of
 * a profile, the device options. */
bool km_settings_valid(const km_settings_t *s);

/* True when s holds something a version 2 blob cannot: a table entry that only version 3 accepts (km_keymap_entry_needs_v3),
 * a DKS / Mod-Morph / SOCD record, gamepad curves or options that differ from the default, device options that differ from the
 * default. Meaningful for valid settings. */
bool km_settings_needs_v3(const km_settings_t *s);

/* The slots that carry a key (km_keymap_slot_is_key), in slot order: writes KM_KEYMAP_KEYS slot numbers to out, returns how
 * many (61). This is the order of a version 3 blob's per-key data. */
unsigned km_settings_key_slots(uint8_t out[KM_KEYMAP_KEYS]);

/* Serialize with the lowest version that holds s (see above). Returns the number of bytes written (KM_SETTINGS_ENCODED_SIZE for
 * version 2, less for version 3), or 0 if cap is too small or s holds an invalid value (invalid settings are never written
 * out). A version 3 blob does not store the slots without a key. */
unsigned km_settings_encode(const km_settings_t *s, uint8_t *buf, unsigned cap);

/* The size km_settings_encode() would write (0 for invalid settings): a protocol layer can check its buffer first. */
unsigned km_settings_encoded_size(const km_settings_t *s);

/* The two writers separately. _v2 returns 0 when the settings need version 3; _v3 always writes the section format. */
unsigned km_settings_encode_v2(const km_settings_t *s, uint8_t *buf, unsigned cap);
unsigned km_settings_encode_v3(const km_settings_t *s, uint8_t *buf, unsigned cap);

/* Parse and validate exactly len bytes (version 1, 2 or 3). On false *s is left untouched. Nothing here can read outside
 * [buf, buf + len) or fail in any way except by returning false. */
bool km_settings_decode(km_settings_t *s, const uint8_t *buf, unsigned len);

/* CRC32 (IEEE, reflected, init/xorout 0xFFFFFFFF); init = 0 for a fresh computation, pass the previous result to continue. */
uint32_t km_crc32(uint32_t crc, const uint8_t *data, unsigned len);

#endif /* KM_SETTINGS_H */
