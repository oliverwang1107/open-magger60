#!/usr/bin/env python3
"""magger60_scope.py - read the "scope" channel of OUR custom Magger60 firmware (M3 sensing): the sensor values the
firmware publishes with km_scope_publish(), shown as a table, a live heat-map, or logged to CSV.

It only talks to firmware built from this repository (the cmd 3 build string carries the KMFW label). The stock
firmware is never sent the scope command (0xC5). Everything the tool can send is checked twice (guard_outgoing, called
by the real transport and by the simulator): the cmd 3 version query and the read-only scope request, nothing else.
No flash, no writes.

    python tools/magger60_scope.py                      # dry run: show the packets, demo on a SIMULATED keyboard
    python tools/magger60_scope.py --self-test          # no hardware: protocol, safety and rendering checks
    python tools/magger60_scope.py --run                # read once, print the table of the 76 slots
    python tools/magger60_scope.py --run --live         # refresh the heat-map until Ctrl+C
    python tools/magger60_scope.py --run --csv out.csv  # log every new snapshot with timestamps (add --seconds N)
    python tools/magger60_scope.py --sim --live         # the same displays against the simulated keyboard

Protocol (firmware/hid/include/km/km_scope.h): request 'U' 0xC5, offset = start | want_tag << 8, len = bytes accepted;
reply data = tag, seq, count (LE16 each) then up to 25 LE16 values. A snapshot is read page by page and accepted only
if tag and seq are the same in every page (a publish in between = torn read = read again).
"""
import argparse
import csv
import importlib.util
import io
import json
import math
import re
import sys
import time
from datetime import datetime
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

CMD_SCOPE = 0xC5
MAX_VALUES = 128
HEADER_BYTES = 6
PAGE_VALUES = (bk.MAX_CHUNK - HEADER_BYTES) // 2     # 25 values per reply
TAG_S1, TAG_S2, TAG_S3 = 0x0001, 0x0002, 0x0003
TAG_NAMES = {TAG_S1: "S1 single readings", TAG_S2: "S2 raw scan", TAG_S3: "S3 16-point sums"}
VECTORS = REPO / "firmware/tests/golden/scope_vectors.h"
FIRMWARE_LABEL = "KMFW"

# Slot -> (key name, row, col). The names come from the slot map measured on the keyboard (one press per key,
# firmware/core/data/slotmap_measured.json); the grid cell of a key is its place in the ANSI 60 % layout that the
# measurement walked through (tools/magger60_keymap_measure.py: row, then position inside the row). '' = no key was
# found on that slot (an ADC4 mirror or a floating input).
MEASURED_MAP = REPO / "firmware/core/data/slotmap_measured.json"


def load_slots(path=MEASURED_MAP):
    km = _load("magger60_keymap_measure")
    cell, pos = {}, 0
    for r, length in enumerate(km.ROW_LENGTHS):
        for c, name in enumerate(km.ORDER[pos:pos + length]):
            cell[name] = (r, c)
        pos += length
    named = json.loads(Path(path).read_text(encoding="utf-8"))["slots"]
    out = []
    for s in range(76):
        name = named.get(str(s))
        out.append((name, *cell[name]) if name is not None else ("", None, None))
    return out


SLOTS = load_slots()
GRID_ROWS, GRID_COLS = 5, 15


class ScopeError(Exception):
    """The keyboard did not answer as the scope protocol says, or it is not our firmware."""


class NothingPublished(ScopeError):
    """The firmware is ours but has not published anything yet."""


# --------------------------------------------------------------------------- packets

def build_request(start: int = 0, length: int = bk.MAX_CHUNK, want_tag: int = 0) -> bytes:
    """Scope request: 'U', cmd 0xC5, no XOR, len = reply data bytes accepted, offset = start | want_tag << 8, no payload."""
    if not 0 <= start <= 0xFF:
        raise bk.SafetyError("start %d out of range" % start)
    if not 0 <= length <= bk.MAX_CHUNK:
        raise bk.SafetyError("length %d out of range" % length)
    if not 0 <= want_tag <= 0xFFFF:
        raise bk.SafetyError("tag out of range")
    p = bytearray(bk.REPORT_LEN)
    p[0] = bk.HDR_NORMAL
    p[1] = CMD_SCOPE
    p[3] = bk.checksum(length, start | (want_tag << 8))
    p[4] = length
    p[5] = start
    p[6] = want_tag & 0xFF
    p[7] = want_tag >> 8
    return guard_scope_request(bytes(p))


