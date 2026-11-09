#!/usr/bin/env python3
"""magger60_config.py - read and write the settings of OUR custom Magger60 firmware over its configuration protocol
(command 0xC6, CONFIG_PROTOCOL.md): show the protocol info, dump the settings blob to a file, load a blob into the
keyboard's RAM staging buffer, commit it (the one step that writes flash), switch the active profile, read live depths.

It only talks to firmware built from this repository (the cmd 3 build string must carry the KMFW label); the stock
firmware is never sent a 0xC6 packet. Everything the tool can send is checked twice (guard_outgoing, called by the real
transport and by the simulator) and only these requests exist: the cmd 3 version query, GET_INFO, GET_SETTINGS,
LIVE_DEPTH, DEVICE_STATUS and SCENE_GET (all read-only), SET_BEGIN / SET_PAGE and SCENE_BEGIN / SCENE_PAGE (RAM staging only),
COMMIT and SCENE_COMMIT (flash) and SELECT_PROFILE (persistent).
Recalibration and calibration saving are deliberately not in this tool.

Safe by default: without --run the tool talks to a SIMULATED keyboard and prints what it would send. With --run it talks
to the real keyboard: info, dump and depth need only --run; load, commit and select-profile also need --confirm <word>
(STAGE for load, SAVE for commit, PROF for select-profile).

    python tools/magger60_config.py info                           # dry run: simulated keyboard, shows the packets
    python tools/magger60_config.py --self-test                    # no hardware: protocol, safety and simulator checks
    python tools/magger60_config.py info --run                     # real keyboard: protocol info
    python tools/magger60_config.py dump settings.bin --run        # save the live settings blob to a file
    python tools/magger60_config.py load settings.bin --run --confirm STAGE      # stage in RAM only (no flash)
    python tools/magger60_config.py commit settings.bin --run --confirm SAVE     # stage, COMMIT, read back and compare
    python tools/magger60_config.py select-profile 2 --run --confirm PROF
    python tools/magger60_config.py depth --run                    # the 76 live depths (0.01 mm)
    python tools/magger60_config.py status --run                   # protocol 2: host lock lights, gamepad mode, USB fall-back, ...
    python tools/magger60_config.py scene-dump scene.bin --run     # protocol 2: save the live light scene (KMLS blob)
    python tools/magger60_config.py scene-commit scene.bin --run --confirm LITE   # stage + COMMIT the scene (writes its flash sector)

The settings are an opaque byte blob here (its format belongs to the firmware's settings module); the tool moves bytes,
checks their CRC-32 and never interprets them.
"""
import argparse
import importlib.util
import re
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent


