#!/usr/bin/env python3
"""magger60_stream.py - send light frames to OUR custom Magger60 firmware in real time (vendor command 0xC7, STREAM_PROTOCOL.md).

The keyboard shows the frames on its 61 lights instead of its normal effect for as long as the stream lives. The stream is
one-directional (frames go to the keyboard; it answers only the packets that ask for an acknowledgement) and it ends by itself:
when this tool stops, crashes or the cable is pulled, the keyboard drops back to its normal effect after the session time-out
(500 ms by default). Nothing a frame can contain raises the firmware's brightness limits, and nothing here reaches flash.

Safe by default: without --run the tool talks to a SIMULATED keyboard (virtual time, no hardware) and shows what the lights
would look like. --run needs our firmware with the stream feature (the cmd 3 build string must carry KMFW; the stock firmware is
never sent a 0xC7 packet) and the 'hidapi' package.

    python tools/magger60_stream.py --self-test
    python tools/magger60_stream.py solid --color 00ff00 --duration 5            # simulated keyboard, prints the lights
    python tools/magger60_stream.py dot --fps 20 --duration 6 --run               # a moving dot along the wiring order
    python tools/magger60_stream.py rainbow --run
    python tools/magger60_stream.py test --run                                    # red, green, blue, white, rows, dot
    python tools/magger60_stream.py file clip.kman --run                          # a KMAN clip (tools/badapple_convert.py) or raw gray frames
    python tools/magger60_stream.py probe --run                                   # only check that the firmware speaks 0xC7

Patterns: solid, dot, rows, rainbow, test, file. Common options: --fps, --duration seconds (0 = until Ctrl+C), --brightness 0-255,
--format rgb|gray, --timeout-ms 100-5000 (how long the keyboard waits for the next packet before it ends the stream).
"""
import argparse
import importlib.util
import os
import random
import re
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent


def _load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / (name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


cfg = _load("magger60_config")
bk, info = cfg.bk, cfg.info

# ---------------------------------------------------------------------------------------------------------- the protocol
CMD_STREAM = 0xC7
HDR, REPORT_LEN, MAX_DATA = 0x55, 64, 56
OP_START, OP_FRAME, OP_STOP, OP_KEEPALIVE = 1, 2, 3, 4
FLAG_ACK = 0x01
FMT_RGB, FMT_GRAY = 0, 1
LIGHTS = 61
VERSION = 1
TIMEOUT_DEFAULT, TIMEOUT_MIN, TIMEOUT_MAX = 500, 100, 5000
WINDOW, ACK_EVERY, ACK_MIN_MS, BAD_RUN_MAX = 16, 8, 2, 32
(OK, ERR_UNKNOWN_OP, ERR_BAD_LENGTH, ERR_BAD_ARG, ERR_SESSION, ERR_STALE, ERR_UNAVAILABLE) = range(7)
STATUS_NAMES = ["OK", "UNKNOWN_OP", "BAD_LENGTH", "BAD_ARG", "SESSION", "STALE", "UNAVAILABLE"]
ST_IDLE, ST_OPEN, ST_LIVE = 0, 1, 2
STATE_NAMES = ["IDLE", "OPEN", "LIVE"]
REPLY_BYTES = 22
FRAME_HDR = 6
CHUNK_LIGHTS = {FMT_RGB: (MAX_DATA - FRAME_HDR) // 3, FMT_GRAY: MAX_DATA - FRAME_HDR}      # 16 and 50

# Position of every light (from km_led.c): 1/8 key widths, five rows of 14/14/13/12/8 lights, rows 1 and 3 run right to left.
X8 = [4, 12, 20, 28, 36, 44, 52, 60, 68, 76, 84, 92, 100, 112,
      114, 104, 96, 88, 80, 72, 64, 56, 48, 40, 32, 24, 16, 6,
      7, 18, 26, 34, 42, 50, 58, 66, 74, 82, 90, 98, 111,
      109, 94, 86, 78, 70, 62, 54, 46, 38, 30, 22, 9,
      5, 15, 25, 55, 85, 95, 105, 115]
ROW_START = [0, 14, 28, 41, 53, 61]


class StreamError(Exception):
    pass


def checksum(data_len, seq, data):
    return (data_len + (seq & 0xFF) + (seq >> 8) + sum(data)) & 0xFF


def build_packet(op, session, seq, flags=0, body=b""):
    """A 64 byte 'U' report: command 0xC7, the sequence number in the offset field, op / session / flags / body as data."""
    data = bytes([op, session, flags]) + bytes(body)
    if len(data) > MAX_DATA:
        raise ValueError("data longer than %d bytes" % MAX_DATA)
    if not 0 <= seq <= 0xFFFF:
        raise ValueError("sequence number must be 16 bit")
    p = bytearray(REPORT_LEN)
    p[0], p[1], p[2] = HDR, CMD_STREAM, 0
    p[3], p[4] = checksum(len(data), seq, data), len(data)
    p[5], p[6], p[7] = seq & 0xFF, seq >> 8, 0
    p[8:8 + len(data)] = data
    return bytes(p)


def build_start(session, seq, fmt=FMT_RGB, timeout_ms=0, flags=0):
    return build_packet(OP_START, session, seq, flags, bytes([VERSION, fmt, timeout_ms & 0xFF, timeout_ms >> 8, 0]))


def pack_pixels(pixels, fmt):
    """61 (r, g, b) tuples -> the bytes of a frame in the session format."""
    if len(pixels) != LIGHTS:
        raise ValueError("a frame has %d lights" % LIGHTS)
    out = bytearray()
    for r, g, b in pixels:
        if not (0 <= r <= 255 and 0 <= g <= 255 and 0 <= b <= 255):
            raise ValueError("colour values are 0..255")
        if fmt == FMT_GRAY:
            if not r == g == b:
                raise ValueError("the gray format needs r == g == b")
            out.append(r)
        else:
            out += bytes((r, g, b))
    return bytes(out)


def frame_chunks(pixels, fmt):
    """[(first, count, bytes)] covering the frame in the largest chunks one packet can carry."""
    bpp = 1 if fmt == FMT_GRAY else 3
    raw = pack_pixels(pixels, fmt)
    per = CHUNK_LIGHTS[fmt]
    return [(i, min(per, LIGHTS - i), raw[i * bpp:(i + min(per, LIGHTS - i)) * bpp]) for i in range(0, LIGHTS, per)]


def build_frame_chunk(session, seq, frame_no, first, count, raw, flags=0):
    return build_packet(OP_FRAME, session, seq, flags, bytes([frame_no & 0xFF, first, count]) + raw)


class Reply:
    def __init__(self, raw):
        raw = bytes(raw)
        if len(raw) < 8 or raw[0] != 0xAA:
            raise StreamError("not an OK reply: %s" % raw[:12].hex())
        if raw[1] != CMD_STREAM:
            raise StreamError("reply of command 0x%02X, not 0xC7" % raw[1])
        n = raw[4]
        data = raw[8:8 + n]
        if n > MAX_DATA or (n + raw[5] + raw[6] + raw[7] + sum(data)) & 0xFF != raw[3]:
            raise StreamError("reply checksum does not match")
        if bytes(data) == b"unknw":
            raise StreamError("this firmware does not know command 0xC7 (no stream support)")
        if n != REPLY_BYTES:
            raise StreamError("reply has %d data bytes, expected %d" % (n, REPLY_BYTES))
        self.seq = raw[5] | raw[6] << 8
        self.op, self.status, self.session, self.state = data[0], data[1], data[2], data[3]
        self.last_seq = data[4] | data[5] << 8
        self.ready_frame, self.window = data[6], data[7]
        self.committed = data[8] | data[9] << 8
        self.dropped = data[10] | data[11] << 8
        self.lost = data[12] | data[13] << 8
        self.stale = data[14] | data[15] << 8
        self.rejected = data[16] | data[17] << 8
        self.version, self.fmt_mask = data[18], data[19]
        self.timeout_ms = data[20] | data[21] << 8

    @property
    def status_name(self):
        return STATUS_NAMES[self.status] if self.status < len(STATUS_NAMES) else "0x%02X" % self.status

    def __str__(self):
        return ("%s op %d session %d state %s last_seq %d frame %d | committed %d dropped %d lost %d stale %d rejected %d | timeout %d ms"
                % (self.status_name, self.op, self.session, STATE_NAMES[self.state] if self.state < 3 else self.state, self.last_seq,
                   self.ready_frame, self.committed, self.dropped, self.lost, self.stale, self.rejected, self.timeout_ms))


def guard_outgoing(packet):
    """Last line of defence right before a write, written from the spec: only the cmd 3 version query or a well-formed
    0xC7 packet may leave this tool."""
    if len(packet) != REPORT_LEN:
        raise bk.SafetyError("a report is %d bytes" % REPORT_LEN)
    if packet[1] == bk.CMD_GET_INFO:
        return bk.guard_outgoing(packet)
    if packet[0] != HDR or packet[1] != CMD_STREAM or packet[2] != 0 or packet[7] != 0:
        raise bk.SafetyError("not a plain 0xC7 'U' packet")
    n = packet[4]
    if n < 3 or n > MAX_DATA or any(packet[8 + n:]):
        raise bk.SafetyError("bad data length or padding")
    if packet[3] != checksum(n, packet[5] | packet[6] << 8, packet[8:8 + n]):
        raise bk.SafetyError("bad checksum")
    op, session, flags = packet[8], packet[9], packet[10]
    if op not in (OP_START, OP_FRAME, OP_STOP, OP_KEEPALIVE) or session == 0 or flags & ~FLAG_ACK & 0xFF:
        raise bk.SafetyError("op, session or flags not allowed")
    need = {OP_START: 8, OP_STOP: 3, OP_KEEPALIVE: 3}
    if op in need and n != need[op]:
        raise bk.SafetyError("wrong length for op %d" % op)
    if op == OP_FRAME and (n < FRAME_HDR or n > MAX_DATA):
        raise bk.SafetyError("wrong length for a frame chunk")
    return packet


# ----------------------------------------------------------------------------------------------------------- the keyboard model
class SimKeyboard:
    """An independent model of km_stream.c written from STREAM_PROTOCOL.md (not a port): per-light lists, a dict of counters.
    Time is virtual: call tick(now_ms). write() handles one report and queues the reply, if any; read() pops one."""

    def __init__(self, enabled=True, build="Jan  1 2026,00:00:00KMFW-SIM", stream=True):
        self.enabled, self.build, self.has_stream = enabled, build, stream
        self.now = 0
        self.out = []
        self.last_rx = 0
        self.seq = 0                         # newest accepted sequence number; kept when a session ends
        self.reset_session()
        self.timeout = TIMEOUT_DEFAULT
        self.count = dict(committed=0, dropped=0, lost=0, stale=0, rejected=0)
        self.serial = 0
        self.ready = None
        self.ready_frame = 0
        self.last_reply = None

    def reset_session(self):
        self.state, self.session, self.fmt = ST_IDLE, 0, FMT_RGB
        self.have = [None] * LIGHTS          # the frame being assembled
        self.cur = None                      # (frame number, done flag)
        self.bad_run = 0
        self.last_rx = self.now

    def tick(self, now_ms):
        self.now = now_ms
        if self.state != ST_IDLE and ((now_ms - self.last_rx) & 0xFFFFFFFF) >= self.timeout:
            self.reset_session()

    def set_enabled(self, enabled):
        self.enabled = enabled
        if not enabled:
            self.reset_session()

    def visible(self):
        return list(self.ready) if self.state == ST_LIVE else None

    def read(self):
        return self.out.pop(0) if self.out else None

    def _bump(self, key, n=1):
        self.count[key] = min(0xFFFF, self.count[key] + n)

    def _reply(self, packet, status, limited):
        if limited:
            if self.last_reply is not None and self.now - self.last_reply < ACK_MIN_MS:
                return
            self.last_reply = self.now
        d = bytearray(REPLY_BYTES)
        d[0] = packet[8] if packet[4] else 0
        d[1] = status
        d[2] = self.session if self.state != ST_IDLE else 0
        d[3] = self.state
        d[4:6] = self.seq.to_bytes(2, "little")
        d[6] = self.ready_frame
        d[7] = WINDOW
        for i, key in enumerate(("committed", "dropped", "lost", "stale", "rejected")):
            d[8 + 2 * i:10 + 2 * i] = self.count[key].to_bytes(2, "little")
        d[18], d[19] = VERSION, 3
        d[20:22] = self.timeout.to_bytes(2, "little")
        off = packet[5] | packet[6] << 8
        r = bytearray(REPORT_LEN)
        r[0], r[1] = 0xAA, CMD_STREAM
        r[3] = (REPLY_BYTES + (off & 0xFF) + (off >> 8) + sum(d)) & 0xFF
        r[4] = REPLY_BYTES
        r[5], r[6] = off & 0xFF, off >> 8
        r[8:8 + REPLY_BYTES] = d
        self.out.append(bytes(r))

    def _reject(self, packet, status, control, answer=True):
        self._bump("rejected")
        self.bad_run = min(255, self.bad_run + 1)
        if self.state != ST_IDLE and self.bad_run >= BAD_RUN_MAX:
            self.reset_session()
        if answer:
            self._reply(packet, status, not control)

    def write(self, packet):
        packet = bytes(packet)
        if len(packet) != REPORT_LEN or packet[0] != HDR:
            return                                                  # raw / garbage reports are not answered
        n = packet[4]
        if n > MAX_DATA:
            return
        if (n + packet[5] + packet[6] + packet[7] + sum(packet[8:8 + n])) & 0xFF != packet[3]:
            r = bytearray(REPORT_LEN)                               # the dispatcher's own answer to a bad checksum
            r[0], r[1], r[4] = 0xAB, packet[1], 4
            r[8:12] = b"cscr"
            r[3] = (4 + sum(b"cscr")) & 0xFF
            self.out.append(bytes(r))
            return
        if packet[1] == bk.CMD_GET_INFO:
            body = bytes([0x04, 0x01]) + self.build.encode()
            r = bytearray(REPORT_LEN)
            r[0], r[1], r[3], r[4] = 0xAA, 3, (len(body) + sum(body)) & 0xFF, len(body)
            r[8:8 + len(body)] = body
            self.out.append(bytes(r))
            return
        if packet[1] != CMD_STREAM or not self.has_stream:
            r = bytearray(REPORT_LEN)
            r[0], r[1], r[4], r[8:13] = 0xAA, packet[1], 5, b"unknw"
            r[3] = (5 + packet[5] + packet[6] + packet[7] + sum(b"unknw")) & 0xFF
            r[5:8] = packet[5:8]
            self.out.append(bytes(r))
            return
        self._stream(packet, n)

    # ---- command 0xC7
    def _stream(self, packet, n):
        if n < 3:
            self._bump("rejected")
            return
        op, session, flags = packet[8], packet[9], packet[10]
        offset = packet[5] | packet[6] << 8 | packet[7] << 16
        xor = packet[2]
        control = op != OP_FRAME
        answer = control or bool(flags & FLAG_ACK)
        if op not in (OP_START, OP_FRAME, OP_STOP, OP_KEEPALIVE):
            return self._reject(packet, ERR_UNKNOWN_OP, True)
        if xor != 0 or offset > 0xFFFF or flags & ~FLAG_ACK & 0xFF:
            return self._reject(packet, ERR_BAD_ARG, control, answer)
        body = packet[8:8 + n]
        if op == OP_START:
            return self._start(packet, body, offset, session)
        if op == OP_FRAME:
            return self._frame(packet, body, offset, session, answer)
        if self.state == ST_IDLE or session != self.session:
            return self._reject(packet, ERR_SESSION, True)
        if n != 3:
            return self._reject(packet, ERR_BAD_LENGTH, True)
        if op == OP_STOP:
            self.reset_session()
            return self._reply(packet, OK, False)
        # keepalive
        fresh = self._fresh(offset)
        if fresh is None:
            self._bump("stale")
            return self._reply(packet, ERR_STALE, False)
        self._accept(offset, fresh)
        self._reply(packet, OK, False)

    def _start(self, packet, body, offset, session):
        if len(body) != 8:
            return self._reject(packet, ERR_BAD_LENGTH, True)
        version, fmt, timeout, reserved = body[3], body[4], body[5] | body[6] << 8, body[7]
        if session == 0 or version != VERSION or fmt > FMT_GRAY or reserved != 0 or \
                (timeout != 0 and not TIMEOUT_MIN <= timeout <= TIMEOUT_MAX):
            return self._reject(packet, ERR_BAD_ARG, True)
        if not self.enabled:
            return self._reply(packet, ERR_UNAVAILABLE, False)
        self.count = dict.fromkeys(self.count, 0)
        self.reset_session()
        self.session, self.fmt, self.state = session, fmt, ST_OPEN
        self.timeout = timeout or TIMEOUT_DEFAULT
        self.seq = offset
        self.last_rx = self.now
        self._reply(packet, OK, False)

    def _fresh(self, seq):
        diff = (seq - self.seq) & 0xFFFF
        if diff == 0 or diff >= 0x8000:
            return None
        return diff - 1

    def _accept(self, seq, gap):
        self._bump("lost", gap)
        self.seq = seq
        self.bad_run = 0
        self.last_rx = self.now

    def _frame(self, packet, body, offset, session, answer):
        if len(body) < FRAME_HDR:
            return self._reject(packet, ERR_BAD_LENGTH, False, answer)
        if self.state == ST_IDLE or session != self.session:
            return self._reject(packet, ERR_SESSION, False, answer)
        frame, first, count = body[3], body[4], body[5]
        bpp = 1 if self.fmt == FMT_GRAY else 3
        if count == 0 or first >= LIGHTS or first + count > LIGHTS:
            return self._reject(packet, ERR_BAD_ARG, False, answer)
        if len(body) != FRAME_HDR + count * bpp:
            return self._reject(packet, ERR_BAD_LENGTH, False, answer)
        fresh = self._fresh(offset)
        if fresh is None:
            self._bump("stale")
            return self._reply(packet, ERR_STALE, True) if answer else None
        self._accept(offset, fresh)
        newer = self.cur is None or (0 < (frame - self.cur[0]) & 0xFF < 0x80)
        if newer:
            if self.cur is not None and not self.cur[1] and any(p is not None for p in self.have):
                self._bump("dropped")
            self.cur = [frame, False]
            self.have = [None] * LIGHTS
        elif frame != self.cur[0] or self.cur[1]:
            self._bump("stale")
            return self._reply(packet, ERR_STALE, True) if answer else None
        px = body[FRAME_HDR:]
        for i in range(count):
            self.have[first + i] = (px[i], px[i], px[i]) if bpp == 1 else tuple(px[3 * i:3 * i + 3])
        if all(p is not None for p in self.have):
            self.ready, self.ready_frame, self.cur[1] = list(self.have), self.cur[0], True
            self.serial += 1
            self.count["committed"] = (self.count["committed"] + 1) & 0xFFFF
            self.state = ST_LIVE
        if answer:
            self._reply(packet, OK, True)


# ----------------------------------------------------------------------------------------------------------- links and clocks
class VirtualClock:
    """Virtual time for the simulated keyboard: sleeping moves the time and lets the keyboard time out."""

    def __init__(self, kb=None):
        self.t = 0.0
        self.kb = kb

    def __call__(self):
        return self.t

    def sleep(self, seconds):
        self.t += max(0.0, seconds)
        if self.kb:
            self.kb.tick(int(self.t * 1000))


class SimLink:
    """The simulated keyboard behind the same write()/read() as the real device. loss/dup/swap are probabilities for the host to
    keyboard direction (deterministic with `seed`); `swap` delays a packet behind the next one."""

    def __init__(self, kb, clock, loss=0.0, dup=0.0, swap=0.0, reply_loss=0.0, seed=1):
        self.kb, self.clock = kb, clock
        self.loss, self.dup, self.swap, self.reply_loss = loss, dup, swap, reply_loss
        self.rng = random.Random(seed)
        self.held = None
        self.sent = 0

    def write(self, packet):
        guard_outgoing(packet)
        self.sent += 1
        self.kb.tick(int(self.clock() * 1000))
        r = self.rng.random()
        if r < self.loss:
            return
        batch = [packet]
        if self.rng.random() < self.dup:
            batch.append(packet)
        if self.held is not None:
            batch.append(self.held)
            self.held = None
        elif self.rng.random() < self.swap:
            self.held, batch = batch[0], batch[1:]
        for p in batch:
            self.kb.write(p)

    def read(self, timeout_ms=0):
        r = self.kb.read()
        if r is None and timeout_ms:
            self.clock.sleep(timeout_ms / 1000.0)
            r = self.kb.read()
        if r is not None and self.rng.random() < self.reply_loss:
            return self.read(0)
        return r


class HidLink:
    """The real keyboard through hidapi: report ID 0 + 64 bytes out, 64 bytes in."""

    def __init__(self, path):
        self._h = bk.HidapiTransport(path, timeout_ms=200)

    def write(self, packet):
        guard_outgoing(packet)
        self._h._dev.write(b"\x00" + bytes(packet))

    def read(self, timeout_ms=0):
        data = self._h._dev.read(REPORT_LEN, max(1, int(timeout_ms)))   # 0 would be a BLOCKING read in the hid module
        return bytes(data[:REPORT_LEN]) if data and len(data) >= REPORT_LEN else None

    def close(self):
        self._h.close()


# ----------------------------------------------------------------------------------------------------------------- the client
def seq_newer(a, b):
    """True if sequence number a is ahead of b (16 bit, wrapping)."""
    d = (a - b) & 0xFFFF
    return 0 < d < 0x8000


class StreamClient:
    """One session: START, frames with flow control, keep-alives, STOP. Time and sleeping are injected (virtual in the tests).

    Flow control: every ACK_EVERY-th packet carries ACK_REQ; the keyboard's reply names the newest sequence number it accepted.
    At most `window` packets may be unanswered; a frame that does not fit is skipped, never queued, so a slow or silent keyboard
    never makes this tool pile up or block."""

    def __init__(self, link, clock=time.monotonic, sleep=time.sleep, fmt=FMT_RGB, timeout_ms=0, session=None, seq0=None,
                 ack_every=ACK_EVERY, log=None):
        self.link, self.clock, self.sleep = link, clock, sleep
        self.fmt, self.timeout_ms = fmt, timeout_ms
        self.session = session if session else (random.SystemRandom().randrange(1, 256))
        self.seq = (random.SystemRandom().randrange(0, 0x10000) if seq0 is None else seq0) & 0xFFFF
        self.acked = self.seq
        self.window = WINDOW
        self.ack_every = ack_every
        self.since_ack_req = 0
        self.frame_no = 0
        self.running = False
        self.need_restart = False
        self.last = None
        self.log = log or (lambda *_: None)
        self.stats = dict(frames=0, skipped=0, packets=0, acks=0, restarts=0, keepalives=0, errors=0)

    # -- plumbing
    def _next_seq(self):
        self.seq = (self.seq + 1) & 0xFFFF
        return self.seq

    def _write(self, packet):
        self.link.write(packet)
        self.stats["packets"] += 1

    def _on_reply(self, rep):
        self.last = rep
        self.stats["acks"] += 1
        if rep.session == self.session and seq_newer(rep.last_seq, self.acked):
            self.acked = rep.last_seq
        if rep.window:
            self.window = rep.window
        if self.running and (rep.state == ST_IDLE or (rep.session != self.session and rep.state != ST_IDLE)):
            self.need_restart = True

    def poll(self, wait_ms=0):
        """Take the replies that are there (waiting at most wait_ms for the first one). Returns how many."""
        n = 0
        while True:
            raw = self.link.read(wait_ms if n == 0 else 0)
            if raw is None:
                return n
            try:
                rep = Reply(raw)
            except StreamError:
                self.stats["errors"] += 1
                continue
            self._on_reply(rep)
            n += 1

    def _request(self, packet, op, tries=3, wait_ms=100):
        """Send a control packet and wait for its reply (same op and sequence number); resend the same packet when it stays silent."""
        for _ in range(tries):
            self._write(packet)
            for _ in range(8):
                raw = self.link.read(wait_ms)
                if raw is None:
                    break
                try:
                    rep = Reply(raw)
                except StreamError:
                    continue
                self._on_reply(rep)
                if rep.op == op and rep.seq == (packet[5] | packet[6] << 8):
                    return rep
        raise StreamError("no answer to op %d" % op)

    # -- session
    def start(self):
        self.acked = self.seq                                  # START carries the base sequence number
        rep = self._request(build_start(self.session, self.seq, self.fmt, self.timeout_ms), OP_START)
        if rep.status != OK:
            raise StreamError("START refused: %s" % rep.status_name)
        self.running, self.need_restart = True, False
        self.since_ack_req = 0
        self.log("stream started: session %d, time-out %d ms, window %d" % (self.session, rep.timeout_ms, self.window))
        return rep

    def restart(self):
        self.stats["restarts"] += 1
        self.log("the keyboard ended the stream (silence or another session): starting again")
        self.seq = (self.seq + 1) & 0xFFFF
        return self.start()

    def stop(self):
        """Close the session. The keyboard would drop the stream by itself after the time-out; this is the polite way."""
        if not self.running:
            return None
        self.running = False
        try:
            return self._request(build_packet(OP_STOP, self.session, self._next_seq()), OP_STOP, tries=2, wait_ms=100)
        except StreamError:
            return None

    def keepalive(self, want_reply=True):
        self.stats["keepalives"] += 1
        pkt = build_packet(OP_KEEPALIVE, self.session, self._next_seq())
        if want_reply:
            return self._request(pkt, OP_KEEPALIVE)
        self._write(pkt)
        return None

    def unanswered(self):
        return (self.seq - self.acked) & 0xFFFF

    def send_frame(self, pixels):
        """Send one frame. Returns False (frame skipped) when the keyboard has not caught up with the window."""
        self.poll(0)
        if self.need_restart:
            self.restart()
        chunks = frame_chunks(pixels, self.fmt)
        if self.unanswered() + len(chunks) > self.window:
            self.poll(20)                                       # give the acknowledgements a moment
            if self.unanswered() + len(chunks) > self.window:
                self.stats["skipped"] += 1
                if self.unanswered() >= self.window:            # silent keyboard: ask it, with a control packet that is always answered
                    try:
                        self.keepalive()
                    except StreamError:
                        pass
                return False
        self.frame_no = (self.frame_no + 1) & 0xFF
        for i, (first, count, raw) in enumerate(chunks):
            self.since_ack_req += 1
            ack = self.since_ack_req >= self.ack_every
            if ack:
                self.since_ack_req = 0
            self._write(build_frame_chunk(self.session, self._next_seq(), self.frame_no, first, count, raw, FLAG_ACK if ack else 0))
        self.stats["frames"] += 1
        return True


def verify_firmware(link):
    """cmd 3 first: only our firmware (KMFW) gets a 0xC7 packet. Then a KEEPALIVE without a session shows that 0xC7 is understood."""
    link.write(bk.build_request(bk.CMD_GET_INFO, 0, 0))
    raw = link.read(500)
    if raw is None:
        raise StreamError("no answer to the version query")
    build = info.decode(raw)["build"]
    if not cfg.is_kmfw_build(build):
        raise StreamError("this keyboard runs '%s', not our firmware (the build string must carry KMFW); no stream packet was sent" % build)
    link.write(build_packet(OP_KEEPALIVE, 1, 0))
    raw = link.read(500)
    if raw is None:
        raise StreamError("no answer to the stream probe")
    rep = Reply(raw)                                                # raises "no stream support" for an "unknw" answer
    if rep.version != VERSION:
        raise StreamError("stream protocol version %d is not supported by this tool (it speaks %d)" % (rep.version, VERSION))
    return build, rep


# ---------------------------------------------------------------------------------------------------------------------- patterns
def scale(c, brightness):
    return tuple((v * brightness + 127) // 255 for v in c)


def hsv(h, s=255, v=255):
    """h in 0..1535 (six sectors of 256); integer arithmetic only."""
    h %= 1536
    sector, f = divmod(h, 256)
    p, q, t = v * (255 - s) // 255, v * (255 - s * f // 255) // 255, v * (255 - s * (255 - f) // 255) // 255
    return [(v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q)][sector]


BLACK = (0, 0, 0)
BASIC = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)]
ROW_COLORS = [(255, 0, 0), (255, 160, 0), (0, 255, 0), (0, 160, 255), (160, 0, 255)]


def row_of(i):
    r = 0
    while r < 4 and i >= ROW_START[r + 1]:
        r += 1
    return r


def pattern_solid(color):
    return lambda t: [color] * LIGHTS


def pattern_dot(color, step_s=0.1):
    return lambda t: [color if i == int(t / step_s) % LIGHTS else BLACK for i in range(LIGHTS)]


def pattern_rows():
    return lambda t: [ROW_COLORS[row_of(i)] for i in range(LIGHTS)]


def pattern_rainbow(speed=300):
    """A rainbow that drifts to the right: hue from the light's horizontal position, like the firmware's own rainbow."""
    return lambda t: [hsv(int(X8[i] * 12 - t * speed), 255, 255) for i in range(LIGHTS)]


def pattern_test(phase_s=1.5):
    """Red, green, blue, white, the five rows, then a dot running along the wiring order: tells at a glance that every colour channel,
    every row and the light order are right."""
    dot = pattern_dot((255, 255, 255), 0.08)
    rows = pattern_rows()

    def f(t):
        k = int(t / phase_s)
        if k < 4:
            return [BASIC[k]] * LIGHTS
        if k == 4:
            return rows(t)
        return dot(t - 5 * phase_s)
    return f


def load_clip(path, src_fps=15):
    """(frames, fps): frames are lists of 61 gray values. .kman needs tools/badapple_convert.py; anything else is raw gray
    frames of 61 bytes each (--src-fps gives the rate)."""
    data = Path(path).read_bytes()
    if data[:4] == b"KMAN":
        conv = HERE / "badapple_convert.py"
        if not conv.exists():
            raise StreamError("reading .kman needs tools/badapple_convert.py (not in this tree); convert to raw gray frames instead")
        mod = _load("badapple_convert")
        meta, frames = mod.decode(data)
        return [list(f) for f in mod.to_levels(meta, frames)], meta["fps"]
    if not data or len(data) % LIGHTS:
        raise StreamError("a raw clip is a multiple of %d bytes (one gray byte per light and frame)" % LIGHTS)
    return [list(data[i:i + LIGHTS]) for i in range(0, len(data), LIGHTS)], src_fps


def pattern_clip(frames, fps, tint, loop=True):
    def f(t):
        k = int(t * fps)
        if loop:
            k %= len(frames)
        k = min(k, len(frames) - 1)
        return [tuple((g * c + 127) // 255 for c in tint) for g in frames[k]]
    return f


def parse_color(text):
    if not re.fullmatch(r"[0-9a-fA-F]{6}", text or ""):
        raise ValueError("colour must be rrggbb")
    return tuple(int(text[i:i + 2], 16) for i in (0, 2, 4))


# ---------------------------------------------------------------------------------------------------------------------- the run
def show(frame, kb_state=""):
    """The lights as five text rows (ANSI colours), left to right as on the keyboard, 8 columns per key width."""
    rows = [[" "] * 15 for _ in range(5)]
    for i, p in enumerate(frame):
        col = min(14, X8[i] // 8)
        rows[row_of(i)][col] = "\x1b[48;2;%d;%d;%dm  \x1b[0m" % p
    return "\n".join("".join(c if c != " " else "  " for c in r) for r in rows) + ("  " + kb_state if kb_state else "")


def run_pattern(client, make, duration, fps, clock, sleep, log=print, static=False):
    """Send frames at `fps` for `duration` seconds (0 = until interrupted). A static pattern sends one frame and keep-alives."""
    client.start()
    t0 = clock()
    period = 1.0 / fps
    n = 0
    try:
        while True:
            now = clock() - t0
            if duration and now >= duration:
                break
            if static and n > 0:
                client.keepalive(want_reply=True)
                sleep(min(period, client.last.timeout_ms / 1000.0 / 3 if client.last else period))
                n += 1
                continue
            client.send_frame(make(now))
            n += 1
            nxt = t0 + n * period
            lag = nxt - clock()
            if lag > 0:
                sleep(lag)
            elif lag < -period * 5:                              # far behind: re-anchor instead of bursting
                t0 = clock() - n * period
    except KeyboardInterrupt:
        log("interrupted")
    finally:
        client.stop()
    return n


def summary(client):
    s = client.stats
    bits = ["%d frames sent, %d skipped (window full), %d packets, %d replies, %d restarts, %d keep-alives" % (
        s["frames"], s["skipped"], s["packets"], s["acks"], s["restarts"], s["keepalives"])]
    if client.last:
        r = client.last
        bits.append("keyboard counters: committed %d, dropped %d, lost %d, stale %d, rejected %d" % (r.committed, r.dropped, r.lost, r.stale, r.rejected))
    return "\n".join(bits)


# ------------------------------------------------------------------------------------------------------------------ test vectors
VECTOR_HEADER = Path(REPO) / "firmware/tests/golden/stream_vectors.h"


def fnv1a(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def vector_script():
    """A scripted conversation: (name, now_ms, request or None). Replies, state and frame hash are what SimKeyboard produces."""
    rgb = [((i * 4) & 255, (255 - i * 3) & 255, (i * 7 + 9) & 255) for i in range(LIGHTS)]
    rgb2 = [((i * 5 + 40) & 255, (i * 2) & 255, 200) for i in range(LIGHTS)]
    gray = [(i * 4 + 3,) * 3 for i in range(LIGHTS)]
    steps = []
    t = 0

    def add(name, now, packet):
        steps.append((name, now, packet))

    add("keepalive without a session", 0, build_packet(OP_KEEPALIVE, 7, 0))
    add("start rgb, base 65530, 600 ms time-out", 10, build_start(7, 65530, FMT_RGB, 600))
    seq = 65530
    for fno, frame in ((1, rgb), (2, rgb2)):
        for i, (first, count, raw) in enumerate(frame_chunks(frame, FMT_RGB)):
            seq = (seq + 1) & 0xFFFF                                # crosses the 16 bit wrap
            last = i == 3
            add("frame %d chunk %d%s" % (fno, i, " (ack)" if last else ""), 20 + 40 * fno, build_frame_chunk(7, seq, fno, first, count, raw, FLAG_ACK if last else 0))
    chunks3 = frame_chunks(rgb, FMT_RGB)
    seq += 1
    add("frame 3 chunk 0", 120, build_frame_chunk(7, seq & 0xFFFF, 3, *chunks3[0]))
    seq += 2                                                         # chunk 1 lost on the way
    add("frame 3 chunk 2 (a chunk is missing)", 121, build_frame_chunk(7, seq & 0xFFFF, 3, *chunks3[2]))
    add("repeat of the same packet (duplicate)", 122, build_frame_chunk(7, seq & 0xFFFF, 3, *chunks3[2], FLAG_ACK))
    for i, (first, count, raw) in enumerate(frame_chunks(rgb2, FMT_RGB)):
        seq += 1
        add("frame 4 chunk %d" % i, 130 + i, build_frame_chunk(7, seq & 0xFFFF, 4, first, count, raw, FLAG_ACK if i == 3 else 0))
    add("a late chunk of frame 3 with an old sequence number", 140, build_frame_chunk(7, (seq - 3) & 0xFFFF, 3, *chunks3[1], FLAG_ACK))
    add("wrong session", 141, build_frame_chunk(8, (seq + 1) & 0xFFFF, 5, *chunks3[0], FLAG_ACK))
    add("unknown op", 142, build_packet(0x7E, 7, (seq + 1) & 0xFFFF))
    add("keepalive", 150, build_packet(OP_KEEPALIVE, 7, (seq + 1) & 0xFFFF))
    add("still alive just inside the time-out", 749, None)
    add("time-out: 600 ms after the last valid packet", 750, None)
    add("frame after the time-out is refused", 751, build_frame_chunk(7, (seq + 2) & 0xFFFF, 6, *chunks3[0], FLAG_ACK))
    add("start gray, default time-out", 800, build_start(9, 500, FMT_GRAY, 0))
    for i, (first, count, raw) in enumerate(frame_chunks(gray, FMT_GRAY)):
        add("gray frame chunk %d" % i, 810, build_frame_chunk(9, 501 + i, 1, first, count, raw, FLAG_ACK if i == 1 else 0))
    add("start with a bad argument (time-out 50 ms)", 820, build_start(9, 503, FMT_GRAY, 50))
    add("frame too long for its count", 821, build_packet(OP_FRAME, 9, 504, 0, bytes([2, 0, 1, 1, 2, 3, 4, 5])))
    add("stop", 830, build_packet(OP_STOP, 9, 505))
    add("stop again (no session)", 831, build_packet(OP_STOP, 9, 506))
    return steps


def vector_rows():
    kb = SimKeyboard()
    rows = []
    for name, now, packet in vector_script():
        kb.tick(now)
        reply = b""
        if packet is not None:
            kb.write(packet)
            r = kb.read()
            reply = r if r is not None else b""
        frame = kb.visible()
        h = fnv1a(b"".join(bytes(p) for p in frame)) if frame else 0
        rows.append((name, now, packet or b"", reply, kb.state, kb.serial, h))
    return rows


def vector_header_text():
    out = ["/* Generated by tools/magger60_stream.py --gen-vectors from its own model of the keyboard (written from",
           " * STREAM_PROTOCOL.md, not from km_stream.c). tests/test_stream.c replays every request through km_vendor_handle and requires",
           " * the identical reply, state, frame counter and frame hash. Do not edit by hand. */",
           "#ifndef KM_STREAM_VECTORS_H", "#define KM_STREAM_VECTORS_H", "",
           "typedef struct {", "    const char *name;", "    unsigned now_ms;", "    const char *request;     /* hex of the 64 byte report, \"\" = only advance the clock */",
           "    const char *reply;       /* hex of the 64 byte reply, \"\" = no reply */", "    unsigned state;          /* km_stream_state() afterwards */",
           "    unsigned serial;         /* km_stream_serial() afterwards */", "    unsigned frame_hash;     /* FNV-1a 32 of the 183 frame bytes while LIVE, else 0 */",
           "} km_stream_vector_t;", "", "static const km_stream_vector_t k_stream_vectors[] = {"]
    for name, now, req, rep, state, serial, h in vector_rows():
        out.append('    { "%s", %d, "%s", "%s", %d, %d, 0x%08Xu },' % (name, now, req.hex(), rep.hex(), state, serial, h))
    out += ["};", "", "#define KM_STREAM_VECTOR_COUNT (sizeof k_stream_vectors / sizeof k_stream_vectors[0])", "", "#endif", ""]
    return "\n".join(out)


# --------------------------------------------------------------------------------------------------------------------- self test
def self_test():
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)
            print("FAIL:", what)

    def raises(fn, exc, what):
        try:
            fn()
        except exc:
            return
        except Exception as e:                                       # noqa: BLE001
            check(False, "%s (wrong exception %r)" % (what, e))
            return
        check(False, what + " (no exception)")

    # --- packet layer, written by hand from the spec
    p = build_packet(OP_KEEPALIVE, 7, 0x0102)
    check(p[:8].hex() == "55c700" + "%02x" % ((3 + 0x02 + 0x01 + 4 + 7) & 0xFF) + "03" + "0201" + "00" and p[8:11] == bytes([4, 7, 0]) and not any(p[11:]), "keep-alive packet bytes")
    check(build_start(7, 65530, FMT_GRAY, 600)[8:16] == bytes([1, 7, 0, 1, 1, 0x58, 0x02, 0]), "start body")
    check(len(frame_chunks([BLACK] * LIGHTS, FMT_RGB)) == 4 and [c[1] for c in frame_chunks([BLACK] * LIGHTS, FMT_RGB)] == [16, 16, 16, 13], "rgb chunking 16/16/16/13")
    check([c[1] for c in frame_chunks([BLACK] * LIGHTS, FMT_GRAY)] == [50, 11], "gray chunking 50/11")
    check(max(len(build_frame_chunk(1, 1, 1, f, c, r)) for f, c, r in frame_chunks([(1, 2, 3)] * LIGHTS, FMT_RGB)) == 64, "reports are 64 bytes")
    raises(lambda: pack_pixels([(1, 2, 3)] * LIGHTS, FMT_GRAY), ValueError, "gray format refuses coloured pixels")
    raises(lambda: pack_pixels([(256, 0, 0)] * LIGHTS, FMT_RGB), ValueError, "colour value 256")
    raises(lambda: pack_pixels([(0, 0, 0)] * 60, FMT_RGB), ValueError, "60 lights")
    raises(lambda: build_packet(OP_FRAME, 1, 0x10000), ValueError, "sequence number 17 bit")
    check(len(X8) == LIGHTS and ROW_START[-1] == LIGHTS and [row_of(i) for i in (0, 13, 14, 27, 28, 40, 41, 52, 53, 60)] == [0, 0, 1, 1, 2, 2, 3, 3, 4, 4], "light layout tables")
    km_led = REPO / "firmware/core/src/km_led.c"
    if km_led.exists():
        m = re.search(r"k_x8\[KM_LED_COUNT\]\s*=\s*\{([^}]*)\}", km_led.read_text())
        check(m is not None and [int(v) for v in re.findall(r"\d+", m.group(1))] == X8, "X8 equals k_x8 in km_led.c")
    check(CHUNK_LIGHTS == {FMT_RGB: 16, FMT_GRAY: 50}, "chunk sizes")

    # --- guard: only well-formed 0xC7 packets and the cmd 3 query may leave
    guard_outgoing(build_packet(OP_KEEPALIVE, 7, 1))
    for bad, what in ((bk.build_request(0xE0, 0, 8)[:1] + bytes([0xE1]) + bytes(62), "flash write command"),
                      (bytes([0x5F, CMD_STREAM]) + bytes(62), "raw header"),
                      (build_packet(OP_KEEPALIVE, 0, 1), "session 0"),
                      (build_packet(0x20, 7, 1), "unknown op"),
                      (build_packet(OP_FRAME, 7, 1, 0x02, bytes([1, 0, 1, 0, 0, 0])), "reserved flag"),
                      (build_packet(OP_STOP, 7, 1, 0, b"\x01"), "STOP with extra data"),
                      (bytes(63), "short report")):
        raises(lambda b=bad: guard_outgoing(b), bk.SafetyError, "guard refuses " + what)
    tampered = bytearray(build_packet(OP_KEEPALIVE, 7, 1))
    tampered[3] ^= 1
    raises(lambda: guard_outgoing(bytes(tampered)), bk.SafetyError, "guard refuses a bad checksum")

    # --- the keyboard model: the behaviours STREAM_PROTOCOL.md promises
    def kb_run(packets, kb=None, step=5):
        """Feed (time, packet) pairs; times are multiplied by `step` ms so that acknowledgements are not rate limited."""
        kb = kb or SimKeyboard()
        replies = []
        for now, pkt in packets:
            kb.tick(now * step)
            kb.write(pkt)
            r = kb.read()
            replies.append(Reply(r) if r is not None and r[0] == 0xAA and r[1] == CMD_STREAM else r)
        return kb, replies

    rgb = [(i, 255 - i, (i * 3) & 255) for i in range(LIGHTS)]
    pk = [(0, build_start(5, 100, FMT_RGB, 0))]
    for i, (f, c, r) in enumerate(frame_chunks(rgb, FMT_RGB)):
        pk.append((10, build_frame_chunk(5, 101 + i, 1, f, c, r, FLAG_ACK if i == 3 else 0)))
    kb, reps = kb_run(pk, step=1)
    check(reps[0].status == OK and reps[0].state == ST_OPEN and reps[0].timeout_ms == 500, "start answered OPEN with the default time-out")
    check(all(r is None for r in reps[1:4]), "chunks without ACK_REQ get no reply")
    check(reps[4].status == OK and reps[4].state == ST_LIVE and reps[4].committed == 1 and reps[4].last_seq == 104, "ack after the last chunk: LIVE, committed 1")
    check(kb.visible() == rgb, "the complete frame is visible, pixel exact")
    kb.tick(509)
    check(kb.state == ST_LIVE, "alive 499 ms after the last packet")
    kb.tick(510)
    check(kb.state == ST_IDLE and kb.visible() is None, "time-out at exactly 500 ms ends the stream")
    # half a frame never shows
    kb2, _ = kb_run([(0, build_start(5, 0)), (1, build_frame_chunk(5, 1, 1, 0, 16, bytes(48)))])
    check(kb2.visible() is None and kb2.state == ST_OPEN, "an incomplete frame is never shown")
    # a new frame number abandons the incomplete one
    chunks = frame_chunks(rgb, FMT_RGB)
    kb3, reps = kb_run([(0, build_start(5, 0)), (1, build_frame_chunk(5, 1, 1, *chunks[0])), (2, build_frame_chunk(5, 2, 2, *chunks[0])),
                        (3, build_frame_chunk(5, 3, 2, *chunks[1])), (4, build_frame_chunk(5, 4, 2, *chunks[2])), (5, build_frame_chunk(5, 5, 2, *chunks[3], FLAG_ACK))])
    check(reps[-1].dropped == 1 and reps[-1].committed == 1 and kb3.visible() == rgb, "newer frame number drops the incomplete frame")
    # duplicates and late packets are ignored and counted
    pkt = build_frame_chunk(5, 6, 3, *chunks[0])
    kb4, reps = kb_run([(0, build_start(5, 0)), (1, pkt), (2, pkt), (3, build_frame_chunk(5, 1, 3, *chunks[1], FLAG_ACK))])
    check(reps[-1].status == ERR_STALE, "late packet answered STALE")
    check(kb4.count["stale"] == 2 and kb4.visible() is None, "duplicate and late sequence numbers: stale 2, nothing applied")
    # gaps are counted
    kb5, reps = kb_run([(0, build_start(5, 10)), (1, build_frame_chunk(5, 15, 1, *chunks[0], FLAG_ACK))])
    check(reps[-1].lost == 4 and reps[-1].last_seq == 15, "a gap of four packets is counted as lost")
    # the wrap
    kb6, reps = kb_run([(0, build_start(5, 0xFFFF)), (1, build_frame_chunk(5, 0, 1, *chunks[0], FLAG_ACK))])
    check(reps[-1].status == OK and reps[-1].last_seq == 0 and reps[-1].lost == 0, "sequence 0xFFFF -> 0 is in order")
    # validation: nothing malformed changes the state
    kb7, reps = kb_run([(0, build_start(5, 0)),
                        (1, build_frame_chunk(5, 1, 1, 60, 2, bytes(6), FLAG_ACK)),
                        (2, build_frame_chunk(5, 2, 1, 0, 0, b"", FLAG_ACK)),
                        (3, build_frame_chunk(5, 3, 1, 0, 2, bytes(5), FLAG_ACK)),
                        (4, build_frame_chunk(6, 4, 1, 0, 1, bytes(3), FLAG_ACK)),
                        (5, build_packet(OP_FRAME, 5, 5, FLAG_ACK, bytes([1, 0])))])
    check([r.status for r in reps[1:]] == [ERR_BAD_ARG, ERR_BAD_ARG, ERR_BAD_LENGTH, ERR_SESSION, ERR_BAD_LENGTH], "malformed frame chunks are refused with the right status")
    check(kb7.seq == 0 and kb7.count["rejected"] == 5 and kb7.state == ST_OPEN, "refused packets change neither the sequence number nor the state")
    kb8, reps = kb_run([(0, build_packet(OP_FRAME, 5, 1, 0, bytes(6))), (1, build_start(0, 1)), (2, build_start(5, 1, 7)), (3, build_start(5, 1, 0, 99)),
                        (4, build_start(5, 1, 0, 5001)), (5, build_packet(OP_START, 5, 1, 0, bytes([2, 0, 0, 0, 0]))), (6, build_packet(0x33, 5, 1))])
    check([r.status for r in reps[1:]] == [ERR_BAD_ARG] * 5 + [ERR_UNKNOWN_OP] and kb8.state == ST_IDLE, "bad START arguments and an unknown op are refused")
    check(reps[0] is None, "a frame chunk without a session and without ACK_REQ is silently dropped")
    # 32 malformed packets in a row end the session
    kb9 = SimKeyboard()
    kb_run([(0, build_start(5, 0))], kb9, step=1)
    for i in range(BAD_RUN_MAX - 1):
        kb9.write(build_frame_chunk(5, 1 + i, 1, 99, 1, bytes(3)))
    check(kb9.state == ST_OPEN, "31 malformed packets: still open")
    kb9.write(build_frame_chunk(5, 40, 1, 99, 1, bytes(3)))
    check(kb9.state == ST_IDLE, "the 32nd malformed packet ends the session")
    # disabled
    kb10 = SimKeyboard(enabled=False)
    _, reps = kb_run([(0, build_start(5, 0))], kb10, step=1)
    check(reps[0].status == ERR_UNAVAILABLE and kb10.state == ST_IDLE, "disabled lights refuse a stream")
    # ack rate limit: a second ACK_REQ less than 2 ms after a reply is not answered
    kb11, reps = kb_run([(0, build_start(5, 0)), (10, build_frame_chunk(5, 1, 1, *chunks[0], FLAG_ACK)), (11, build_frame_chunk(5, 2, 1, *chunks[1], FLAG_ACK)),
                         (12, build_frame_chunk(5, 3, 1, *chunks[2], FLAG_ACK))], step=1)
    check(reps[1] is not None and reps[2] is None and reps[3] is not None, "ACK_REQ replies are limited to one per 2 ms")
    # gray expands to r = g = b
    gray = [(i * 4,) * 3 for i in range(LIGHTS)]
    kb12, _ = kb_run([(0, build_start(5, 0, FMT_GRAY))] + [(1, build_frame_chunk(5, 1 + i, 1, f, c, r)) for i, (f, c, r) in enumerate(frame_chunks(gray, FMT_GRAY))])
    check(kb12.visible() == gray, "gray frames are shown as r = g = b")

    # --- fuzz: random reports never raise, never leave a half frame visible
    rng = random.Random(7)
    kbf = SimKeyboard()
    kbf.tick(0)
    for i in range(20000):
        pkt = bytearray(rng.randrange(256) for _ in range(REPORT_LEN))
        if rng.random() < 0.8:
            pkt[0], pkt[1] = HDR, CMD_STREAM
            if rng.random() < 0.7:
                pkt[2], pkt[7] = 0, 0
                pkt[4] = rng.randrange(0, 12) if rng.random() < 0.7 else rng.randrange(0, 70)
                pkt[8], pkt[9], pkt[10] = rng.choice([1, 2, 3, 4, 5]), rng.choice([0, 1, 2]), rng.choice([0, 1, 3])
                if pkt[4] <= MAX_DATA:
                    pkt[3] = checksum(pkt[4], pkt[5] | pkt[6] << 8, pkt[8:8 + pkt[4]])
        kbf.tick(i)
        kbf.write(bytes(pkt))
        while kbf.read() is not None:
            pass
        v = kbf.visible()
        check(v is None or (len(v) == LIGHTS and all(isinstance(p, tuple) and len(p) == 3 for p in v)), "fuzz: the visible frame is None or complete")
        if failures:
            break

    # --- the client over the simulated link
    def session(pattern, loss=0.0, dup=0.0, swap=0.0, reply_loss=0.0, fmt=FMT_RGB, duration=2.0, fps=30, static=False, seed=3, **kw):
        kb = SimKeyboard()
        clock = VirtualClock(kb)
        link = SimLink(kb, clock, loss, dup, swap, reply_loss, seed)
        cl = StreamClient(link, clock, clock.sleep, fmt=fmt, session=9, seq0=0xFFF0, **kw)
        run_pattern(cl, pattern, duration, fps, clock, clock.sleep, log=lambda *_: None, static=static)
        return kb, cl, clock

    kb, cl, clock = session(pattern_solid((10, 20, 30)), duration=1.0)
    check(kb.state == ST_IDLE, "after run_pattern the session is closed")
    check(cl.stats["frames"] >= 29 and cl.stats["skipped"] == 0 and kb.count["committed"] >= 29, "30 fps for one second: %d frames sent" % cl.stats["frames"])
    check(cl.stats["acks"] >= cl.stats["packets"] // ACK_EVERY - 2, "one reply about every %d packets" % ACK_EVERY)
    check(cl.stats["packets"] < 130 * 4 / 4 + 40, "bandwidth: about 4 packets per frame")
    # the lights follow the pattern
    kb, cl, clock = session(pattern_dot((255, 0, 0), 0.1), duration=0.35, fps=40)
    check(kb.ready is not None and sum(1 for p in kb.ready if p != BLACK) == 1, "the dot pattern shows exactly one light")
    # a gray session sends half the packets
    kbg, clg, _ = session(pattern_solid((9, 9, 9)), fmt=FMT_GRAY, duration=1.0)
    check(clg.stats["packets"] < cl.stats["packets"] * 4 // 2 + 60 and kbg.count["committed"] >= 29, "gray frames need 2 packets, not 4")
    # a static pattern: one frame, then keep-alives; the lights stay on far longer than the time-out
    kbs, cls, _ = session(pattern_solid((0, 40, 0)), duration=3.0, fps=5, static=True)
    check(cls.stats["frames"] == 0 or cls.stats["keepalives"] > 3, "static pattern keeps the session alive with keep-alives")
    # silence: the host disappears -> the keyboard ends the stream by itself
    kb = SimKeyboard()
    clock = VirtualClock(kb)
    link = SimLink(kb, clock)
    cl = StreamClient(link, clock, clock.sleep, session=4, seq0=5)
    cl.start()
    cl.send_frame([(1, 2, 3)] * LIGHTS)
    check(kb.state == ST_LIVE, "stream live after one frame")
    clock.sleep(0.499)
    check(kb.state == ST_LIVE, "still live after 499 ms of silence")
    clock.sleep(0.002)
    check(kb.state == ST_IDLE and kb.visible() is None, "host silent for 500 ms: back to the normal effect")
    cl.send_frame([(1, 2, 3)] * LIGHTS)                          # the keyboard has forgotten the session; the ACK_REQ reply says so
    cl.poll(5)
    check(cl.need_restart and kb.state == ST_IDLE, "the reply of an ended session makes the client restart")
    cl.send_frame([(7, 8, 9)] * LIGHTS)
    check(cl.stats["restarts"] == 1 and kb.state == ST_LIVE and kb.visible() == [(7, 8, 9)] * LIGHTS, "the client starts a new session and streams on")
    # a stalled keyboard (never answers): the client skips frames, never queues more than the window
    class Mute(SimLink):
        def read(self, timeout_ms=0):
            if timeout_ms:
                self.clock.sleep(timeout_ms / 1000.0)
            return None
    kb = SimKeyboard()
    clock = VirtualClock(kb)
    mute = Mute(kb, clock)
    cl = StreamClient(mute, clock, clock.sleep, session=4, seq0=5)
    cl.running = True
    sent = sum(1 for _ in range(100) if cl.send_frame([(1, 2, 3)] * LIGHTS))
    check(sent <= WINDOW // 4 + 1 and cl.stats["skipped"] >= 90, "no replies: at most window/4 frames sent, the rest skipped (sent %d)" % sent)
    # loss, duplicates and reordering: the keyboard never shows a torn frame, ends in a complete one, counters add up
    pat = pattern_rainbow()
    for seed in range(1, 9):
        kb, cl, _ = session(pat, loss=0.08, dup=0.05, swap=0.05, reply_loss=0.2, duration=2.0, seed=seed)
        check(kb.count["committed"] > 20, "lossy link (seed %d): frames still get through (%d)" % (seed, kb.count["committed"]))
        check(kb.count["lost"] > 0 or kb.count["stale"] > 0 or kb.count["dropped"] > 0, "lossy link (seed %d): loss is noticed" % seed)
    # not our firmware / no stream support
    kb = SimKeyboard(build="Jan  1 2026,00:00:00STOCK-SIM")
    clock = VirtualClock(kb)
    link = SimLink(kb, clock)
    sent_before = link.sent
    raises(lambda: verify_firmware(link), StreamError, "stock firmware is refused")
    check(link.sent == sent_before + 1 and kb.count["rejected"] == 0, "against the stock firmware only the cmd 3 query was sent")
    kb = SimKeyboard(stream=False)
    link = SimLink(kb, VirtualClock(kb))
    raises(lambda: verify_firmware(link), StreamError, "KMFW without stream support is refused")
    kb = SimKeyboard()
    link = SimLink(kb, VirtualClock(kb))
    build, rep = verify_firmware(link)
    check("KMFW" in build and rep.version == VERSION and rep.state == ST_IDLE, "verify_firmware on the model")

    # --- patterns
    check(len(pattern_test()(0)) == LIGHTS and pattern_test()(0) == [(255, 0, 0)] * LIGHTS and pattern_test()(1.6)[0] == (0, 255, 0), "test pattern phases")
    r = pattern_rows()(0)
    check([r[0], r[14], r[28], r[41], r[53]] == ROW_COLORS and r[13] == ROW_COLORS[0] and r[52] == ROW_COLORS[3], "rows pattern follows the five rows")
    d = pattern_dot((1, 2, 3), 0.1)
    check([i for i, p in enumerate(d(0.55)) if p != BLACK] == [5] and [i for i, p in enumerate(d(6.15)) if p != BLACK] == [0], "dot position, wraps after light 60")
    check(all(0 <= c <= 255 for p in pattern_rainbow()(1.234) for c in p), "rainbow values in range")
    check(hsv(0) == (255, 0, 0) and hsv(256) == (255, 255, 0) and hsv(768) == (0, 255, 255) and hsv(1536) == hsv(0), "hsv corners")
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        f = Path(td) / "clip.gray"
        f.write_bytes(bytes(range(61)) + bytes(61))
        frames, fps = load_clip(f, 10)
        check(len(frames) == 2 and fps == 10 and frames[0][60] == 60, "raw gray clip loads")
        f.write_bytes(b"abc")
        raises(lambda: load_clip(f), StreamError, "clip of the wrong length")
        pc = pattern_clip(frames, 10, (255, 128, 0))
        check(pc(0)[60] == (60, 30, 0) and pc(0.1)[60] == (0, 0, 0) and pc(0.2)[60] == (60, 30, 0), "clip pattern: tint and loop")
    conv = HERE / "badapple_convert.py"
    if conv.exists():                                                  # only when P7b's converter is in this tree
        mod = _load("badapple_convert")
        pal_frames = [[(i + f) % 4 for i in range(61)] for f in range(3)]
        data = mod.encode(pal_frames, 2, 15, 30)
        with tempfile.TemporaryDirectory() as td:
            f = Path(td) / "c.kman"
            f.write_bytes(data if isinstance(data, (bytes, bytearray)) else bytes(data[0]))
            try:
                frames, fps = load_clip(f)
                check(fps == 15 and len(frames) == 3 and len(frames[0]) == 61, "a KMAN clip loads through badapple_convert")
            except Exception as e:                                     # noqa: BLE001
                check(False, "KMAN clip loading: %r" % e)

    # --- the golden vectors the C test replays
    rows = vector_rows()
    check(len(rows) > 30 and any(r[3] for r in rows) and any(not r[3] for r in rows if r[2]), "vector script has replies and silent packets")
    check(rows[2][4] == ST_OPEN and any(r[4] == ST_LIVE for r in rows) and rows[-1][4] == ST_IDLE, "vector script visits OPEN, LIVE and IDLE")
    if VECTOR_HEADER.exists():
        same = VECTOR_HEADER.read_text().replace("\r\n", "\n") == vector_header_text()
        check(same, "firmware/tests/golden/stream_vectors.h is up to date (run --gen-vectors)")

    print("%d failure(s)" % len(failures) if failures else "magger60_stream self-test: all checks passed")
    return 1 if failures else 0


# ------------------------------------------------------------------------------------------------------------------------- main
def main(argv=None):
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--gen-vectors", action="store_true", help="write firmware/tests/golden/stream_vectors.h")
    sub = ap.add_subparsers(dest="cmd")
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--run", action="store_true", help="talk to the real keyboard (our firmware with the stream feature only)")
    common.add_argument("--fps", type=float, default=30.0, help="frames per second (default 30; the lights show at most 50)")
    common.add_argument("--duration", type=float, default=5.0, help="seconds, 0 = until Ctrl+C (default 5)")
    common.add_argument("--brightness", type=int, default=255, choices=range(256), metavar="0-255", help="scale every colour (the firmware limits apply on top)")
    common.add_argument("--format", choices=["rgb", "gray"], default="rgb", help="gray sends one byte per light (half the packets; neutral colours only)")
    common.add_argument("--timeout-ms", type=int, default=0, help="stream time-out in the keyboard, 100..5000 (0 = default 500)")
    common.add_argument("--loss", type=float, default=0.0, help="simulated keyboard only: fraction of packets lost on the way")
    p = sub.add_parser("solid", parents=[common], help="one colour on all lights")
    p.add_argument("--color", default="00ff00")
    p = sub.add_parser("dot", parents=[common], help="one light running along the wiring order (0..60)")
    p.add_argument("--color", default="ffffff")
    p.add_argument("--step-ms", type=int, default=100)
    sub.add_parser("rows", parents=[common], help="every row its own colour")
    sub.add_parser("rainbow", parents=[common], help="a drifting rainbow")
    sub.add_parser("test", parents=[common], help="red, green, blue, white, rows, running dot (1.5 s each)")
    p = sub.add_parser("file", parents=[common], help="play a clip: .kman (needs badapple_convert.py) or raw gray frames of 61 bytes")
    p.add_argument("path")
    p.add_argument("--src-fps", type=float, default=15.0, help="rate of a raw clip (a .kman carries its own)")
    p.add_argument("--color", default="ffffff", help="tint of the gray clip")
    p.add_argument("--once", action="store_true", help="do not loop")
    sub.add_parser("probe", parents=[common], help="check that the firmware answers 0xC7, send nothing else")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.gen_vectors:
        VECTOR_HEADER.write_text(vector_header_text(), newline="\n")
        print("wrote", VECTOR_HEADER)
        return 0
    if args.cmd is None:
        ap.print_help()
        return 2
    try:
        fmt = FMT_GRAY if args.format == "gray" else FMT_RGB
        static = False
        if args.cmd == "solid":
            make, static = pattern_solid(scale(parse_color(args.color), args.brightness)), True
        elif args.cmd == "dot":
            make = pattern_dot(scale(parse_color(args.color), args.brightness), args.step_ms / 1000.0)
        elif args.cmd == "rows":
            make, static = pattern_rows(), True
        elif args.cmd == "rainbow":
            make = pattern_rainbow()
        elif args.cmd == "test":
            make = pattern_test()
        elif args.cmd == "file":
            frames, fps = load_clip(args.path, args.src_fps)
            args.fps = max(args.fps, fps)
            make = pattern_clip(frames, fps, parse_color(args.color), not args.once)
            if args.duration == 5.0:
                args.duration = 0 if not args.once else len(frames) / fps
        else:
            make = pattern_solid(BLACK)
        if args.brightness != 255 and args.cmd in ("rows", "rainbow", "test", "file"):
            base = make
            make = lambda t, base=base: [scale(c, args.brightness) for c in base(t)]       # noqa: E731
        if fmt == FMT_GRAY:
            base = make
            make = lambda t, base=base: [(p[0],) * 3 for p in base(t)]                      # noqa: E731
        if args.run:
            link = HidLink(bk.pick_interface()["path"])
            clock, sleep = time.monotonic, time.sleep
        else:
            print("Dry run: this is a demonstration on a SIMULATED keyboard (virtual time); nothing was sent to a real one. --run talks to the real one.\n")
            kb = SimKeyboard()
            clock = VirtualClock(kb)
            link = SimLink(kb, clock, loss=args.loss)
            sleep = clock.sleep
        try:
            build, rep = verify_firmware(link)
            print("firmware:", build)
            if args.cmd == "probe":
                print("0xC7 understood:", rep)
                return 0
            client = StreamClient(link, clock, sleep, fmt=fmt, timeout_ms=args.timeout_ms, log=print)
            n = run_pattern(client, make, args.duration, args.fps, clock, sleep, log=print, static=static)
            print(summary(client))
            if not args.run:
                shown = kb.ready
                print("\nthe lights at the end of the stream:\n" + show(shown) if shown else "")
            return 0
        finally:
            if args.run:
                link.close()
    except (bk.BackupError, bk.SafetyError, cfg.ConfigError, StreamError, ValueError, OSError) as exc:
        print("ERROR:", exc)
        return 1


if __name__ == "__main__":
    sys.exit(main())