def guard_scope_request(packet: bytes) -> bytes:
    if len(packet) != bk.REPORT_LEN:
        raise bk.SafetyError("packet is not 64 bytes")
    if packet[0] != bk.HDR_NORMAL:
        raise bk.SafetyError("only 'U' packets may be sent (raw '_' packets can reboot the keyboard)")
    if packet[2] != 0:
        raise bk.SafetyError("XOR-masked packets are not allowed")
    if packet[1] != CMD_SCOPE:
        raise bk.SafetyError("not the scope command")
    if packet[4] > bk.MAX_CHUNK:
        raise bk.SafetyError("length field above %d" % bk.MAX_CHUNK)
    if any(packet[8:]):
        raise bk.SafetyError("a scope request must not carry payload bytes")
    if packet[3] != bk.checksum(packet[4], packet[5] | (packet[6] << 8) | (packet[7] << 16)):
        raise bk.SafetyError("checksum does not match the header")
    return packet


def guard_outgoing(packet: bytes) -> bytes:
    """The only packets this tool may send: the cmd 3 version query and the scope request."""
    if len(packet) != bk.REPORT_LEN:
        raise bk.SafetyError("packet is not 64 bytes")
    if packet[1] == bk.CMD_GET_INFO:
        return bk.guard_outgoing(packet)
    if packet[1] == CMD_SCOPE:
        return guard_scope_request(packet)
    raise bk.SafetyError("command 0x%02X is not allowed in the scope tool" % packet[1])


class Page:
    def __init__(self, tag, seq, count, start, values):
        self.tag, self.seq, self.count, self.start, self.values = tag, seq, count, start, values


def decode_reply(resp: bytes) -> Page:
    if len(resp) != bk.REPORT_LEN:
        raise ScopeError("reply is not 64 bytes")
    if resp[0] == bk.HDR_NORMAL or resp[0] == 0xAB:
        raise ScopeError("the keyboard reports a checksum error (0xAB)" if resp[0] == 0xAB else "unexpected reply header")
    if resp[0] != 0xAA or resp[1] != CMD_SCOPE:
        raise ScopeError("unexpected reply: %s" % resp[:16].hex())
    n = resp[4]
    if n < HEADER_BYTES or n > bk.MAX_CHUNK or n % 2:
        raise ScopeError("bad reply length %d (is this really our firmware?)" % n)
    data = resp[8:8 + n]
    if (n + resp[5] + resp[6] + resp[7] + sum(data)) & 0xFF != resp[3]:
        raise ScopeError("reply checksum does not match: %s" % resp[:16].hex())
    if resp[6] or resp[7]:
        raise ScopeError("reply offset above 255")
    tag = data[0] | (data[1] << 8)
    seq = data[2] | (data[3] << 8)
    count = data[4] | (data[5] << 8)
    if count > MAX_VALUES:
        raise ScopeError("count %d above %d" % (count, MAX_VALUES))
    values = [data[i] | (data[i + 1] << 8) for i in range(HEADER_BYTES, n, 2)]
    if len(values) > PAGE_VALUES or (values and resp[5] + len(values) > count):
        raise ScopeError("reply carries more values than the snapshot has")
    if any(resp[8 + n:]):
        raise ScopeError("reply has data beyond its length")
    return Page(tag, seq, count, resp[5], values)


def is_scope_build(build: str) -> bool:
    """True for a cmd 3 build string of our firmware: [optional "Mon dd yyyy,hh:mm:ss" stamp] + "KMFW...".
    Other firmware (the stock one) does not carry the KMFW label."""
    m = re.match(r"^[A-Z][a-z]{2} [ 0-9][0-9] [0-9]{4},[0-9]{2}:[0-9]{2}:[0-9]{2}", build)
    rest = build[m.end():] if m else build
    return rest.startswith(FIRMWARE_LABEL)


# --------------------------------------------------------------------------- transport and client

class ScopeTransport(bk.HidapiTransport):
    """The backup tool's HID transport, but with this tool's own allow-list (backup's would refuse 0xC5)."""

    def exchange(self, packet: bytes) -> bytes:
        packet = guard_outgoing(packet)
        self._dev.write(b"\x00" + packet)
        data = self._dev.read(bk.REPORT_LEN, self._timeout)
        if not data or len(data) < bk.REPORT_LEN:
            raise bk.BackupError("no (or short) response from the keyboard")
        return bytes(data[:bk.REPORT_LEN])


class Snapshot:
    def __init__(self, tag, seq, values):
        self.tag, self.seq, self.values = tag, seq, values