def _load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / (name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


bk = _load("magger60_backup")
info = _load("magger60_info")

CMD_CONFIG = 0xC6
FIRMWARE_LABEL = "KMFW"
PROTO_VERSION = 2
SUPPORTED_PROTO = (1, 2)          # version 2 only ADDS (DEVICE_STATUS, SCENE_*, feature bits 7..10); every version-1 op is unchanged
MAX_SETTINGS = 8192
MAX_SCENE = 1024
DEVICE_STATUS_BYTES = 16
GET_PAGE = 44
SET_PAGE = 52
MAX_DEPTHS = 25
MAX_SLOTS = 128
VECTORS = REPO / "firmware/tests/golden/config_vectors.h"

OP_GET_INFO, OP_GET_SETTINGS, OP_SET_BEGIN, OP_SET_PAGE, OP_COMMIT = 1, 2, 3, 4, 5
OP_SELECT_PROFILE, OP_RECALIBRATE, OP_SAVE_CAL, OP_LIVE_DEPTH = 6, 7, 8, 9
OP_DEVICE_STATUS = 0x0A
OP_SCENE_GET, OP_SCENE_BEGIN, OP_SCENE_PAGE, OP_SCENE_COMMIT = 0x30, 0x31, 0x32, 0x33
# The only ops this tool may send. 7 and 8 (calibration) and the LED ops (magger60_led.py) are NOT in it.
ALLOWED_OPS = {OP_GET_INFO, OP_GET_SETTINGS, OP_SET_BEGIN, OP_SET_PAGE, OP_COMMIT, OP_SELECT_PROFILE, OP_LIVE_DEPTH,
               OP_DEVICE_STATUS, OP_SCENE_GET, OP_SCENE_BEGIN, OP_SCENE_PAGE, OP_SCENE_COMMIT}
# Ops that need a confirmation word inside the request, and the word.
WORDS = {OP_COMMIT: b"SAVE", OP_SELECT_PROFILE: b"PROF", OP_SCENE_COMMIT: b"LITE"}
# Exact request data length (op + token + args) per op; SET_PAGE and SCENE_PAGE are 4 + 1..52.
ARG_LEN = {OP_GET_INFO: 0, OP_GET_SETTINGS: 4, OP_SET_BEGIN: 2, OP_COMMIT: 10, OP_SELECT_PROFILE: 5, OP_LIVE_DEPTH: 2,
           OP_DEVICE_STATUS: 0, OP_SCENE_GET: 4, OP_SCENE_BEGIN: 2, OP_SCENE_COMMIT: 10}
# CLI confirmation words (what the user types after --confirm) for the commands that change state.
CLI_CONFIRM = {"load": "STAGE", "commit": "SAVE", "select-profile": "PROF", "scene-load": "STAGE", "scene-commit": "LITE"}

STATUS = {0: "OK", 1: "UNKNOWN_OP", 2: "BAD_LENGTH", 3: "BAD_ARG", 4: "RANGE", 5: "NEED_CONFIRM", 6: "SEQUENCE",
          7: "STALE", 8: "TOO_BIG", 9: "NOT_STAGED", 10: "BAD_CRC", 11: "INVALID", 12: "UNSUPPORTED", 13: "BUSY",
          14: "FAILED"}
ST_OK, ST_SEQUENCE, ST_STALE, ST_BUSY = 0, 6, 7, 13


class ConfigError(Exception):
    """The keyboard did not answer as the protocol says, answered with an error, or is not our firmware."""

    def __init__(self, message, status=None):
        super().__init__(message)
        self.status = status


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def is_kmfw_build(build: str) -> bool:
    """True for a cmd 3 build string of our firmware: [optional "Mon dd yyyy,hh:mm:ss" stamp] + "KMFW...".
    Other firmware (the stock one) does not carry the KMFW label."""
    m = re.match(r"^[A-Z][a-z]{2} [ 0-9][0-9] [0-9]{4},[0-9]{2}:[0-9]{2}:[0-9]{2}", build)
    return (build[m.end():] if m else build).startswith(FIRMWARE_LABEL)


# --------------------------------------------------------------------------- packets

def build_request(op: int, token: int, args: bytes = b"") -> bytes:
    """A 0xC6 request: 'U', cmd 0xC6, no XOR, offset 0, data = op, token, args. Refused (SafetyError) for anything
    outside the whitelist or with the wrong shape."""
    if op not in ALLOWED_OPS:
        raise bk.SafetyError("config op 0x%02X is not allowed in this tool" % op)
    if not 0 <= token <= 0xFF:
        raise bk.SafetyError("token out of range")
    data = bytes([op, token]) + bytes(args)
    p = bytearray(bk.REPORT_LEN)
    p[0] = bk.HDR_NORMAL
    p[1] = CMD_CONFIG
    if len(data) > bk.MAX_CHUNK:
        raise bk.SafetyError("request data above %d bytes" % bk.MAX_CHUNK)
    p[3] = bk.checksum(len(data), 0, data)
    p[4] = len(data)
    p[8:8 + len(data)] = data
    return guard_config_request(bytes(p))


def guard_config_request(packet: bytes) -> bytes:
    """Independent check of the exact bytes about to be sent, written from the spec rather than from build_request."""
    if len(packet) != bk.REPORT_LEN:
        raise bk.SafetyError("packet is not 64 bytes")
    if packet[0] != bk.HDR_NORMAL:
        raise bk.SafetyError("only 'U' packets may be sent (raw '_' packets can reboot the keyboard)")
    if packet[1] != CMD_CONFIG:
        raise bk.SafetyError("not the config command")
    if packet[2] != 0:
        raise bk.SafetyError("XOR-masked packets are not allowed")
    if packet[5] or packet[6] or packet[7]:
        raise bk.SafetyError("the offset field must be 0")
    n = packet[4]
    if n < 2 or n > bk.MAX_CHUNK:
        raise bk.SafetyError("bad data length %d" % n)
    if any(packet[8 + n:]):
        raise bk.SafetyError("bytes after the data must be zero")
    if packet[3] != bk.checksum(n, 0, packet[8:8 + n]):
        raise bk.SafetyError("checksum does not match the data")
    op = packet[8]
    if op not in ALLOWED_OPS:
        raise bk.SafetyError("config op 0x%02X is not allowed in this tool" % op)
    args = packet[10:8 + n]
    if op in (OP_SET_PAGE, OP_SCENE_PAGE):
        if not 3 <= len(args) <= 2 + SET_PAGE:
            raise bk.SafetyError("SET_PAGE carries 1..%d data bytes" % SET_PAGE)
    elif len(args) != ARG_LEN[op]:
        raise bk.SafetyError("op 0x%02X needs %d argument bytes, not %d" % (op, ARG_LEN[op], len(args)))
    if op in WORDS and bytes(args[:4]) != WORDS[op]:
        raise bk.SafetyError("op 0x%02X must carry the confirmation word %r" % (op, WORDS[op].decode()))
    if op in (OP_GET_SETTINGS, OP_SCENE_GET):
        off, want, sid = args[0] | (args[1] << 8), args[2], args[3]
        if not 1 <= want <= GET_PAGE or (off == 0 and sid != 0):
            raise bk.SafetyError("bad GET_SETTINGS arguments")
    if op in (OP_SET_BEGIN, OP_SCENE_BEGIN):
        total = args[0] | (args[1] << 8)
        if not 1 <= total <= (MAX_SETTINGS if op == OP_SET_BEGIN else MAX_SCENE):
            raise bk.SafetyError("SET_BEGIN length out of range")
    if op == OP_SELECT_PROFILE and args[4] > 15:
        raise bk.SafetyError("profile index out of range")
    if op == OP_LIVE_DEPTH and not (1 <= args[1] <= MAX_DEPTHS and args[0] + args[1] <= MAX_SLOTS):
        raise bk.SafetyError("bad LIVE_DEPTH arguments")
    return packet


def guard_outgoing(packet: bytes) -> bytes:
    """The only packets this tool may send: the cmd 3 version query and the whitelisted config requests."""
    if len(packet) != bk.REPORT_LEN:
        raise bk.SafetyError("packet is not 64 bytes")
    if packet[1] == bk.CMD_GET_INFO:
        return bk.guard_outgoing(packet)
    if packet[1] == CMD_CONFIG:
        return guard_config_request(packet)
    raise bk.SafetyError("command 0x%02X is not allowed in the config tool" % packet[1])


class Reply:
    def __init__(self, op, token, status, payload):
        self.op, self.token, self.status, self.payload = op, token, status, payload

    @property
    def name(self):
        return STATUS.get(self.status, "status %d" % self.status)


def decode_reply(resp: bytes) -> Reply:
    if len(resp) != bk.REPORT_LEN:
        raise ConfigError("reply is not 64 bytes")
    if resp[0] == 0xAB:
        raise ConfigError("the keyboard reports a checksum error (0xAB)")
    if resp[0] != 0xAA or resp[1] != CMD_CONFIG:
        raise ConfigError("unexpected reply: %s" % resp[:16].hex())
    n = resp[4]
    if n < 3 or n > bk.MAX_CHUNK:
        raise ConfigError("bad reply length %d (is this really our firmware?)" % n)
    data = resp[8:8 + n]
    if resp[2] or resp[5] or resp[6] or resp[7]:
        raise ConfigError("reply header fields must be zero")
    if bk.checksum(n, 0, data) != resp[3]:
        raise ConfigError("reply checksum does not match: %s" % resp[:16].hex())
    if any(resp[8 + n:]):
        raise ConfigError("reply has data beyond its length")
    return Reply(data[0], data[1], data[2], bytes(data[3:]))


def le16(v):
    return bytes([v & 0xFF, (v >> 8) & 0xFF])


def le32(v):
    return le16(v & 0xFFFF) + le16(v >> 16)


def rd16(b, i=0):
    return b[i] | (b[i + 1] << 8)


def rd32(b, i=0):
    return rd16(b, i) | (rd16(b, i + 2) << 16)


# --------------------------------------------------------------------------- transports

class ConfigTransport(bk.HidapiTransport):
    """The backup tool's HID transport with this tool's own allow-list. Stale replies (an older request's answer that is
    still queued) are discarded: the reply must carry the command, and for 0xC6 the op and token, of the request."""

    def exchange(self, packet: bytes) -> bytes:
        packet = guard_outgoing(packet)
        self._dev.write(b"\x00" + packet)
        for _ in range(8):
            data = self._dev.read(bk.REPORT_LEN, self._timeout)
            if not data or len(data) < bk.REPORT_LEN:
                raise bk.BackupError("no (or short) response from the keyboard")
            resp = bytes(data[:bk.REPORT_LEN])
            if resp[1] != packet[1]:
                continue
            if packet[1] == CMD_CONFIG and resp[0] == 0xAA and (resp[8] != packet[8] or resp[9] != packet[9]):
                continue
            return resp
        raise bk.BackupError("only stale replies came back")


# --------------------------------------------------------------------------- client

class Info:
    def __init__(self, payload):
        if len(payload) < 18:
            raise ConfigError("GET_INFO reply too short (%d bytes)" % len(payload))
        p = payload
        self.proto_version, self.profile_count, self.active_profile = p[0], p[1], p[2]
        self.max_settings, self.get_page, self.set_page = rd16(p, 3), p[5], p[6]
        self.features = rd16(p, 7)
        self.stage_state, self.stage_total, self.stage_next = p[9], rd16(p, 10), rd16(p, 12)
        self.commits = rd32(p, 14)
        self.scene_max = rd16(p, 18) if len(p) >= 20 else 0       # protocol 2

    FEATURES = ["read settings", "write settings", "select profile", "recalibrate", "save calibration", "live depth", "lights",
                "device status", "light scene", "stream", "host lock lights"]

    def has(self, name):
        return bool(self.features >> self.FEATURES.index(name) & 1)

    def feature_names(self):
        return [n for i, n in enumerate(self.FEATURES) if self.features >> i & 1]

    def __str__(self):
        return ("protocol %d, %d profiles (active %s), settings up to %d bytes, pages %d read / %d write\n"
                "features: %s\nstaging: %s (%d of %d bytes), commits since boot: %d" % (
                    self.proto_version, self.profile_count, "?" if self.active_profile == 0xFF else self.active_profile,
                    self.max_settings, self.get_page, self.set_page, ", ".join(self.feature_names()) or "none",
                    ["empty", "receiving", "complete"][self.stage_state] if self.stage_state < 3 else "?",
                    self.stage_next, self.stage_total, self.commits)
                + ("\nlight scene up to %d bytes" % self.scene_max if self.scene_max else ""))


class ConfigClient:
    def __init__(self, transport: bk.Transport, retries: int = 4, log=None):
        self.t = transport
        self.retries = retries
        self.verified = False
        self.build = None
        self.token = 0
        self.info = None
        self.log = log or (lambda s: None)

    def verify(self) -> str:
        """Ask cmd 3 and demand our firmware, then GET_INFO and demand protocol version 1 or 2. Until this passes no other
        0xC6 request is built."""
        d = info.decode(self.t.exchange(bk.build_request(bk.CMD_GET_INFO, 0, 0)))
        self.build = d["build"]
        if not is_kmfw_build(self.build):
            raise ConfigError("this keyboard runs '%s', not our firmware (build string must carry %s); "
                              "no config command was sent" % (self.build, FIRMWARE_LABEL))
        self.verified = True
        self.info = self.get_info()
        if self.info.proto_version not in SUPPORTED_PROTO:
            self.verified = False
            raise ConfigError("protocol version %d is not supported by this tool (it speaks %s)" % (
                self.info.proto_version, " and ".join(str(v) for v in SUPPORTED_PROTO)))
        return self.build

    def call(self, op: int, args: bytes = b"", ok=(ST_OK,)) -> Reply:
        """Send one request and return its reply. A missing reply (timeout) is retried with the SAME request: every op
        is safe to repeat (a repeated SET_PAGE is acknowledged without effect, a repeated COMMIT is refused as NOT_STAGED
        - COMMIT handles that itself). A reply that is not for this request (op/token) is ignored."""
        if not self.verified and op != OP_GET_INFO:
            raise ConfigError("firmware not verified yet; call verify() first")
        self.token = (self.token + 1) & 0xFF
        packet = build_request(op, self.token, args)
        last = None
        for attempt in range(self.retries):
            try:
                rep = decode_reply(self.t.exchange(packet))
            except bk.BackupError as exc:
                last = exc
                self.log("no reply (%s); attempt %d" % (exc, attempt + 1))
                continue
            if rep.op != op or rep.token != self.token:
                self.log("stale reply ignored")
                continue
            if rep.status not in ok:
                raise ConfigError("op 0x%02X refused: %s" % (op, rep.name), rep.status) from None
            return rep
        raise ConfigError("no answer to op 0x%02X after %d attempts: %s" % (op, self.retries, last))

    def get_info(self) -> Info:
        return Info(self.call(OP_GET_INFO).payload)

    def dump(self, tries: int = 4, op: int = OP_GET_SETTINGS, limit: int = MAX_SETTINGS) -> bytes:
        """Read the live settings blob; every page must come from the same snapshot and the CRC must match."""
        for _ in range(tries):
            blob, sid, total, crc, stale = bytearray(), 0, None, None, False
            while total is None or len(blob) < total:
                rep = self.call(op, le16(len(blob)) + bytes([GET_PAGE, sid]), ok=(ST_OK, ST_STALE))
                if rep.status == ST_STALE:
                    stale = True
                    break
                p = rep.payload
                if len(p) < 9:
                    raise ConfigError("GET_SETTINGS reply too short")
                psid, ptotal, pcrc, poff = p[0], rd16(p, 1), rd32(p, 3), rd16(p, 7)
                data = p[9:]
                if total is None:
                    sid, total, crc = psid, ptotal, pcrc
                    if not 1 <= total <= limit or sid == 0:
                        raise ConfigError("nonsense GET_SETTINGS header (total %d, id %d)" % (total, sid))
                if (psid, ptotal, pcrc) != (sid, total, crc) or poff != len(blob):
                    stale = True
                    break
                if not data or len(blob) + len(data) > total:
                    raise ConfigError("GET_SETTINGS page has %d bytes at offset %d (total %d)" % (len(data), poff, total))
                blob += data
            if stale:
                continue
            if crc32(bytes(blob)) != crc:
                raise ConfigError("the settings blob read back does not match its CRC-32")
            return bytes(blob)
        raise ConfigError("could not read a consistent settings snapshot in %d tries" % tries)

    def stage(self, blob: bytes, begin: int = OP_SET_BEGIN, page: int = OP_SET_PAGE, limit: int = None) -> None:
        """SET_BEGIN + SET_PAGE...: RAM staging only. A lost reply resends the page; SEQUENCE resumes at the expected offset."""
        limit = self.info.max_settings if limit is None else limit
        if not 1 <= len(blob) <= limit:
            raise ConfigError("the blob must be 1..%d bytes, not %d" % (limit, len(blob)))
        self.call(begin, le16(len(blob)))
        pos, errors = 0, 0
        while pos < len(blob):
            chunk = blob[pos:pos + min(SET_PAGE, self.info.set_page)]
            rep = self.call(page, le16(pos) + chunk, ok=(ST_OK, ST_SEQUENCE))
            nxt = rd16(rep.payload, 0) if len(rep.payload) >= 2 else None
            if rep.status == ST_OK:
                if nxt != pos + len(chunk):
                    raise ConfigError("the keyboard acknowledged the page but expects offset %s, not %d" % (nxt, pos + len(chunk)))
                pos = nxt
            else:
                errors += 1
                if errors > 20 or nxt is None or nxt > len(blob):
                    raise ConfigError("staging keeps going out of sequence")
                self.log("sequence error: resuming at offset %d" % nxt)
                pos = nxt
        if begin == OP_SET_BEGIN:
            inf = self.get_info()
            if inf.stage_state != 2 or inf.stage_total != len(blob) or inf.stage_next != len(blob):
                raise ConfigError("staging is not complete after the last page")

    def commit(self, blob: bytes) -> int:
        """COMMIT what is staged (the caller has just staged `blob`). Returns the keyboard's commit counter. A lost reply
        is resolved by reading GET_INFO: if the counter went up and nothing is staged, the commit happened."""
        before = self.get_info().commits
        args = b"SAVE" + le16(len(blob)) + le32(crc32(blob))
        self.token = (self.token + 1) & 0xFF
        packet = build_request(OP_COMMIT, self.token, args)
        for attempt in range(self.retries):
            try:
                rep = decode_reply(self.t.exchange(packet))
            except bk.BackupError:
                inf = self.get_info()
                if inf.commits == before + 1 and inf.stage_state == 0:
                    return inf.commits
                if inf.commits != before:
                    raise ConfigError("commit counter moved by %d, expected 0 or 1" % (inf.commits - before))
                continue                      # nothing happened: the request was lost, send it again
            if rep.op != OP_COMMIT or rep.token != packet[9]:
                continue
            if rep.status != ST_OK:
                raise ConfigError("COMMIT refused: %s" % rep.name, rep.status)
            return rd32(rep.payload, 0)
        raise ConfigError("no answer to COMMIT after %d attempts" % self.retries)

    # ---- protocol 2 ----
    def need_proto2(self, feature):
        if self.info.proto_version < 2 or not self.info.has(feature):
            raise ConfigError("this firmware does not provide '%s' (protocol %d, features: %s)" % (
                feature, self.info.proto_version, ", ".join(self.info.feature_names()) or "none"))

    def status(self) -> dict:
        """DEVICE_STATUS (read-only): the 16 bytes decoded."""
        self.need_proto2("device status")
        p = self.call(OP_DEVICE_STATUS).payload
        if len(p) != DEVICE_STATUS_BYTES:
            raise ConfigError("DEVICE_STATUS reply has %d bytes, not %d" % (len(p), DEVICE_STATUS_BYTES))
        return {"host_leds": p[0], "num": bool(p[0] & 1), "caps": bool(p[0] & 2), "scroll": bool(p[0] & 4),
                "pad_mode_setting": p[1], "pad_mode_active": p[2],
                "usb_fell_back": bool(p[3] & 1), "stream_active": bool(p[3] & 2), "anim_playing": bool(p[3] & 4),
                "host_leds_known": bool(p[3] & 8), "profile": p[4], "layer": p[5], "stream_state": p[6],
                "stream_frames": rd16(p, 8), "stream_lost": rd16(p, 10), "led_reports": rd16(p, 12)}

    def scene_dump(self) -> bytes:
        self.need_proto2("light scene")
        return self.dump(op=OP_SCENE_GET, limit=MAX_SCENE)

    def scene_stage(self, blob: bytes) -> None:
        self.need_proto2("light scene")
        self.stage(blob, begin=OP_SCENE_BEGIN, page=OP_SCENE_PAGE, limit=min(MAX_SCENE, self.info.scene_max or MAX_SCENE))

    def scene_commit(self, blob: bytes) -> int:
        """SCENE_COMMIT what is staged. A lost reply is NOT retried blindly: SCENE_COMMIT is refused as NOT_STAGED the second time,
        so a lost reply is resolved by reading the scene back and comparing it with `blob`."""
        self.need_proto2("light scene")
        args = b"LITE" + le16(len(blob)) + le32(crc32(blob))
        self.token = (self.token + 1) & 0xFF
        packet = build_request(OP_SCENE_COMMIT, self.token, args)
        for _ in range(self.retries):
            try:
                rep = decode_reply(self.t.exchange(packet))
            except bk.BackupError:
                if self.scene_dump() == blob:
                    return -1                  # it happened (the counter is unknown)
                continue
            if rep.op != OP_SCENE_COMMIT or rep.token != packet[9]:
                continue
            if rep.status != ST_OK:
                raise ConfigError("SCENE_COMMIT refused: %s" % rep.name, rep.status)
            return rd32(rep.payload, 0)
        raise ConfigError("no answer to SCENE_COMMIT after %d attempts" % self.retries)

    def select_profile(self, index: int) -> int:
        if not 0 <= index < self.info.profile_count:
            raise ConfigError("profile %d: the keyboard has %d profiles (0..%d)" % (index, self.info.profile_count, self.info.profile_count - 1))
        rep = self.call(OP_SELECT_PROFILE, b"PROF" + bytes([index]))
        return rep.payload[0]

    def live_depth(self, count: int = 76) -> list:
        out = []
        while len(out) < count:
            n = min(MAX_DEPTHS, count - len(out))
            p = self.call(OP_LIVE_DEPTH, bytes([len(out), n])).payload
            if len(p) != 2 + 2 * n or p[0] != len(out) or p[1] != n:
                raise ConfigError("bad LIVE_DEPTH reply")
            out += [rd16(p, 2 + 2 * i) for i in range(n)]
        return out


# --------------------------------------------------------------------------- simulated keyboard

class SimulatedConfigKeyboard(bk.Transport):
    """A keyboard that follows CONFIG_PROTOCOL.md: an independent Python model of the C code in km_config.c. stock=True
    behaves like the stock firmware (cmd 3 with the stock build string, everything else 'unknw')."""

    STOCK_BUILD = "Jan  1 2026,00:00:00STOCK-SIM"
    OUR_BUILD = "Jan  1 2026,00:00:00KMFW-SIM"

    def __init__(self, stock=False, blob=None, profiles=4, active=2, slots=76, on_exchange=None, proto=2, scene=None, devstatus=None):
        self.stock = stock
        self.build = self.STOCK_BUILD if stock else self.OUR_BUILD
        self.blob = bytes(blob) if blob is not None else bytes((j * 3 + 1) & 0xFF for j in range(100))
        self.profiles, self.active, self.slots = profiles, active, slots
        self.on_exchange = on_exchange       # f(sim, n_exchanges, phase) with phase "before"/"after" the request is processed
        self.seen = []                        # (cmd, op) of every packet that arrived
        self.flash_writes = 0                 # number of commits that reached the (simulated) flash
        self.commits = 0
        self.state, self.total, self.next, self.stage = 0, 0, 0, bytearray()
        self.last = None
        self.snap, self.snap_id, self.snap_valid = b"", 0, False
        self.recal = self.savecal = 0
        # protocol 2: the light scene (own staging, own snapshot) and a device status
        self.proto = proto
        self.scene = bytes(scene) if scene is not None else bytes((j * 5 + 2) & 0xFF for j in range(50))
        self.scene_commits = self.scene_flash_writes = 0
        self.sc_state, self.sc_total, self.sc_next, self.sc_stage, self.sc_last = 0, 0, 0, bytearray(), None
        self.sc_snap, self.sc_snap_id, self.sc_snap_valid = b"", 0, False
        self.devstatus = bytes(devstatus) if devstatus is not None else bytes([2, 1, 0, 0x09, 2, 1, 2, 0, 0x34, 0x12, 3, 0, 7, 0, 0, 0])

    @staticmethod
    def _frame(cmd, data, header=0xAA):
        r = bytearray(bk.REPORT_LEN)
        r[0], r[1], r[3], r[4] = header, cmd, bk.checksum(len(data), 0, data), len(data)
        r[8:8 + len(data)] = data
        return bytes(r)

    def depth(self, slot):
        return 1000 + 7 * slot

    def exchange(self, packet: bytes) -> bytes:
        return self.process(guard_outgoing(packet))

    def process(self, packet: bytes) -> bytes:
        """The keyboard's side, without the host's allow-list (the golden vectors also contain requests the tool would
        never send: the keyboard has to answer those correctly too)."""
        self.seen.append((packet[1], packet[8] if packet[1] == CMD_CONFIG else None))
        n = len(self.seen)
        if self.on_exchange:
            self.on_exchange(self, n, "before")
        if packet[1] == bk.CMD_GET_INFO:
            out = self._frame(3, bytes([0x04, 0x01]) + self.build.encode("ascii"))
        elif self.stock:
            out = self._frame(packet[1], b"unknw")
        else:
            out = self._config(packet)
        if self.on_exchange:
            self.on_exchange(self, n, "after")
        return out

    def _config(self, packet):
        ln = packet[4]
        d = packet[8:8 + ln]
        if ln < 2:
            return self._frame(CMD_CONFIG, bytes([d[0] if ln else 0, 0, 2]))
        op, token = d[0], d[1]
        a = d[2:]
        pl = b""

        def done(status, payload=b""):
            return self._frame(CMD_CONFIG, bytes([op, token, status]) + payload)

        if op == OP_GET_INFO:
            if len(a) != 0:
                return done(2)
            feat = 0x3F if self.proto < 2 else 0x3F | 0x80 | 0x100 | 0x200 | 0x400
            return done(0, bytes([self.proto, self.profiles, self.active]) + le16(MAX_SETTINGS) + bytes([GET_PAGE, SET_PAGE]) + le16(feat)
                        + bytes([self.state]) + le16(self.total) + le16(self.next) + le32(self.commits)
                        + (le16(MAX_SCENE) if self.proto >= 2 else b""))
        if op == OP_GET_SETTINGS:
            if len(a) != 4:
                return done(2)
            off, want, sid = rd16(a), a[2], a[3]
            if want == 0 or want > GET_PAGE or (off == 0 and sid != 0):
                return done(3)
            if off == 0:
                self.snap, self.snap_valid = self.blob, True
                self.snap_id = self.snap_id % 255 + 1
            else:
                if not self.snap_valid or sid != self.snap_id:
                    return done(7)
                if off > len(self.snap):
                    return done(4)
            data = self.snap[off:off + want]
            return done(0, bytes([self.snap_id]) + le16(len(self.snap)) + le32(crc32(self.snap)) + le16(off) + data)
        if op == OP_SET_BEGIN:
            if len(a) != 2:
                return done(2)
            total = rd16(a)
            if total == 0:
                return done(3)
            if total > MAX_SETTINGS:
                return done(8)
            self.state, self.total, self.next, self.stage, self.last = 1, total, 0, bytearray(MAX_SETTINGS), None
            return done(0, le16(MAX_SETTINGS))
        if op == OP_SET_PAGE:
            if len(a) < 3:
                return done(2)
            off, data = rd16(a), bytes(a[2:])
            if self.state == 0:
                return done(9)
            if self.last == (off, data):
                return done(0, le16(self.next))
            if off != self.next:
                return done(6, le16(self.next))
            if off + len(data) > self.total:
                return done(4, le16(self.next))
            self.stage[off:off + len(data)] = data
            self.last = (off, data)
            self.next = off + len(data)
            if self.next == self.total:
                self.state = 2
            return done(0, le16(self.next))
        if op == OP_COMMIT:
            if len(a) != 10:
                return done(2)
            if bytes(a[:4]) != b"SAVE":
                return done(5)
            if self.state != 2 or self.next != self.total:
                return done(9)
            if rd16(a, 4) != self.total or rd32(a, 6) != crc32(bytes(self.stage[:self.total])):
                return done(10)
            self.blob = bytes(self.stage[:self.total])
            self.flash_writes += 1
            self.commits += 1
            self.state, self.total, self.next, self.last, self.snap_valid = 0, 0, 0, None, False
            return done(0, le32(self.commits))
        if op == OP_SELECT_PROFILE:
            if len(a) != 5:
                return done(2)
            if bytes(a[:4]) != b"PROF":
                return done(5)
            if a[4] >= self.profiles:
                return done(4)
            self.active = a[4]
            self.snap_valid = False
            return done(0, bytes([self.active]))
        if op in (OP_RECALIBRATE, OP_SAVE_CAL):
            word = b"RCAL" if op == OP_RECALIBRATE else b"SCAL"
            if len(a) != 4:
                return done(2)
            if bytes(a) != word:
                return done(5)
            if op == OP_RECALIBRATE:
                self.recal += 1
            else:
                self.savecal += 1
            return done(0)
        if op == OP_LIVE_DEPTH:
            if len(a) != 2:
                return done(2)
            first, count = a[0], a[1]
            if count == 0 or count > MAX_DEPTHS:
                return done(3)
            if first + count > MAX_SLOTS or first + count > self.slots:
                return done(4)
            return done(0, bytes([first, count]) + b"".join(le16(self.depth(first + i)) for i in range(count)))
        if self.proto >= 2:
            r = self._config2(op, a, done)
            if r is not None:
                return r
        return done(1)

    def _config2(self, op, a, done):
        """The protocol-2 ops: DEVICE_STATUS and the SCENE_* family (an independent model of km_config.c)."""
        if op == OP_DEVICE_STATUS:
            return done(2) if len(a) else done(0, self.devstatus)
        if op == OP_SCENE_GET:
            if len(a) != 4:
                return done(2)
            off, want, sid = rd16(a), a[2], a[3]
            if want == 0 or want > GET_PAGE or (off == 0 and sid != 0):
                return done(3)
            if off == 0:
                self.sc_snap, self.sc_snap_valid = self.scene, True
                self.sc_snap_id = self.sc_snap_id % 255 + 1
            else:
                if not self.sc_snap_valid or sid != self.sc_snap_id:
                    return done(7)
                if off > len(self.sc_snap):
                    return done(4)
            return done(0, bytes([self.sc_snap_id]) + le16(len(self.sc_snap)) + le32(crc32(self.sc_snap)) + le16(off)
                        + self.sc_snap[off:off + want])
        if op == OP_SCENE_BEGIN:
            if len(a) != 2:
                return done(2)
            total = rd16(a)
            if total == 0:
                return done(3)
            if total > MAX_SCENE:
                return done(8)
            self.sc_state, self.sc_total, self.sc_next, self.sc_stage, self.sc_last = 1, total, 0, bytearray(MAX_SCENE), None
            return done(0, le16(MAX_SCENE))
        if op == OP_SCENE_PAGE:
            if len(a) < 3:
                return done(2)
            off, data = rd16(a), bytes(a[2:])
            if self.sc_state == 0:
                return done(9)
            if self.sc_last == (off, data):
                return done(0, le16(self.sc_next))
            if off != self.sc_next:
                return done(6, le16(self.sc_next))
            if off + len(data) > self.sc_total:
                return done(4, le16(self.sc_next))
            self.sc_stage[off:off + len(data)] = data
            self.sc_last = (off, data)
            self.sc_next = off + len(data)
            if self.sc_next == self.sc_total:
                self.sc_state = 2
            return done(0, le16(self.sc_next))
        if op == OP_SCENE_COMMIT:
            if len(a) != 10:
                return done(2)
            if bytes(a[:4]) != b"LITE":
                return done(5)
            if self.sc_state != 2 or self.sc_next != self.sc_total:
                return done(9)
            if rd16(a, 4) != self.sc_total or rd32(a, 6) != crc32(bytes(self.sc_stage[:self.sc_total])):
                return done(10)
            self.scene = bytes(self.sc_stage[:self.sc_total])
            self.scene_flash_writes += 1
            self.scene_commits += 1
            self.sc_state, self.sc_total, self.sc_next, self.sc_last, self.sc_snap_valid = 0, 0, 0, None, False
            return done(0, le32(self.scene_commits))
        return None


# --------------------------------------------------------------------------- self-test

def parse_vectors():
    text = VECTORS.read_text(encoding="utf-8")
    out = []
    for m in re.finditer(r'\{ "([^"]+)", (\d+), "([0-9a-f]{128})", "([0-9a-f]{128})" \}', text):
        out.append((m.group(1), int(m.group(2)), bytes.fromhex(m.group(3)), bytes.fromhex(m.group(4))))
    return out


def self_test() -> int:
    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)

    def refuses(fn, what):
        try:
            fn()
        except (bk.SafetyError, ConfigError):
            return
        fails.append("not refused: " + what)

    vectors = parse_vectors()
    check(len(vectors) >= 20, "golden vectors found (%d)" % len(vectors))

    # 1. golden packets: the simulator (an independent model) reproduces every reply; build_request reproduces every request
    for i, (name, group, req, rep) in enumerate(vectors):
        sim = SimulatedConfigKeyboard()
        for pname, pg, preq, _ in vectors[:i]:
            if pg == group:
                sim.process(preq)
        got = sim.process(req)
        check(got == rep, "golden reply: " + name)
        n = req[4]
        op, token, args = req[8], req[9], req[10:8 + n]
        if n >= 2:
            try:
                check(build_request(op, token, args) == req, "build_request reproduces: " + name)
            except bk.SafetyError:
                pass                               # vectors with ops/shapes the tool must not send (e.g. recalibrate)
        else:
            refuses(lambda: guard_config_request(req), "request without op/token: " + name)
    check(crc32(b"123456789") == 0xCBF43926, "crc32 check value")

    # 2. the allow-list and the double check
    for op in (0, 7, 8, 0x0B, 0x20, 0x21, 0x34, 0x80, 0xFF):
        refuses(lambda op=op: build_request(op, 1, b"RCAL"), "op 0x%02X" % op)
    good = build_request(OP_COMMIT, 5, b"SAVE" + le16(10) + le32(1))
    for name, mut in {
        "header 0x5f": lambda p: p.__setitem__(0, 0x5F), "cmd 0x06": lambda p: p.__setitem__(1, 0x06),
        "cmd 0xE1": lambda p: p.__setitem__(1, 0xE1), "cmd 0xC5": lambda p: p.__setitem__(1, 0xC5),
        "xor key": lambda p: p.__setitem__(2, 1), "offset field": lambda p: p.__setitem__(5, 1),
        "checksum": lambda p: p.__setitem__(3, p[3] ^ 1), "payload after data": lambda p: p.__setitem__(40, 1),
        "word SAVF": lambda p: p.__setitem__(10 + 3, ord("F")), "length": lambda p: p.__setitem__(4, 11),
        "op becomes 7": lambda p: p.__setitem__(8, 7),
    }.items():
        p = bytearray(good)
        mut(p)
        refuses(lambda p=p: guard_config_request(bytes(p)), "tampered " + name)
        refuses(lambda p=p: guard_outgoing(bytes(p)), "tampered (outer) " + name)
    refuses(lambda: guard_config_request(good[:63]), "63 bytes")
    refuses(lambda: build_request(OP_COMMIT, 1, b"SAVX" + le16(1) + le32(1)), "commit without the word")
    refuses(lambda: build_request(OP_COMMIT, 1, b"\0\0\0\0" + le16(1) + le32(1)), "commit with a zero word")
    refuses(lambda: build_request(OP_SELECT_PROFILE, 1, b"SAVE\x01"), "profile with the commit word")
    refuses(lambda: build_request(OP_SET_PAGE, 1, le16(0)), "empty page")
    refuses(lambda: build_request(OP_SET_PAGE, 1, le16(0) + bytes(53)), "53-byte page")
    refuses(lambda: build_request(OP_SET_BEGIN, 1, le16(0)), "begin 0")
    refuses(lambda: build_request(OP_SET_BEGIN, 1, le16(8193)), "begin 8193")
    refuses(lambda: build_request(OP_GET_SETTINGS, 1, le16(0) + bytes([45, 0])), "want 45")
    refuses(lambda: build_request(OP_GET_SETTINGS, 1, le16(0) + bytes([44, 3])), "page 0 with an id")
    refuses(lambda: build_request(OP_LIVE_DEPTH, 1, bytes([120, 9])), "depth 129")
    refuses(lambda: build_request(OP_GET_INFO, 1, b"\0"), "info with an argument")
    refuses(lambda: guard_outgoing(bk.build_request(0x05, 0, 8)), "backup read command 0x05")
    refuses(lambda: guard_outgoing(b"\x55\xC5" + bytes(62)), "scope command")

    # 3. a full client session against the simulator
    sim = SimulatedConfigKeyboard()
    c = ConfigClient(sim)
    check(c.verify().endswith("KMFW-SIM"), "verify returns the build")
    check(c.info.profile_count == 4 and c.info.active_profile == 2 and c.info.feature_names()[0] == "read settings", "info fields")
    check(c.dump() == sim.blob and len(sim.blob) == 100, "dump equals the keyboard's settings")
    new = bytes((i * 11 + 5) & 0xFF for i in range(1000))
    c.stage(new)
    check(sim.flash_writes == 0 and sim.blob != new, "staging alone writes nothing")
    check(c.get_info().stage_state == 2, "staged and complete")
    check(c.commit(new) == 1 and sim.flash_writes == 1 and sim.blob == new, "commit writes exactly once")
    check(c.dump() == new, "dump after commit")
    check(c.select_profile(3) == 3 and sim.active == 3, "select profile")
    refuses(lambda: c.select_profile(4), "profile 4 (client side)")
    check(c.live_depth() == [1000 + 7 * s for s in range(76)], "live depth, 76 slots in 4 requests")
    blob4k = bytes((i * 13) & 0xFF for i in range(8192))
    c.stage(blob4k)
    check(c.commit(blob4k) == 2 and sim.blob == blob4k and c.dump() == blob4k, "8192-byte settings round trip")
    refuses(lambda: c.stage(bytes(8193)), "8193-byte blob")
    refuses(lambda: c.stage(b""), "empty blob")
    check(all(op in ALLOWED_OPS for cmd, op in sim.seen if cmd == CMD_CONFIG), "only whitelisted ops reached the keyboard")

    # 3b. protocol 2: status, scene read / write, refusals
    check(c.info.proto_version == 2 and c.info.scene_max == MAX_SCENE and c.info.has("device status") and c.info.has("light scene"), "info is protocol 2")
    st_ = c.status()
    check(st_["caps"] and not st_["num"] and st_["usb_fell_back"] and st_["profile"] == 2 and st_["layer"] == 1
          and st_["stream_frames"] == 0x1234 and st_["led_reports"] == 7, "device status fields")
    check(c.scene_dump() == sim.scene and len(sim.scene) == 50, "scene dump")
    sc_new = bytes((i * 3 + 1) & 0xFF for i in range(214))
    c.scene_stage(sc_new)
    check(sim.scene_flash_writes == 0 and sim.scene != sc_new and sim.flash_writes == 2, "scene staging alone writes nothing")
    check(c.scene_commit(sc_new) == 1 and sim.scene == sc_new and sim.scene_flash_writes == 1 and c.scene_dump() == sc_new, "scene commit")
    refuses(lambda: c.scene_stage(bytes(1025)), "1025-byte scene")
    refuses(lambda: c.scene_stage(b""), "empty scene")
    refuses(lambda: build_request(OP_SCENE_COMMIT, 1, b"SAVE" + le16(1) + le32(1)), "scene commit with the settings word")
    refuses(lambda: build_request(OP_COMMIT, 1, b"LITE" + le16(1) + le32(1)), "settings commit with the scene word")
    refuses(lambda: build_request(OP_SCENE_BEGIN, 1, le16(1025)), "scene begin 1025")
    refuses(lambda: build_request(OP_SCENE_BEGIN, 1, le16(0)), "scene begin 0")
    refuses(lambda: build_request(OP_DEVICE_STATUS, 1, b"\0"), "device status with an argument")
    refuses(lambda: build_request(OP_SCENE_PAGE, 1, le16(0)), "empty scene page")
    # a protocol-1 keyboard: the client refuses protocol-2 requests before sending them
    old = SimulatedConfigKeyboard(proto=1)
    co = ConfigClient(old)
    co.verify()
    check(co.info.proto_version == 1 and not co.info.has("device status") and co.info.scene_max == 0, "protocol 1 info")
    n_seen = len(old.seen)
    refuses(lambda: co.status(), "status on protocol 1")
    refuses(lambda: co.scene_dump(), "scene dump on protocol 1")
    check(len(old.seen) == n_seen, "nothing was sent for them")
    check(co.dump() == old.blob, "protocol 1 settings still work")
    # a lost reply of the scene commit is resolved by reading the scene back
    state2 = {"n": 0}

    def drop_commit_reply(s_, n, ph):
        if ph == "after" and s_.seen[-1][1] == OP_SCENE_COMMIT and state2["n"] == 0:
            state2["n"] = 1
            raise bk.BackupError("no response (injected)")
    sim5 = SimulatedConfigKeyboard(on_exchange=drop_commit_reply)
    c5 = ConfigClient(sim5)
    c5.verify()
    c5.scene_stage(sc_new)
    c5.scene_commit(sc_new)
    check(sim5.scene_flash_writes == 1 and sim5.scene == sc_new, "lost SCENE_COMMIT reply: exactly one scene write")

    # 4. stock firmware: refused before any 0xC6 packet
    stock = SimulatedConfigKeyboard(stock=True)
    refuses(lambda: ConfigClient(stock).verify(), "stock firmware")
    check(all(cmd == bk.CMD_GET_INFO for cmd, _ in stock.seen), "stock keyboard only saw the cmd 3 query")
    unver = ConfigClient(SimulatedConfigKeyboard())
    refuses(lambda: unver.call(OP_GET_INFO + 1, b""), "call before verify")
    check(is_kmfw_build("Jan  1 2026,00:00:00KMFW-K3") and is_kmfw_build("KMFW-X")
          and not is_kmfw_build("Jan  1 2026,00:00:00STOCK-SIM") and not is_kmfw_build("xKMFW"), "is_kmfw_build")

    # 5. packet loss: a lost reply and a lost request at every position of a stage+commit run
    def run_with_loss(n_drop, phase):
        def hook(sim_, n, ph):
            if n == n_drop and ph == phase:
                raise bk.BackupError("no response (injected)")
        s = SimulatedConfigKeyboard(on_exchange=hook)
        cl = ConfigClient(s)
        cl.verify()
        data = bytes((i * 7) & 0xFF for i in range(300))
        cl.stage(data)
        cl.commit(data)
        return s, data, cl

    total_exchanges = None
    s0 = SimulatedConfigKeyboard()
    cl0 = ConfigClient(s0)
    cl0.verify()
    d0 = bytes((i * 7) & 0xFF for i in range(300))
    cl0.stage(d0)
    cl0.commit(d0)
    total_exchanges = len(s0.seen)
    for k in range(2, total_exchanges + 1):          # exchange 1 is the cmd 3 query (not retried by design)
        for phase in ("before", "after"):
            try:
                s, data, cl = run_with_loss(k, phase)
                check(s.blob == data and s.flash_writes == 1, "loss at %d/%s: settings committed exactly once" % (k, phase))
            except ConfigError as exc:
                fails.append("loss at %d/%s: %s" % (k, phase, exc))
    # a lost reply to the COMMIT itself must not make a second flash write
    commit_idx = [i for i, (cmd, op) in enumerate(s0.seen, 1) if op == OP_COMMIT][0]
    s, data, cl = run_with_loss(commit_idx, "after")
    check(s.flash_writes == 1, "lost COMMIT reply: still one flash write")

    # 6. out-of-sequence recovery: the keyboard "forgets" a page (request lost silently after processing is simulated by
    #    dropping a page before it is processed, then the client's retry arrives) - and a hostile reorder
    sim2 = SimulatedConfigKeyboard()
    c2 = ConfigClient(sim2)
    c2.verify()
    c2.call(OP_SET_BEGIN, le16(200))
    rep = c2.call(OP_SET_PAGE, le16(52) + bytes(52), ok=(0, ST_SEQUENCE))
    check(rep.status == ST_SEQUENCE and rd16(rep.payload) == 0, "skipped page is answered SEQUENCE with the expected offset")
    check(c2.get_info().stage_next == 0, "nothing was written by the out-of-order page")

    # 7. dump retries after a stale snapshot (the keyboard re-snapshots in between, simulated by a second reader)
    sim3 = SimulatedConfigKeyboard(blob=bytes(range(200)))
    c3 = ConfigClient(sim3)
    c3.verify()
    state = {"done": False}

    def poke(s, n, ph):
        if ph == "after" and not state["done"] and s.seen[-1][1] == OP_GET_SETTINGS and s.snap_id == 1 and n > 3:
            state["done"] = True
            s.snap_valid = False                      # e.g. a commit by someone else dropped the snapshot
    sim3.on_exchange = poke
    check(c3.dump() == bytes(range(200)), "dump restarts after STALE")

    # 8. CLI behaviour: dry run is the default, confirmation words are required, nothing real is touched
    import io
    import contextlib
    import tempfile

    def cli(argv):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = main(argv)
        return rc, buf.getvalue()

    with tempfile.TemporaryDirectory() as tmp:
        f = Path(tmp) / "s.bin"
        f.write_bytes(bytes(range(64)))
        out = Path(tmp) / "o.bin"
        rc, text = cli(["info"])
        check(rc == 0 and "Dry run" in text and "KMFW-SIM" in text, "info dry run")
        rc, text = cli(["dump", str(out)])
        check(rc == 0 and "Dry run" in text and out.exists() is False, "dump dry run writes no file")
        rc, text = cli(["dump", str(out), "--sim"])
        check(rc == 0 and out.read_bytes() == SimulatedConfigKeyboard().blob, "dump --sim writes the simulated blob")
        rc, text = cli(["commit", str(f)])
        check(rc == 0 and "Dry run" in text, "commit dry run")
        rc, text = cli(["commit", str(f), "--run"])
        check(rc == 1 and "--confirm SAVE" in text, "commit --run without --confirm stops")
        rc, text = cli(["commit", str(f), "--run", "--confirm", "STAGE"])
        check(rc == 1 and "--confirm SAVE" in text, "commit with the wrong word stops")
        rc, text = cli(["load", str(f), "--run"])
        check(rc == 1 and "--confirm STAGE" in text, "load --run without --confirm stops")
        rc, text = cli(["select-profile", "1", "--run"])
        check(rc == 1 and "--confirm PROF" in text, "select-profile --run without --confirm stops")
        rc, text = cli(["select-profile", "1", "--confirm", "PROF"])
        check(rc == 0 and "Dry run" in text, "confirm without --run is still a dry run")
        rc, text = cli(["commit", str(Path(tmp) / "missing.bin"), "--sim", "--confirm", "SAVE"])
        check(rc == 1, "missing input file")
        rc, text = cli(["commit", str(f), "--sim", "--confirm", "SAVE"])
        check(rc == 0 and "committed" in text, "commit on the simulator with the word")

    print("SELF-TEST", "FAILED" if fails else "OK", "(%d golden vectors)" % len(vectors))
    for f_ in fails:
        print("  FAIL:", f_)
    return 1 if fails else 0


