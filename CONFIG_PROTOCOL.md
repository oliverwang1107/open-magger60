# Configuration protocol (command 0xC6), version 2

The settings channel of the custom firmware. It runs on the vendor-class HID interface (IF1, 64-byte reports, report
ID 0) next to the version query (command 3) and the read-only scope channel (command 0xC5, `km_scope.h`); both keep
working unchanged. The device side is `firmware/hid/src/km_config.c` (pure logic over a table of callbacks, tested on
the host with a fake backend in `firmware/tests/test_config.c`); the host side is `tools/magger60_config.py` and
`ui/js/device/ConfigClient.js`. This file is the specification all three are checked against: the packets in section 7
are written out byte by byte in `firmware/tests/golden/config_vectors.h` and every implementation must reproduce them.

**Version 2 (M6 P8) only adds**: the op `0x0A DEVICE_STATUS`, the scene ops `0x30..0x33`, feature bits 7..10 and two more bytes at
the end of the GET_INFO reply (sections 3 and 7a). Every version-1 request and reply is byte-for-byte what it was: a host that
speaks version 1 works with a version-2 keyboard (it reads the 18 bytes it knows and ignores the rest), and a version-2 host
works with a version-1 keyboard (it sees `proto_version` 1, no new feature bits, and does not send the new ops).

Everything here is original to this project. It does not use, and must never be sent to, the stock firmware: the
host tools check that the command-3 build string carries `KMFW` before they send a single 0xC6 packet.

## 1. Frame

A request is an ordinary `'U'` packet (see `km_hid.h`):

```
[0]    0x55            header 'U'
[1]    0xC6            command (KM_CMD_CONFIG)
[2]    0x00            XOR key, must be 0 (a masked request is refused: BAD_ARG)
[3]    checksum        (len + off0 + off1 + off2 + sum(data[0..len-1])) & 0xFF
[4]    len             number of data bytes, 0..56
[5..7] offset          LE24, must be 0 (otherwise BAD_ARG)
[8..]  data            len bytes, then zero padding to 64 bytes
```

A reply is the standard response frame: `[0] = 0xAA`, `[1] = 0xC6`, `[2] = 0`, `[3]` checksum (same formula),
`[4]` = number of data bytes, `[5..7]` = 0, data from `[8]`, zero padding. A request with a bad checksum gets the usual
`0xAB` / `"cscr"` reply from the vendor dispatcher and never reaches this module. A raw `'_'` packet is never answered.
A keyboard that does not know the command (stock firmware, or a build without it) answers `"unknw"`.

### Request data

```
data[0]     op          sub-command (section 3)
data[1]     token       any byte; echoed in the reply, so the host can tell a stale or duplicated reply from the answer
data[2..]   arguments   exactly the bytes the sub-command defines (a request that is longer or shorter: BAD_LENGTH)
```

### Reply data

```
data[0]     op          echo of the request's op (0 if the request had none)
data[1]     token       echo of the request's token (0 if the request had none)
data[2]     status      0 = OK, otherwise an error code (section 4)
data[3..]   payload     as defined per sub-command; empty for every error reply except where stated
```

Integers are little endian. All lengths below are the number of data bytes.

## 2. Rules that hold for every request

- **No partial effect.** All checks (length, fields, ranges, confirmation, sequence, CRC) happen before the first state
  change. A request that is answered with an error changed nothing, except that a rejected `SET_PAGE` reply tells
  where the transfer stands.