class ScopeClient:
    def __init__(self, transport: bk.Transport):
        self.t = transport
        self.verified = False
        self.build = None

    def verify(self) -> str:
        """Ask cmd 3 and demand our firmware. Until this passes the scope command is never sent."""
        d = info.decode(self.t.exchange(bk.build_request(bk.CMD_GET_INFO, 0, 0)))
        self.build = d["build"]
        if not is_scope_build(self.build):
            raise ScopeError("this keyboard runs '%s', not our firmware (build string must carry %s); "
                             "the scope command was NOT sent" % (self.build, FIRMWARE_LABEL))
        self.verified = True
        return self.build

    def page(self, start: int, length: int = bk.MAX_CHUNK, want_tag: int = 0) -> Page:
        if not self.verified:
            raise ScopeError("firmware not verified yet; call verify() first")
        return decode_reply(self.t.exchange(build_request(start, length, want_tag)))

    def snapshot(self, want_tag: int = 0, retries: int = 6) -> Snapshot:
        for _ in range(retries):
            p0 = self.page(0, bk.MAX_CHUNK, want_tag)
            if p0.tag == 0 or p0.seq == 0:
                raise NothingPublished("the firmware has not published anything yet")
            if want_tag and p0.tag != want_tag:
                raise ScopeError("tag 0x%04X is not published; the keyboard publishes tag 0x%04X" % (want_tag, p0.tag))
            values = list(p0.values)
            torn = False
            while len(values) < p0.count:
                p = self.page(len(values), bk.MAX_CHUNK, p0.tag)
                if p.tag != p0.tag or p.seq != p0.seq or p.count != p0.count or p.start != len(values) or not p.values:
                    torn = True               # a publish happened between two pages: start again
                    break
                values += p.values
            if not torn:
                return Snapshot(p0.tag, p0.seq, values)
        raise ScopeError("could not get a consistent snapshot in %d tries (the firmware publishes faster than the "
                         "pages can be read)" % retries)


# --------------------------------------------------------------------------- simulated keyboard

class SimulatedScopeKeyboard(bk.Transport):
    """A keyboard that follows the protocol in km_scope.h (an independent Python model of the C code). scope=False
    behaves like the stock firmware: cmd 3 with the stock build string, everything else 'unknw'."""

    STOCK_BUILD = "Jan  1 2026,00:00:00STOCK-SIM"
    OUR_BUILD = "Jan  1 2026,00:00:00KMFW-SIM"

    def __init__(self, scope: bool = True, build: str = None, on_exchange=None, animate=None):
        self.scope = scope
        self.build = build or (self.OUR_BUILD if scope else self.STOCK_BUILD)
        self.on_exchange = on_exchange       # called as f(sim, number of exchanges so far) after each reply
        self.animate = animate               # called as f(sim) before each reply (demo mode)
        self.seen_cmds = []
        self.tag = self.seq = 0
        self.values = []

    def publish(self, tag, values):
        if tag == 0:
            return
        self.values = [v & 0xFFFF for v in values][:MAX_VALUES]
        self.tag = tag
        self.seq = self.seq + 1 if self.seq != 0xFFFF else 1

    @staticmethod
    def _reply(cmd, data, offset=0, header=0xAA):
        r = bytearray(bk.REPORT_LEN)
        r[0], r[1] = header, cmd
        r[3] = (len(data) + offset + sum(data)) & 0xFF
        r[4] = len(data)
        r[5] = offset
        r[8:8 + len(data)] = data
        return bytes(r)

    def exchange(self, packet: bytes) -> bytes:
        packet = guard_outgoing(packet)
        cmd = packet[1]
        self.seen_cmds.append(cmd)
        if self.animate:
            self.animate(self)
        if cmd == bk.CMD_GET_INFO:
            data = bytes([0x04, 0x01]) + self.build.encode("ascii")
            out = self._reply(cmd, data)
        elif not self.scope:
            out = self._reply(cmd, b"unknw")
        else:
            start, want = packet[5], packet[6] | (packet[7] << 8)
            n = 0
            if want in (0, self.tag):
                n = (packet[4] - HEADER_BYTES) // 2 if packet[4] >= HEADER_BYTES else 0
                n = max(0, min(n, PAGE_VALUES, len(self.values) - start))
            data = bytearray([self.tag & 0xFF, self.tag >> 8, self.seq & 0xFF, self.seq >> 8,
                              len(self.values) & 0xFF, len(self.values) >> 8])
            for v in self.values[start:start + n]:
                data += bytes([v & 0xFF, v >> 8])
            out = self._reply(cmd, bytes(data), start)
        if self.on_exchange:
            self.on_exchange(self, len(self.seen_cmds))
        return out