# --------------------------------------------------------------------------- main

def show_packets():
    print("cmd 3 query (first 16 bytes) :", bk.build_request(bk.CMD_GET_INFO, 0, 0)[:16].hex())
    print("GET_INFO                     :", build_request(OP_GET_INFO, 1)[:16].hex())
    print("GET_SETTINGS offset 0        :", build_request(OP_GET_SETTINGS, 2, le16(0) + bytes([GET_PAGE, 0]))[:16].hex())


def need_confirm(args) -> bool:
    """True when the command may run: read-only commands need only --run; the others also their word."""
    word = CLI_CONFIRM.get(args.cmd)
    if word is None or not args.run:
        return True
    if args.confirm != word:
        print("STOPPED: --confirm %s is required for '%s' with --run" % (word, args.cmd))
        return False
    return True


def run_command(client: ConfigClient, args, write_files: bool = True) -> int:
    print("firmware:", client.verify())
    print(client.info)
    if args.cmd == "info":
        return 0
    if args.cmd == "dump":
        blob = client.dump()
        if write_files:
            Path(args.file).write_bytes(blob)
            print("%d bytes, CRC-32 %08X written to %s" % (len(blob), crc32(blob), args.file))
        else:
            print("%d bytes, CRC-32 %08X read from the simulated keyboard (dry run: %s not written)" % (len(blob), crc32(blob), args.file))
        return 0
    if args.cmd == "depth":
        vals = client.live_depth(76)
        for i in range(0, len(vals), 8):
            print("  ".join("%2d:%4d" % (s, vals[s]) for s in range(i, min(i + 8, len(vals)))))
        return 0
    if args.cmd == "select-profile":
        print("active profile is now", client.select_profile(args.index))
        return 0
    if args.cmd == "status":
        for k_, v_ in client.status().items():
            print("  %-18s %s" % (k_, v_))
        return 0
    if args.cmd == "scene-dump":
        blob = client.scene_dump()
        if write_files:
            Path(args.file).write_bytes(blob)
            print("scene: %d bytes, CRC-32 %08X written to %s" % (len(blob), crc32(blob), args.file))
        else:
            print("scene: %d bytes, CRC-32 %08X read from the simulated keyboard (dry run: %s not written)" % (len(blob), crc32(blob), args.file))
        return 0
    if args.cmd in ("scene-load", "scene-commit"):
        blob = Path(args.file).read_bytes()
        print("staging scene %d bytes, CRC-32 %08X (RAM only)" % (len(blob), crc32(blob)))
        client.scene_stage(blob)
        if args.cmd == "scene-load":
            print("staged; nothing was written to flash. 'scene-commit' stages again and commits.")
            return 0
        n = client.scene_commit(blob)
        back = client.scene_dump()
        print("scene committed (commit #%s); read back %d bytes: %s" % (n if n >= 0 else "?", len(back), "identical" if back == blob else "DIFFERENT"))
        return 0 if back == blob else 1
    blob = Path(args.file).read_bytes()
    print("staging %d bytes, CRC-32 %08X (RAM only)" % (len(blob), crc32(blob)))
    client.stage(blob)
    if args.cmd == "load":
        print("staged and complete; nothing was written to flash. 'commit' stages again and commits.")
        return 0
    n = client.commit(blob)
    back = client.dump()
    print("committed (commit #%d); read back %d bytes: %s" % (n, len(back), "identical" if back == blob else "DIFFERENT"))
    return 0 if back == blob else 1