- **Persistent state needs a confirmation word.** COMMIT, SELECT_PROFILE, RECALIBRATE_REQUEST and
  SAVE_CALIBRATION_REQUEST carry four ASCII bytes (a different word for each, section 3). It is compared before
  anything else is looked at, so a request without or with a wrong word is always answered `NEED_CONFIRM`, whatever else
  is wrong with it, and has no effect. Flash is reached only through COMMIT (the backend's `commit_settings`).
- **Staging is RAM only.** `SET_BEGIN`/`SET_PAGE` fill a staging buffer of up to 8192 bytes. Nothing staged is used
  until a COMMIT with the right confirmation word, the right length and the right CRC-32 arrives and the validator accepts it.
- **Exact lengths.** The offset field and the XOR key must be 0; the data length must be exactly the one the sub-command
  defines (SET_PAGE: 3..54 bytes).
- **Versioning.** GET_INFO never changes its request; later protocol versions only append payload bytes to its reply and
  add new ops. A host reads `proto_version` first and uses only what that version defines.

## 3. Sub-commands

CRC-32 everywhere is the usual one (IEEE 802.3, reflected, init and final XOR `0xFFFFFFFF`, as zlib's `crc32`);
`crc32("123456789") = 0xCBF43926`.

### 0x01 GET_INFO (read-only)

Request args: none (`len = 2`).

Reply payload (20 bytes; the first 18 are the version-1 reply):

| offset | size | field | meaning |
|---|---|---|---|
| 0 | 1 | `proto_version` | 2 (1 on older firmware) |
| 1 | 1 | `profile_count` | valid profile indices are 0..n-1 |
| 2 | 1 | `active_profile` | 0xFF = the backend cannot tell |
| 3 | 2 | `max_settings` | 8192 |
| 5 | 1 | `get_page_bytes` | most settings bytes per GET_SETTINGS reply (44) |
| 6 | 1 | `set_page_bytes` | most settings bytes per SET_PAGE request (52) |
| 7 | 2 | `features` | bit0 read settings, bit1 write settings (commit), bit2 select profile, bit3 recalibrate, bit4 save calibration, bit5 live depth, bit6 light settings (0x20 / 0x21), **bit7 device status (0x0A), bit8 light scene (0x30..0x33), bit9 stream (command 0xC7, `STREAM_PROTOCOL.md`) exists, bit10 the firmware captures the host lock lights**, **bit11 the lights are stored per key profile (LED_GET / LED_SET take a profile argument)** (bits 7 and 8 mean the op works; 9 and 10 are facts about the firmware) |
| 9 | 1 | `stage_state` | 0 empty, 1 receiving, 2 complete |
| 10 | 2 | `stage_total` | declared length of the staged blob |
| 12 | 2 | `stage_next` | next expected offset of the staging transfer |
| 14 | 4 | `commits` | successful COMMITs since boot (lets a host check a commit whose reply was lost) |
| 18 | 2 | `scene_max` | version 2: largest light scene blob (1024), 0 when bit8 is not set |

### 0x02 GET_SETTINGS (read-only)

Reads the live settings blob in pages from a **snapshot**: a request for offset 0 asks the backend for the current blob
and keeps a copy; the following pages of the same read come from that copy, so a read is consistent even if the
settings change in between. A COMMIT or SELECT_PROFILE drops the snapshot (later pages are `STALE`).

Request args (`len = 6`): `offset u16`, `want u8`, `snap_id u8`.

- `want` = most settings bytes wanted, 1..44 (otherwise BAD_ARG).
- offset 0: `snap_id` must be 0 (otherwise BAD_ARG); a new snapshot is taken.
- offset > 0: `snap_id` must be the id returned by the offset-0 page, and the snapshot must still exist (otherwise
  `STALE`); `offset > total_len` is `RANGE`; `offset == total_len` is OK with no data.

Reply payload: `snap_id u8` (1..255, 0 is never used; counts up with every snapshot), `total_len u16`, `crc32 u32`
(of the whole blob), `offset u16` (echo), then `min(want, total_len - offset)` data bytes (reply `len` = 12 + that).
The host reads pages until it has `total_len` bytes, checks that `snap_id`, `total_len` and `crc32` are the same in
every page, and checks the CRC of what it assembled. If a page says `STALE`, it starts again at offset 0.
`FAILED` = the backend could not produce the settings. `UNSUPPORTED` = no read callback.

### 0x03 SET_BEGIN

Request args (`len = 4`): `total u16`: length of the blob that will follow. 0 → BAD_ARG, over 8192 → `TOO_BIG`,
no commit callback in this build → `UNSUPPORTED`.
Effect: discards whatever was staged and starts a new transfer (`stage_state` = receiving, `stage_next` = 0).
Reply payload: `capacity u16` (8192).

### 0x04 SET_PAGE

Request args (`len = 4 + n`, n = 1..52): `offset u16`, then `n` data bytes.
Sequence rules, checked in this order:

1. nothing begun → `NOT_STAGED`;
2. **repeat of the last accepted page** (same offset, same length, same bytes: its reply was probably lost) → OK, no effect;
3. `offset != stage_next` → `SEQUENCE`; a lost, early, late or duplicated page can never be written anywhere;
4. `offset + n > total` → `RANGE`;
5. otherwise the bytes are copied, `stage_next += n`, and when `stage_next == total` the stage is complete.

Reply payload (for OK, `SEQUENCE` and `RANGE`): `next u16` = the offset the device expects now. A host that gets
`SEQUENCE` resumes from `next` (or restarts with SET_BEGIN); the final CRC check in COMMIT is the safety net for
everything else.

### 0x05 COMMIT

Request args (`len = 12`): confirmation word `"SAVE"` (4 bytes), `len u16`, `crc32 u32`.
Checks, in order: length → confirmation word (`NEED_CONFIRM`) → commit callback exists (`UNSUPPORTED`) → stage is complete
(`NOT_STAGED`) → `len` equals the declared total and `crc32` equals the CRC-32 of the staged bytes (`BAD_CRC`) → the
validator accepts the bytes (`INVALID`; `BUSY` is passed on) → the backend's `commit_settings` (applies and saves; its
`BUSY`, `FAILED`, ... are passed on, and the staging is kept so the host can retry).
Success: the stage is emptied, the read snapshot dropped, the commit counter incremented. Reply payload: `commits u32`
(after this commit). A repeated COMMIT packet is therefore answered `NOT_STAGED` and cannot write twice.
The backend is called from the main loop and may take milliseconds (a flash write); it should answer `BUSY` while a key
is down or the USB endpoint is busy.

### 0x06 SELECT_PROFILE

Request args (`len = 7`): confirmation word `"PROF"`, `index u8`. Index ≥ `profile_count` → `RANGE`. Persistent: the
active profile is part of the saved state, hence the word. Reply payload: `active u8` (as the backend reports it).
Drops the read snapshot.

### 0x07 RECALIBRATE_REQUEST

Request args (`len = 6`): confirmation word `"RCAL"`. Asks the firmware to start a calibration; the backend answers
`BUSY` if that is not possible right now (a key is down). No payload. (The result is not part of this command.)

### 0x08 SAVE_CALIBRATION_REQUEST

Request args (`len = 6`): confirmation word `"SCAL"`. Asks the firmware to persist the learned calibration (the bottomed-out level
every key has learned, kept in its own flash sector and restored at the next boot; design in `firmware/core/CALIBRATION_NOTES.md`).
No payload. Only the persistent build (`KMFW-L33`) provides it: the RAM-only build (`KMFW-L31`) clears feature bit 4 and answers
`UNSUPPORTED`. The firmware's own background save (after a key has learned something new, once the keyboard has been quiet) needs no command;
this one is for "save it now". Replies:
* `OK` - the flash now holds the current learned state, **or** nothing noticeable was learned since the last save / boot (then nothing is
  written: "noticeable" = some key's bottom at least 16 ADC counts deeper than at the last save). The reply does not say which; a host that
  needs to tell can read scope tag 0x0202 (saves ok).
* `BUSY` - not possible now: a key is down, a failure flag is active, the engine is not calibrated, or the previous attempt was less than 10 s
  ago (the firmware refuses to be used as a flash-wear machine). Retry after a moment.
* `FAILED` - the flash refused or the read-back differed; the previously stored calibration is intact.

The command runs to the end inside the request (about 40 flash operations, a fraction of a second at most) and never at the same time as a key
press: it answers `BUSY` instead.

### 0x09 LIVE_DEPTH (read-only)

Request args (`len = 4`): `first u8`, `count u8`; `count` 1..25 (otherwise BAD_ARG); `first + count > 128` → `RANGE`;
the backend may answer `RANGE` for slots beyond the real slot count.
Reply payload: `first u8`, `count u8`, `count` times `depth u16` (0.01 mm, indexed by sensor slot). The values are a
live view; consecutive requests are not a consistent snapshot.

### 0x0A DEVICE_STATUS (version 2, read-only; feature bit7)

Request args: none (`len = 2`; anything else is `BAD_LENGTH`). Reply payload: 16 bytes, filled by the firmware. It reads state and
changes none, needs no word, and cannot start, stop or save anything.

| offset | size | field | meaning |
|---|---|---|---|
| 0 | 1 | `host_leds` | the host's lock lights: bit0 Num, bit1 Caps, bit2 Scroll (0 = all off, or the host has not said yet: see flag bit3) |
| 1 | 1 | `pad_mode_setting` | the gamepad mode in the settings (0 off, 1 HID, 2 XInput-style, M6 P11); a change needs a re-plug |
| 2 | 1 | `pad_mode_active` | the gamepad mode in force right now (0 none, 1 HID) |
| 3 | 1 | `flags` | bit0 USB fell back to the plain descriptor (the host did not accept the gamepad interface within 6 s), bit1 stream active, bit2 animation playing, bit3 host lock lights known (at least one LED report arrived) |
| 4 | 1 | `profile` | active profile |
| 5 | 1 | `layer` | active layer |
| 6 | 1 | `stream_state` | `km_stream_state_t` of the stream receiver (`STREAM_PROTOCOL.md`) |
| 7 | 1 | reserved | 0 |
| 8 | 2 | `stream_frames` | stream frames shown since boot (saturates at 65535) |
| 10 | 2 | `stream_lost` | stream frames lost, stale or rejected (saturates) |
| 12 | 2 | `led_reports` | host LED reports received since boot (saturates) |
| 14 | 2 | reserved | 0 |

`UNSUPPORTED` when the build has no device-status backend (bit7 not set); a backend failure is passed on (anything unknown becomes `FAILED`).

### 0x30 SCENE_GET, 0x31 SCENE_BEGIN, 0x32 SCENE_PAGE, 0x33 SCENE_COMMIT (version 2, the light scene; feature bit8)

The light scene (`km_lightscene.h`: indicator rules, per-key colours, animation settings; the `KMLS` blob, at most 214 bytes) has its
own two flash sectors and is **not part of the settings blob**. The four ops follow **exactly the rules of GET_SETTINGS, SET_BEGIN,
SET_PAGE and COMMIT** (sections above: snapshot read with an id and a CRC, SET_BEGIN sizes the transfer, pages are accepted only at the
next expected offset and a repeat of the last page is acknowledged without effect, COMMIT needs the complete staging buffer and the
length and CRC-32 of what was staged) with these differences:

- they work on **separate buffers** (1024 bytes each for staging and for the read snapshot): a transfer of the scene never disturbs a
  settings transfer or the other way round, and the two have independent snapshot ids and commit counters;
- `SCENE_BEGIN` answers (payload) the scene limit, 1024, and `TOO_BIG` above it; the same u16 is `scene_max` in GET_INFO;
- `SCENE_COMMIT` carries the confirmation word **`LITE`** (the settings word `SAVE` does not commit a scene and `LITE` does not commit
  settings), the reply payload is the scene commit counter (`u32`, successful scene commits since boot), and the firmware's validator is
  `km_lightscene_decode` (a refusal is `INVALID`, the staging stays); the backend applies the scene at once and writes it to its
  own sector (`km_lightscene_save`: a synchronous sector erase, so it answers `BUSY` while a key is down);
- without a scene backend (bit8 not set) all four answer `UNSUPPORTED` after their length and confirmation checks; with a version-1
  firmware they are `UNKNOWN_OP`.

Lost reply to `SCENE_COMMIT`: do **not** send it again blindly (a second one answers `NOT_STAGED`); read the scene back (SCENE_GET)
and compare it with what was sent. `ui/js/device/ConfigClient.js` and `tools/magger60_config.py` do exactly that.

### 0x20 LED_GET and 0x21 LED_SET (optional extension: the lights; the extended form can keep them in flash)

Present only in builds with the lights (`KM_M5_LED`, `LED_NOTES.md`); `GET_INFO` feature bit6 says so, other builds answer
`UNSUPPORTED`. These two ops are outside the settings machinery: **nothing is staged, nothing is committed, and the settings blob
format (`km_settings`) is not involved.** In the plain (six byte) form the values live in RAM and are gone at the next reset; the
extended form below can ask the firmware to keep them in its own flash sector (build `km_l51`, `LED_NOTES.md` section 15). They
carry no confirmation word: the flash write is a background job that is coalesced, rate limited and never runs while a key is down
(section "Saving" below). They cannot raise the brightness limits of the firmware (channel cap, white limit, current budget are
compile-time constants applied after any setting).

LED_GET: request args `len = 2` (none). Reply payload (6 bytes): `effect u8`, `brightness u8`, `speed u8`, `r u8`, `g u8`, `b u8`.

LED_SET: request args (6 bytes), the same six fields. `effect`: 0 off, 1 static colour, 2 breathing, 3 rainbow wave, 4 reactive
(lights the key that goes down, fades after release), 5 depth heat (colour follows the key depth), 6 demo (cycles through 3, 4, 5,
2, 1 every 8 s), **7 custom (one colour per key, from the light scene), 8 external (the stream or an animation), 9 animation (the
built-in clip)**; anything above 9, or `speed == 0`, is `RANGE` and nothing changes (before the layered light core of M6 P6c the limit was
6; the configurator's own check, `LED_EFFECT_MAX` in `ui/js/device/config_protocol.js`, stays 6 until its lights page offers 7..9). `brightness` 0..255 scales the colour down;
`speed` 1..255, bigger is faster; `r g b` is the base colour of the static, breathing and reactive effects. Reply payload: the six
values now in force (as LED_GET). A wrong length is `BAD_LENGTH`. The plain form is unchanged from before the extended form
existed: it never touches the idle time, never asks for a save, and is answered with six bytes.

**Extended form** (backward compatible: an older host keeps using the plain form; an older firmware answers the extended forms
`BAD_LENGTH`, which is how a host detects it):

* LED_GET with one argument byte `1` (`len = 3`): reply payload is 12 bytes: the six light bytes, `idle_s u16` (little endian), then
  `status u8`, `last_result u8`, `saves_ok u8`, `saves_failed u8`. Any other argument value is `BAD_ARG`; two or more argument bytes
  are `BAD_LENGTH`.
* LED_SET with nine argument bytes (`len = 11`): the six light bytes, `idle_s u16` (little endian), `flags u8`. Reply payload: the 12
  bytes of the extended LED_GET, after the change was applied.
  * `idle_s`: seconds without any key activity before the lights fade out (2 s fade), 0 = never; default 600 (10 minutes). Any u16
    value is valid. The first key press brings the lights back (150 ms); so does every LED_SET (a change you make from the host is
    something you want to see, and the idle clock starts again).
  * `flags`: bit0 `SAVE` = also keep this configuration (effect, brightness, speed, colour, `idle_s`) in flash and restore it at the
    next boot. All other bits must be 0, else `BAD_ARG` and nothing is applied. Without `SAVE` the change is live only.
  * A build that can not keep it (every light build except `km_l51`) answers a request with `SAVE` `UNSUPPORTED` and applies nothing,
    so a host never believes something was saved that was not. The live change (no flag) works everywhere.
  * Values are validated exactly as in the plain form (`effect` 0..6, `speed` 1..255): `RANGE`, nothing applied, nothing saved.
* `status` bits: bit0 `SAVE_PENDING` (a requested save has not reached flash yet), bit1 `SAVE_FAILED` (the latest attempt failed or
  gave up after three tries; the live values are still in force and the previous stored record is intact), bit2 `LOADED` (the values in
  force at boot came from flash), bit3 `IDLE_OFF` (the lights are faded out or fading because the idle time ran out), bit4
  `PERSISTENT` (this build can keep the configuration in flash). `last_result` is the latest save result (0 = written and read back,
  1 = identical to what the flash already held, 3..7 = errors: arg, read, erase, program, verify). `saves_ok` / `saves_failed` count
  since boot and saturate at 255.

**Per-profile form** (feature bit 11 = `0x0800`, only builds that store the lights per key profile, `LIGHT_PROFILE_NOTES.md`; other builds answer
these two forms `BAD_LENGTH` and leave the bit clear):

* LED_GET with two argument bytes `1, profile` (`len = 4`): the same 12 bytes, about that profile's lights (its own, else the shared ones).
* LED_SET with ten argument bytes (`len = 12`): the nine bytes of the extended form + `profile`. Reply: the 12 bytes about that profile.
* `profile` = 0..`profile_count`-1, or `255` = the profile in force; anything else is `BAD_ARG` (nothing applied). Unknown flag bits are `BAD_ARG` here too.
* For the profile in force the request is exactly the nine-byte LED_SET (applied now; with `SAVE` also stored as that profile's own lights).
  For another profile `SAVE` is required (else `RANGE`); nothing is applied to the lights now, the entry is stored and shows when that profile is selected.
* status bit5 `PROFILE_OWN` = this profile has lights of its own; clear = it follows the shared lights. A profile switch (any way) loads that profile's
  lights; the nine-byte and plain forms always address the profile in force.

Saving (the extended form with `SAVE`): the request only changes RAM and returns at once (`SAVE_PENDING` goes up). The firmware
writes the record later: not before 2-3 s after the LAST request (a slider that sends a SET per step is coalesced into one write
with the final values), only after the keyboard has been idle for a second (no key down, no fault, no report waiting), at most
once per 10 s, and one flash operation per main-loop pass. A host that wants to know it is done polls the extended LED_GET until
`SAVE_PENDING` is 0 and checks `SAVE_FAILED`. The record is read back and checked after writing; a record equal to the stored one
writes nothing.

## 4. Status codes

| code | name | meaning |
|---|---|---|
| 0 | OK | |
| 1 | UNKNOWN_OP | sub-command not defined |
| 2 | BAD_LENGTH | data length is not the one the sub-command needs (or no op/token at all) |
| 3 | BAD_ARG | malformed field: offset field or XOR key not 0, zero/oversized count, page 0 with a snapshot id |
| 4 | RANGE | value out of range: offset, profile index, slot |
| 5 | NEED_CONFIRM | confirmation word missing or wrong |
| 6 | SEQUENCE | SET_PAGE not at the expected offset (payload: expected offset) |
| 7 | STALE | GET_SETTINGS page of a snapshot that no longer exists |
| 8 | TOO_BIG | more than 8192 bytes |
| 9 | NOT_STAGED | nothing, or not everything, staged |
| 10 | BAD_CRC | COMMIT length/CRC differ from the staged bytes |
| 11 | INVALID | the validator rejected the staged settings |
| 12 | UNSUPPORTED | this firmware build does not provide it |
| 13 | BUSY | not possible right now; retry later |
| 14 | FAILED | the backend tried and failed (e.g. flash verify) |

## 5. Typical sequences

**Scene write (version 2):** GET_INFO (check bit8, `scene_max`) -> SCENE_BEGIN(total) -> SCENE_PAGE x k -> SCENE_COMMIT("LITE", total,
crc) -> SCENE_GET and compare.

**Dump:** GET_INFO → GET_SETTINGS(offset 0, want 44, id 0) → pages at offset += n with the returned `snap_id` until
`total_len` bytes → compare CRC-32.

**Load:** GET_INFO (check `proto_version`, `max_settings`, feature bit1) → SET_BEGIN(total) → SET_PAGE × k (52 bytes each,
resend a page whose reply is missing, resume from `next` after SEQUENCE) → GET_INFO (`stage_state` = 2) → COMMIT("SAVE",
total, crc) → read the blob back (dump) and compare.

**Lost reply to COMMIT:** read GET_INFO; if `commits` went up and `stage_state` is 0, the commit happened; verify with a
dump. Do not simply send COMMIT again.

## 6. Safety summary

- The only request that can reach flash is COMMIT with `"SAVE"`, the exact staged length and CRC, and an accepting validator.
- SELECT_PROFILE, RECALIBRATE_REQUEST and SAVE_CALIBRATION_REQUEST each need their own word; the words are not
  interchangeable (tested), and a missing word never reveals or changes anything else.
- Unknown, short, long and out-of-range requests are answered with an error and change nothing.
- Staging is bounded by 8192 bytes and by the declared length; a page can only land at the next expected offset.
- The host tools send only the whitelisted ops, check the final bytes twice, refuse a keyboard whose build string lacks
  `KMFW`, and are a dry run unless started with `--run --confirm <word>`.

## 7. Examples (complete frames: first 8 bytes of header, then the data; the rest of the 64 bytes is zero)

Fake backend: 100-byte settings `blob[j] = 3j + 1`, 4 profiles, active 2, 76 slots with depth `1000 + 7·slot`.

```
GET_INFO, token 0x11
  req  55 c6 00 14 02 00 00 00 | 01 11
  rep  aa c6 00 dd 15 00 00 00 | 01 11 00  01 04 02 0010 2c 34 3f00 00 0000 0000 00000000

GET_SETTINGS page 0 (want 44)
  req  55 c6 00 64 06 00 00 00 | 02 30 00 00 2c 00
  rep  aa c6 00 8f 38 00 00 00 | 02 30 00  01 6400 e36281b8 0000 <44 bytes: 01 04 07 0a ...>
GET_SETTINGS page 1 (offset 44, id 1)
  req  55 c6 00 92 06 00 00 00 | 02 31 2c 00 2c 01
GET_SETTINGS with a snapshot id that does not exist
  req  55 c6 00 98 06 00 00 00 | 02 33 2c 00 2c 05
  rep  aa c6 00 3f 03 00 00 00 | 02 33 07                           (STALE)

SET_BEGIN 10 bytes
  req  55 c6 00 51 04 00 00 00 | 03 40 0a 00
  rep  aa c6 00 60 05 00 00 00 | 03 40 00 0020                      (capacity 8192)
SET_PAGE offset 0, bytes 00..09
  req  55 c6 00 80 0e 00 00 00 | 04 41 00 00 00 01 02 03 04 05 06 07 08 09
  rep  aa c6 00 54 05 00 00 00 | 04 41 00 0a00                      (next = 10)
SET_PAGE at offset 5 although 10 is expected
  req  55 c6 00 b7 06 00 00 00 | 04 43 05 00 aa bb
  rep  aa c6 00 5c 05 00 00 00 | 04 43 06 0a00                      (SEQUENCE, next = 10)
COMMIT ("SAVE", len 10, crc32 456cd746)
  req  55 c6 00 5c 0c 00 00 00 | 05 44 53 41 56 45 0a 00 46 d7 6c 45
  rep  aa c6 00 51 07 00 00 00 | 05 44 00 01000000                  (OK, commits = 1)
COMMIT without the word
  req  55 c6 00 2f 0c 00 00 00 | 05 46 00 00 00 00 0a 00 46 d7 6c 45
  rep  aa c6 00 53 03 00 00 00 | 05 46 05                           (NEED_CONFIRM)

SELECT_PROFILE 3
  req  55 c6 00 97 07 00 00 00 | 06 50 50 52 4f 46 03
  rep  aa c6 00 5d 04 00 00 00 | 06 50 00 03
RECALIBRATE_REQUEST
  req  55 c6 00 82 06 00 00 00 | 07 53 52 43 41 4c
LIVE_DEPTH first 3 count 2
  req  55 c6 00 68 04 00 00 00 | 09 56 03 02
  rep  aa c6 00 75 09 00 00 00 | 09 56 00 03 02 fd03 0404           (1021, 1028)
```

(The CRC bytes in the COMMIT example are the little-endian bytes of `crc32(00 01 02 ... 09)`, i.e. `46 d7 6c 45`.)

```
LED_SET effect 3 (rainbow), brightness 200, speed 90, colour 1 2 3, token 0x7e
  req  55 c6 00 d2 08 00 00 00 | 21 7e 03 c8 5a 01 02 03
  rep  aa c6 00 d3 09 00 00 00 | 21 7e 00 03 c8 5a 01 02 03
```

```
LED_SET (extended) effect 3, brightness 200, speed 90, colour 1 2 3, idle 600 s, SAVE, token 0x7e
  req  55 c6 00 30 0b 00 00 00 | 21 7e 03 c8 5a 01 02 03 58 02 01
  rep  aa c6 00 45 0f 00 00 00 | 21 7e 00 03 c8 5a 01 02 03 58 02 11 00 01 00
LED_GET (extended), token 0x7f
  req  55 c6 00 a3 03 00 00 00 | 20 7f 01
```

```
LED_SET (per profile) effect 3, brightness 200, speed 90, colour 1 2 3, idle 600 s, SAVE, profile 2, token 0x7e
  req  55 c6 00 33 0c 00 00 00 | 21 7e 03 c8 5a 01 02 03 58 02 01 02
```

(The reply shown is that of the test fake: status `0x11` = `SAVE_PENDING` + `PERSISTENT`, `last_result` 0, one save counted.)

(These examples are also checked byte by byte in `tests/test_config.c`, `test_led_ops` and `test_led_ex_ops`.)

## 7a. Version 2 examples

Golden vectors: `firmware/tests/golden/config_vectors.h`, groups 21..29 and the `get_info` reply, produced independently of every
implementation by `firmware/tools/gen_config_vectors_p2.py` (`--check` tells whether the header is current). The fake backend of the
golden run has a 50-byte live scene (byte j = 5j + 2) and the device status
`02 01 00 09 02 01 02 00 34 12 03 00 07 00 00 00` (Caps on, gamepad set to HID but not active, USB fell back, profile 2, layer 1,
stream state 2, 0x1234 frames, 3 lost, 7 LED reports); feature bits 9 and 10 are announced, so `features` = `0x07BF`.

| request (op, token, args) | reply (op, token, status, payload) |
|---|---|
| `0A 70` DEVICE_STATUS | `0A 70 00` + the 16 bytes above |
| `0A 71 00` (an argument) | `0A 71 02` (BAD_LENGTH) |
| `30 72 0000 2C 00` SCENE_GET offset 0, want 44 | `30 72 00 01 3200 <crc32 of the 50 bytes> 0000` + the first 44 bytes |
| `30 73 2C00 2C 01` last page | `30 73 00 01 3200 <crc32> 2C00` + the last 6 bytes |
| `30 74 2C00 2C 09` no such snapshot | `30 74 07` (STALE) |
| `31 75 0A00` SCENE_BEGIN 10 | `31 75 00 0004` (limit 1024) |
| `32 76 0000` + 10 bytes | `32 76 00 0A00` |
| `33 77 "LITE" 0A00 <crc32>` | `33 77 00 01000000` (first scene commit) |
| the same again | `33 78 09` (NOT_STAGED) |
| `33 79 00000000 ...` no word | `33 79 05` (NEED_CONFIRM) |
| `33 7A "SAVE" ...` the settings word | `33 7A 05` |
| `31 7B 0104` SCENE_BEGIN 1025 | `31 7B 08` (TOO_BIG) |

## 8. Open points (for the integration into `m5_main.c`)

- The backend's `commit_settings` is expected to decode the blob with `km_settings_decode`, apply it to the engine/keymap
  and call `km_store_save`; `validate_settings` can be `km_settings_decode` into a scratch struct.
- The settings blob is the `km_settings` format. Since the layers update (task L4) it is **version 2, 7944 bytes**
  (version 1, 5488 bytes, is still accepted by `km_settings_decode` and migrated); that is why `max_settings` is now 8192 and
  `KM_CONFIG_MAX_SETTINGS` is 8192 (`m5_main.c` asserts the blob fits). The protocol itself did not change: the blob is opaque to it.
- `commit_settings`/`select_profile` should answer `BUSY` while any key is down (rule 2A.4 of `M5_PLAN.md`: flash only
  while no key is pressed and the USB endpoint is idle); the host retries.
- The command must be registered with `km_vendor_set_config()`; until then 0xC6 is answered `"unknw"`.