def demo_animator(period_s: float = 6.0):
    """Animate a simulated keyboard: ~2000 at rest with a little noise, one key at a time pressed down (lower value)."""
    t0 = time.monotonic()
    state = {"last": -1}

    def animate(sim):
        t = time.monotonic() - t0
        frame = int(t * 20)                      # 20 snapshots per second
        if frame == state["last"]:
            return
        state["last"] = frame
        pressed = int(t / period_s * 76) % 76
        depth = 0.5 - 0.5 * math.cos(2 * math.pi * ((t % period_s) * 76 / period_s % 1.0))
        vals = []
        for s in range(76):
            v = 2000 + ((s * 7 + frame * 13) % 9) - 4
            if s == pressed:
                v -= int(depth * 1300)
            vals.append(v)
        sim.publish(TAG_S2, vals)
    return animate


# --------------------------------------------------------------------------- display

def tag_name(tag):
    return TAG_NAMES.get(tag, "tag 0x%04X" % tag)


def key_label(slot):
    """Short label for a key: the first word, or the second when the first is a shifted symbol."""
    name = SLOTS[slot][0]
    parts = name.split()
    if len(parts) == 2 and parts[0] in "_+{}|:\"<>?~!@#$%^&*()":
        return parts[1][:5]
    return (parts[0] if parts else "")[:5]


def have_slot_names(snap):
    return snap.tag in (TAG_S2, TAG_S3) and len(snap.values) == len(SLOTS)


def format_table(snap):
    lines = ["%s   seq %d   %d values" % (tag_name(snap.tag), snap.seq, len(snap.values))]
    if have_slot_names(snap):
        lines.append("slot  key        row,col   value   hex")
        for s, v in enumerate(snap.values):
            name, row, col = SLOTS[s]
            lines.append("%4d  %-9s  %-7s  %6d  0x%04X" % (s, name or "-", "" if row is None else "%d,%d" % (row, col), v, v))
    else:
        lines.append("index   value   hex")
        for i, v in enumerate(snap.values):
            lines.append("%5d  %6d  0x%04X" % (i, v, v))
    return "\n".join(lines)


def pick_bar_chars(stream=None):
    """Block characters when the terminal can show them, plain ASCII otherwise."""
    uni = " ▁▂▃▄▅▆▇█"
    try:
        uni.encode((stream or sys.stdout).encoding or "ascii")
        return uni
    except (UnicodeEncodeError, LookupError):
        return " .:-=+*#%@"


class Heat:
    """Per-slot heat: how far the value is from its resting value (the first snapshot), scaled by the largest move seen
    on that slot (at least min_span counts). Pressing a Hall-effect key moves the value, either way."""

    def __init__(self, min_span: int = 64):
        self.base = None
        self.span = None
        self.min_span = min_span

    def level(self, values):
        if self.base is None or len(self.base) != len(values):
            self.base = list(values)
            self.span = [self.min_span] * len(values)
        out = []
        for i, v in enumerate(values):
            d = abs(v - self.base[i])
            self.span[i] = max(self.span[i], d)
            out.append(d / self.span[i])
        return out


def format_heat(snap, heat: Heat, chars: str) -> str:
    vals = snap.values
    lv = heat.level(vals)
    digits = max(4, len(str(max(vals))) if vals else 4)
    cell = digits + 2
    lines = ["%s   seq %d    (bar = distance from the resting value; Ctrl+C quits)" % (tag_name(snap.tag), snap.seq)]
    if not have_slot_names(snap):
        for i in range(0, len(vals), 8):
            lines.append("  ".join("%3d:%-*d%s" % (j, digits, vals[j], chars[min(len(chars) - 1, round(lv[j] * (len(chars) - 1)))])
                                   for j in range(i, min(i + 8, len(vals)))))
        return "\n".join(lines)
    grid = {}
    for s, (_name, row, col) in enumerate(SLOTS):
        if row is not None:
            grid[(row, col)] = s
    for r in range(GRID_ROWS):
        names, bars = [], []
        for c in range(GRID_COLS):
            s = grid.get((r, c))
            if s is None:
                names.append(" " * cell)
                bars.append(" " * cell)
            else:
                names.append(key_label(s).ljust(cell))
                bar = chars[min(len(chars) - 1, round(lv[s] * (len(chars) - 1)))]
                bars.append(bar + str(vals[s]).rjust(digits) + " ")
        lines.append("".join(names).rstrip())
        lines.append("".join(bars).rstrip())
    other = ["%d%s=%d" % (s, "(" + SLOTS[s][0] + ")" if SLOTS[s][0] else "", vals[s])
             for s in range(len(SLOTS)) if SLOTS[s][1] is None]
    for i in range(0, len(other), 6):
        lines.append(("no key mapped: " if i == 0 else "               ") + "  ".join(other[i:i + 6]))
    return "\n".join(lines)