def main(argv=None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    sub = ap.add_subparsers(dest="cmd")
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--run", action="store_true", help="talk to the real keyboard (our firmware only)")
    common.add_argument("--sim", action="store_true", help="use the simulated keyboard (default without --run)")
    common.add_argument("--confirm", default="", help="the confirmation word for commands that change state")
    sub.add_parser("info", parents=[common], help="protocol info")
    p = sub.add_parser("dump", parents=[common], help="save the live settings blob")
    p.add_argument("file")
    p = sub.add_parser("load", parents=[common], help="stage a settings file in the keyboard's RAM (no flash)")
    p.add_argument("file")
    p = sub.add_parser("commit", parents=[common], help="stage a settings file, COMMIT it (writes flash), read it back")
    p.add_argument("file")
    p = sub.add_parser("select-profile", parents=[common], help="make profile N the active one")
    p.add_argument("index", type=int)
    sub.add_parser("depth", parents=[common], help="live depths of the 76 slots")
    sub.add_parser("status", parents=[common], help="protocol 2: device status (host lock lights, gamepad mode, USB fall-back, ...)")
    p = sub.add_parser("scene-dump", parents=[common], help="protocol 2: save the live light scene blob")
    p.add_argument("file")
    p = sub.add_parser("scene-load", parents=[common], help="protocol 2: stage a scene file in RAM (no flash)")
    p.add_argument("file")
    p = sub.add_parser("scene-commit", parents=[common], help="protocol 2: stage a scene file, COMMIT it (writes flash), read it back")
    p.add_argument("file")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.cmd is None:
        ap.print_help()
        return 2
    if not need_confirm(args):
        return 1
    try:
        if args.cmd in ("load", "commit", "scene-load", "scene-commit") and not Path(args.file).is_file():
            print("ERROR: no such file:", args.file)
            return 1
        if args.run and not args.sim:
            iface = bk.pick_interface()
            t = ConfigTransport(iface["path"])
            try:
                return run_command(ConfigClient(t, log=print), args)
            finally:
                t.close()
        if not args.sim:
            show_packets()
            print("Dry run: nothing was sent to a keyboard; this is a demonstration on a SIMULATED keyboard. "
                  "--run talks to the real one (changing commands also need --confirm <word>).\n")
        return run_command(ConfigClient(SimulatedConfigKeyboard()), args, write_files=args.sim)
    except (bk.BackupError, bk.SafetyError, ConfigError) as exc:
        print("ERROR:", exc)
        return 1


if __name__ == "__main__":
    sys.exit(main())