class CsvLog:
    """One row per new snapshot: time, seconds since start, tag, seq, then the values. Rows of a different tag or
    count than the first are skipped (one file = one kind of data)."""

    def __init__(self, fh):
        self.w = csv.writer(fh)
        self.t0 = time.monotonic()
        self.shape = None
        self.last_seq = None
        self.rows = 0

    def add(self, snap, now=None, mono=None) -> bool:
        if (snap.tag, len(snap.values)) != (self.shape or (snap.tag, len(snap.values))):
            return False
        if snap.seq == self.last_seq:
            return False
        if self.shape is None:
            self.shape = (snap.tag, len(snap.values))
            self.w.writerow(["time", "t_s", "tag", "seq"] + ["s%d" % i for i in range(len(snap.values))])
        self.last_seq = snap.seq
        now = now or datetime.now()
        mono = time.monotonic() if mono is None else mono
        self.w.writerow([now.isoformat(timespec="milliseconds"), "%.3f" % (mono - self.t0), snap.tag, snap.seq] + snap.values)
        self.rows += 1
        return True


# --------------------------------------------------------------------------- self-test

def load_vectors():
    src = VECTORS.read_text(encoding="utf-8")
    return [(m.group(1), int(m.group(2)), bytes.fromhex(m.group(3)), bytes.fromhex(m.group(4)))
            for m in re.finditer(r'\{ "([^"]*)", (\d), "([0-9a-f]*)", "([0-9a-f]*)" \}', src)]


def vector_snapshot(snap):
    """The three snapshots the C golden vectors use (firmware/tests/golden/scope_vectors.h)."""
    if snap == 0:
        return 0x0002, [(i * 1337 + 11) & 0xFFFF for i in range(76)]
    if snap == 1:
        return 0x0003, [(i * 0x0201 + 0x7FF0) & 0xFFFF for i in range(128)]
    return 0, []


def self_test() -> int:
    fails = []

    def check(cond, msg):
        if not cond:
            fails.append(msg)

    def raises(exc, fn, *a):
        try:
            fn(*a)
        except exc:
            return True
        except Exception as e:                                  # noqa: BLE001 - wrong exception type is a failure
            fails.append("%s raised %r" % (getattr(fn, "__name__", fn), e))
            return False
        return False

    # 1. the C golden packets (generated from the written spec, checked by the C tests too)
    vectors = load_vectors()
    check(len(vectors) >= 20, "golden vectors missing (%d)" % len(vectors))
    for name, snap, req, reply in vectors:
        start, length, tag = req[5], req[4], req[6] | (req[7] << 8)
        check(build_request(start, length, tag) == req, "%s: request bytes differ from the golden request" % name)
        sim = SimulatedScopeKeyboard()
        stag, svals = vector_snapshot(snap)
        sim.publish(stag, svals)
        check(sim.exchange(req) == reply, "%s: simulated reply differs from the golden reply" % name)
        pg = decode_reply(reply)
        check(pg.tag == stag and pg.count == len(svals) and pg.seq == (0 if snap == 2 else 1), "%s: header decode" % name)
        want = svals[start:start + len(pg.values)]
        check(pg.values == want, "%s: values decode" % name)
        check(len(pg.values) == len(pg.values[:PAGE_VALUES]), "%s: more than 25 values" % name)

    # 2. nothing but cmd 3 and the scope request can pass the guard
    ok_cmds = []
    for cmd in range(256):
        p = bytearray(64)
        p[0], p[1], p[3] = 0x55, cmd, 0
        try:
            guard_outgoing(bytes(p))
            ok_cmds.append(cmd)
        except bk.SafetyError:
            pass
    check(ok_cmds == [3, CMD_SCOPE], "guard let through %s" % [hex(c) for c in ok_cmds])
    good = build_request(0, 56, 0)
    for label, mut in (("raw header", lambda b: b.__setitem__(0, 0x5F)), ("xor key", lambda b: b.__setitem__(2, 1)),
                       ("bad checksum", lambda b: b.__setitem__(3, b[3] ^ 1)), ("len 57", lambda b: b.__setitem__(4, 57)),
                       ("payload", lambda b: b.__setitem__(20, 1)), ("other cmd 0xE1", lambda b: b.__setitem__(1, 0xE1))):
        b = bytearray(good)
        mut(b)
        check(raises(bk.SafetyError, guard_outgoing, bytes(b)), "guard accepted: " + label)
    check(raises(bk.SafetyError, build_request, 256, 56, 0), "start 256 accepted")
    check(raises(bk.SafetyError, build_request, 0, 57, 0), "length 57 accepted")
    check(raises(bk.SafetyError, build_request, 0, 56, 0x10000), "tag 0x10000 accepted")
    check(raises(bk.SafetyError, guard_outgoing, good[:63]), "short packet accepted")

    # 3. the stock firmware is never sent the scope command
    check(is_scope_build("Jan  1 2026,00:00:00KMFW-D4") and is_scope_build("KMFW-x") and is_scope_build("Feb  3 2027,01:02:03KMFW"),
          "KMFW build strings must be accepted")
    for bad in ("Jan  1 2026,00:00:00STOCK-SIM", "", "xKMFW", "Jan  1 2026,00:00:00xKMFW", "kmfw", "Jan  1 2026,00:00:00 KMFW"):
        check(not is_scope_build(bad), "accepted build string %r" % bad)
    stock = SimulatedScopeKeyboard(scope=False)
    cl = ScopeClient(stock)
    check(raises(ScopeError, cl.verify), "stock firmware was verified")
    check(raises(ScopeError, cl.page, 0), "page read before verify")
    check(raises(ScopeError, cl.snapshot), "snapshot read before verify")
    check(stock.seen_cmds == [3], "stock firmware saw %s" % [hex(c) for c in stock.seen_cmds])
    nostamp = SimulatedScopeKeyboard(build="STOCK-SIM-KMFW")
    check(raises(ScopeError, ScopeClient(nostamp).verify) and nostamp.seen_cmds == [3], "label in the wrong place accepted")

    # 4. paging, torn reads, errors, against the simulator
    def fresh(values_by_tag, **kw):
        s = SimulatedScopeKeyboard(**kw)
        for tag, vals in values_by_tag:
            s.publish(tag, vals)
        c = ScopeClient(s)
        c.verify()
        return s, c

    v76 = [(i * 1337 + 11) & 0xFFFF for i in range(76)]
    v128 = [(i * 0x0201 + 0x7FF0) & 0xFFFF for i in range(128)]
    s, c = fresh([(TAG_S2, v76)])
    snap = c.snapshot()
    check(snap.values == v76 and snap.tag == TAG_S2 and snap.seq == 1, "76-value read")
    check(s.seen_cmds == [3, CMD_SCOPE, CMD_SCOPE, CMD_SCOPE, CMD_SCOPE], "76 values must take 4 requests: %s" % s.seen_cmds)
    s, c = fresh([(TAG_S3, v128)])
    check(c.snapshot(TAG_S3).values == v128 and s.seen_cmds.count(CMD_SCOPE) == 6, "128-value read in 6 requests")
    s, c = fresh([(TAG_S1, [])])
    check(c.snapshot().values == [] and s.seen_cmds.count(CMD_SCOPE) == 1, "empty snapshot")
    s, c = fresh([(TAG_S1, [5])])
    check(c.snapshot().values == [5], "one value")
    s, c = fresh([(TAG_S2, v76[:25])])
    check(c.snapshot().values == v76[:25] and s.seen_cmds.count(CMD_SCOPE) == 1, "exactly one full page")
    s, c = fresh([(TAG_S2, v76[:26])])
    check(c.snapshot().values == v76[:26] and s.seen_cmds.count(CMD_SCOPE) == 2, "one full page and one value")
    s, c = fresh([])
    check(raises(NothingPublished, c.snapshot), "nothing published must be reported")
    s, c = fresh([(TAG_S2, v76)])
    check(raises(ScopeError, c.snapshot, TAG_S3), "wrong wanted tag must be an error")
    check(c.snapshot(TAG_S2).values == v76, "right wanted tag")

    # torn read: a publish between page 1 and page 2 makes the client read again and return only the new snapshot
    v76b = [(v + 1) & 0xFFFF for v in v76]
    state = {"done": False}

    def once(sim, n):
        if n == 3 and not state["done"]:                    # 1 = cmd 3, 2 = page 0, 3 = page 1
            state["done"] = True
            sim.publish(TAG_S2, v76b)
    s, c = fresh([(TAG_S2, v76)], on_exchange=once)
    snap = c.snapshot()
    check(snap.values == v76b and snap.seq == 2, "torn read was not repaired: seq %d" % snap.seq)
    # tearing forever -> a clear error, not mixed data
    s, c = fresh([(TAG_S2, v76)], on_exchange=lambda sim, n: sim.publish(TAG_S2, v76b) if n > 1 else None)
    check(raises(ScopeError, c.snapshot), "endless tearing must end in an error")
    # a tag change between pages counts as torn too
    state2 = {"done": False}

    def retag(sim, n):
        if n == 3 and not state2["done"]:
            state2["done"] = True
            sim.publish(TAG_S3, v76b)
    s, c = fresh([(TAG_S2, v76)], on_exchange=retag)
    check(c.snapshot().tag == TAG_S3, "tag change between pages")

    # 5. decode_reply refuses anything that is not a well-formed scope reply
    s, c = fresh([(TAG_S2, v76)])
    good_reply = s.exchange(build_request(0, 56, 0))
    for label, mut in (("header 0xAB", lambda b: b.__setitem__(0, 0xAB)), ("wrong cmd", lambda b: b.__setitem__(1, 3)),
                       ("bad checksum", lambda b: b.__setitem__(3, b[3] ^ 1)), ("odd length", lambda b: b.__setitem__(4, 7)),
                       ("short length", lambda b: b.__setitem__(4, 4)), ("length 58", lambda b: b.__setitem__(4, 58)),
                       ("count 129", lambda b: b.__setitem__(12, 129)), ("offset high byte", lambda b: b.__setitem__(6, 1)),
                       ("data past length", lambda b: b.__setitem__(63, 1))):
        b = bytearray(good_reply)
        mut(b)
        check(raises(ScopeError, decode_reply, bytes(b)), "decode_reply accepted: " + label)
    check(raises(ScopeError, decode_reply, good_reply[:63]), "short reply accepted")
    check(raises(ScopeError, decode_reply, SimulatedScopeKeyboard._reply(CMD_SCOPE, b"unknw")), "stock 'unknw' reply accepted")
    check(raises(ScopeError, decode_reply, bytes([0xAB, CMD_SCOPE, 0, 0x04 + sum(b"cscr") & 0xFF, 4, 0, 0, 0]) + b"cscr" + bytes(52)),
          "device checksum error not reported")

    # 6. slot table and rendering
    check(len(SLOTS) == 76 and sum(1 for k in SLOTS if k[1] is not None) == 61, "slot table: 61 keys with a position")
    pos = [(r, c_) for _n, r, c_ in SLOTS if r is not None]
    check(len(pos) == len(set(pos)) and all(0 <= r < GRID_ROWS and 0 <= c_ < GRID_COLS for r, c_ in pos), "slot positions")
    measured = json.loads(MEASURED_MAP.read_text(encoding="utf-8"))
    check(all(SLOTS[int(s_)][0] == n for s_, n in measured["slots"].items()), "slot names follow the measured map")
    check(all(SLOTS[int(s_)][0] == "" for s_ in measured["no_key_slots"]), "no-key slots have no name")
    check(SLOTS[18][:3] == ("ESC", 0, 0) and SLOTS[75][0] == "W", "ESC is the top-left key, slot 75 is W")
    snap = Snapshot(TAG_S2, 7, [2000 + i for i in range(76)])
    table = format_table(snap)
    check("ESC" in table and "seq 7" in table and len(table.splitlines()) == 78, "table rendering")
    check("0x07D0" in table.splitlines()[2], "table hex column")
    check(format_table(Snapshot(TAG_S1, 1, [1, 2])).splitlines()[1].startswith("index"), "raw table for other tags")
    heat = Heat()
    chars = " .:-=+*#%@"
    frame0 = format_heat(snap, heat, chars)
    moved = Snapshot(TAG_S2, 8, [v - (1000 if i == 18 else 0) for i, v in enumerate(snap.values)])
    frame1 = format_heat(moved, heat, chars)
    check(len(frame0.splitlines()) == 1 + 2 * GRID_ROWS + 3, "heat frame size")
    check("@" in frame1 and "@" not in frame0, "heat bar for the pressed key (ESC)")
    check(frame1.splitlines()[1].startswith("ESC") and frame1.splitlines()[2].startswith("@"), "ESC is the top-left cell")
    check(max(len(x) for x in frame1.splitlines()) <= 110, "heat frame too wide")
    check(len(pick_bar_chars(io.StringIO())) >= 2, "bar characters")
    buf = io.StringIO()
    log = CsvLog(buf)
    check(log.add(snap, now=datetime(2026, 10, 27, 12, 0, 0), mono=log.t0 + 1.5), "csv: first row")
    check(not log.add(snap), "csv: same seq twice")
    check(not log.add(Snapshot(TAG_S1, 9, [1, 2, 3])), "csv: other shape skipped")
    check(log.add(moved, now=datetime(2026, 10, 27, 12, 0, 1), mono=log.t0 + 2.5), "csv: second row")
    lines = buf.getvalue().splitlines()
    check(len(lines) == 3 and lines[0].startswith("time,t_s,tag,seq,s0,s1") and lines[0].endswith("s75"), "csv header")
    check(lines[1].startswith("2026-10-27T12:00:00.000,1.500,2,7,2000,2001") and lines[2].split(",")[4 + 18] == "1018", "csv rows")

    print("SELF-TEST", "FAILED" if fails else "OK", "(%d golden vectors)" % len(vectors))
    for f in fails:
        print("  FAIL:", f)
    return 1 if fails else 0


# --------------------------------------------------------------------------- main

def clear_screen():
    sys.stdout.write("\x1b[H\x1b[J")


def run_session(client: ScopeClient, args) -> int:
    print("firmware: %s" % client.verify())
    want = args.tag
    if args.csv:
        deadline = time.monotonic() + args.seconds if args.seconds else None
        with open(args.csv, "w", newline="", encoding="utf-8") as fh:
            log = CsvLog(fh)
            try:
                while deadline is None or time.monotonic() < deadline:
                    try:
                        log.add(client.snapshot(want))
                    except NothingPublished:
                        pass
                    time.sleep(args.interval)
            except KeyboardInterrupt:
                pass
            print("%d rows written to %s" % (log.rows, args.csv))
        return 0
    if args.live:
        heat = Heat()
        chars = pick_bar_chars()
        if hasattr(sys.stdout, "reconfigure"):
            sys.stdout.reconfigure(encoding="utf-8")
        try:
            while True:
                try:
                    text = format_heat(client.snapshot(want), heat, chars)
                except NothingPublished:
                    text = "waiting for the firmware to publish ..."
                if not args.no_clear:
                    clear_screen()
                print(text, flush=True)
                time.sleep(args.interval)
        except KeyboardInterrupt:
            return 0
    print(format_table(client.snapshot(want)))
    return 0


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--run", action="store_true", help="talk to the real keyboard (our firmware only)")
    ap.add_argument("--sim", action="store_true", help="use the simulated keyboard instead of hardware")
    ap.add_argument("--live", action="store_true", help="refreshing heat-map instead of a single table")
    ap.add_argument("--csv", metavar="FILE", help="log every new snapshot to FILE")
    ap.add_argument("--seconds", type=float, default=0, help="with --csv: stop after this many seconds")
    ap.add_argument("--tag", type=lambda s: int(s, 0), default=0, help="only this tag (e.g. 2 = raw scan, 3 = sums)")
    ap.add_argument("--interval", type=float, default=None, help="seconds between reads (live 0.1, csv 0.01)")
    ap.add_argument("--no-clear", action="store_true", help="live mode: print frames instead of clearing the screen")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if args.interval is None:
        args.interval = 0.01 if args.csv else 0.1
    try:
        if args.run and not args.sim:
            iface = bk.pick_interface()
            t = ScopeTransport(iface["path"])
            try:
                return run_session(ScopeClient(t), args)
            finally:
                t.close()
        if args.sim:
            return run_session(ScopeClient(SimulatedScopeKeyboard(animate=demo_animator())), args)
        # dry run: show what would be sent, then demonstrate on the simulator
        print("cmd 3 query (first 16 bytes):", bk.build_request(bk.CMD_GET_INFO, 0, 0)[:16].hex())
        for start in (0, 25, 50, 75):
            print("scope request, start %-2d     :" % start, build_request(start)[:16].hex())
        print("Dry run: nothing was sent. --run reads the keyboard (our firmware only); --sim shows the same on a simulator.\n")
        sim = SimulatedScopeKeyboard(animate=demo_animator())
        c = ScopeClient(sim)
        c.verify()
        snap = c.snapshot()
        print("demo on the simulated keyboard:")
        print(format_heat(snap, Heat(), pick_bar_chars()))
        return 0
    except (bk.BackupError, bk.SafetyError, ScopeError) as exc:
        print("ERROR:", exc)
        return 1


if __name__ == "__main__":
    sys.exit(main())
