#!/usr/bin/env python3
"""magger60_led.py - read and change the light settings (effect, brightness, speed, colour, idle-off time) of OUR custom Magger60
firmware over its configuration protocol (command 0xC6, ops 0x20 LED_GET / 0x21 LED_SET, CONFIG_PROTOCOL.md).

Only firmware built from this repository with the lights (KM_M5_LED, images km_l41* and km_l51) answers; the stock firmware is
never sent a 0xC6 packet (the cmd 3 build string must carry KMFW, and GET_INFO must list the light feature, bit 6). A change is
live only (RAM, gone at the next reset) unless you add --save, which asks the firmware to keep it in its own flash sector and
restore it at every boot (image km_l51 only; any other build refuses). The firmware writes it some seconds later, when the keyboard
is idle; --wait follows the status until it is written. The values can not raise the firmware's brightness limits. The only
requests this tool can send are the cmd 3 version query, GET_INFO, LED_GET and LED_SET.

Safe by default: without --run the tool talks to a SIMULATED keyboard and prints what it would send.

    python tools/magger60_led.py --self-test
    python tools/magger60_led.py get --run
    python tools/magger60_led.py set --effect rainbow --brightness 200 --speed 90 --run
    python tools/magger60_led.py set --effect static --color ff8000 --run
    python tools/magger60_led.py set --effect static --color 00ff00 --idle 600 --save --wait --run
Effects: off, static, breath, rainbow, reactive, depth, demo.
--idle SECONDS: lights fade out after this long without a key press (0 = never, default 600 = 10 minutes); the next key press brings them back.
"""
import argparse
import importlib.util
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent


def _load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / (name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


cfg = _load("magger60_config")
bk, info = cfg.bk, cfg.info

OP_LED_GET, OP_LED_SET = 0x20, 0x21
FEAT_LED = 0x0040
EFFECTS = ["off", "static", "breath", "rainbow", "reactive", "depth", "demo", "custom", "external", "anim"]
FLAG_SAVE = 0x01
ST_BAD_LENGTH = 2
# status bits of the extended reply
ST_SAVE_PENDING, ST_SAVE_FAILED, ST_LOADED, ST_IDLE_OFF, ST_PERSISTENT = 0x01, 0x02, 0x04, 0x08, 0x10
SAVE_RESULTS = {0: "written and verified", 1: "identical to the stored record, nothing written", 3: "bad argument", 4: "read error",
                5: "erase error", 6: "program error", 7: "verify error"}
# op -> the numbers of argument bytes after op and token that are allowed (LED_GET: none = plain, 1 = extended; LED_SET: 6 = plain, 9 = extended)
ARG_LEN = {cfg.OP_GET_INFO: (0,), OP_LED_GET: (0, 1), OP_LED_SET: (6, 9)}


def build_request(op: int, token: int, args: bytes = b"") -> bytes:
    if op not in ARG_LEN:
        raise bk.SafetyError("op 0x%02X is not allowed in the light tool" % op)
    return guard_request(_raw_request(op, token, args))


def _raw_request(op, token, args):
    data = bytes([op, token]) + bytes(args)
    p = bytearray(bk.REPORT_LEN)
    p[0], p[1] = bk.HDR_NORMAL, cfg.CMD_CONFIG
    p[3], p[4] = bk.checksum(len(data), 0, data), len(data)
    p[8:8 + len(data)] = data
    return bytes(p)


def guard_request(packet: bytes) -> bytes:
    """Check the exact bytes about to be sent, written from the spec."""
    if len(packet) != bk.REPORT_LEN or packet[0] != bk.HDR_NORMAL or packet[1] != cfg.CMD_CONFIG or packet[2] != 0:
        raise bk.SafetyError("not a plain 0xC6 'U' packet")
    if packet[5] or packet[6] or packet[7]:
        raise bk.SafetyError("the offset field must be 0")
    n = packet[4]
    if n < 2 or any(packet[8 + n:]) or packet[3] != bk.checksum(n, 0, packet[8:8 + n]):
        raise bk.SafetyError("bad length, padding or checksum")
    op = packet[8]
    if op not in ARG_LEN or (n - 2) not in ARG_LEN[op]:
        raise bk.SafetyError("op 0x%02X with %d argument bytes is not allowed" % (op, n - 2))
    if op == OP_LED_GET and n - 2 == 1 and packet[10] != 1:
        raise bk.SafetyError("the extended LED_GET argument must be 1")
    if op == OP_LED_SET:
        effect, speed = packet[10], packet[12]
        if effect >= len(EFFECTS) or speed == 0:
            raise bk.SafetyError("effect must be 0..%d and speed 1..255" % (len(EFFECTS) - 1))
        if n - 2 == 9 and packet[10 + 8] & ~FLAG_SAVE & 0xFF:
            raise bk.SafetyError("only the SAVE flag (bit 0) is defined")
    return packet


def guard_outgoing(packet: bytes) -> bytes:
    if len(packet) == bk.REPORT_LEN and packet[1] == bk.CMD_GET_INFO:
        return bk.guard_outgoing(packet)
    return guard_request(packet)


class LedTransport(bk.HidapiTransport):
    def exchange(self, packet: bytes) -> bytes:
        packet = guard_outgoing(packet)
        self._dev.write(b"\x00" + packet)
        for _ in range(8):
            data = self._dev.read(bk.REPORT_LEN, self._timeout)
            if not data or len(data) < bk.REPORT_LEN:
                raise bk.BackupError("no (or short) response from the keyboard")
            resp = bytes(data[:bk.REPORT_LEN])
            if resp[1] == packet[1] and (packet[1] != cfg.CMD_CONFIG or (resp[8], resp[9]) == (packet[8], packet[9])):
                return resp
        raise bk.BackupError("only stale replies came back")


class SimulatedLedKeyboard(cfg.SimulatedConfigKeyboard):
    """The simulated settings keyboard of magger60_config.py plus the two light ops (an independent model of km_config.c)."""

    def __init__(self, persistent=True, extended=True, **kw):
        super().__init__(**kw)
        self.led = bytearray([6, 255, 128, 0, 150, 255])
        self.idle = 600
        self.persistent, self.extended = persistent, extended
        self.pending = 0            # polls left until the simulated background save has finished
        self.saved = None           # (led bytes, idle) in the simulated flash
        self.saves_ok = 0
        self.last_result = 0

    def exchange(self, packet: bytes) -> bytes:
        return self.process(guard_outgoing(packet))

    def _ex(self):
        """The 12 byte extended reply; every poll moves the simulated background save on."""
        status = ST_PERSISTENT if self.persistent else 0
        if self.pending:
            status |= ST_SAVE_PENDING
            self.pending -= 1
            if not self.pending:
                self.saved = (bytes(self.led), self.idle)
                self.saves_ok += 1
        return bytes(self.led) + bytes([self.idle & 0xFF, self.idle >> 8, status, self.last_result, self.saves_ok, 0])

    def _config(self, packet):
        ln = packet[4]
        d = packet[8:8 + ln]
        if ln >= 2 and d[0] in (OP_LED_GET, OP_LED_SET):
            op, token, a = d[0], d[1], d[2:]

            def done(status, payload=b""):
                return self._frame(cfg.CMD_CONFIG, bytes([op, token, status]) + payload)
            if op == OP_LED_GET:
                if len(a) == 1 and self.extended:
                    return done(3) if a[0] != 1 else done(0, self._ex())
                return done(2) if a else done(0, bytes(self.led))
            if len(a) == 9 and self.extended:
                if a[8] & ~FLAG_SAVE & 0xFF:
                    return done(3)
                if a[0] >= len(EFFECTS) or a[2] == 0:
                    return done(4)
                if a[8] & FLAG_SAVE and not self.persistent:
                    return done(12)
                self.led[:] = a[:6]
                self.idle = a[6] | (a[7] << 8)
                if a[8] & FLAG_SAVE:
                    self.pending = 2
                return done(0, self._ex())
            if len(a) != 6:
                return done(2)
            if a[0] >= len(EFFECTS) or a[2] == 0:
                return done(4)
            self.led[:] = a
            return done(0, bytes(self.led))
        out = super()._config(packet)
        if ln >= 2 and d[0] == cfg.OP_GET_INFO and len(d) == 2:            # add the light feature bit to GET_INFO
            data = bytearray(out[8:8 + out[4]])
            data[3 + 7] |= FEAT_LED
            return self._frame(cfg.CMD_CONFIG, bytes(data))
        return out


def parse_color(text: str):
    t = text.lstrip("#")
    if len(t) != 6:
        raise ValueError("colour must be six hex digits, e.g. ff8000")
    return bytes.fromhex(t)


def describe(v: bytes) -> str:
    s = "effect %s (%d), brightness %d, speed %d, colour #%02x%02x%02x" % (
        EFFECTS[v[0]] if v[0] < len(EFFECTS) else "?", v[0], v[1], v[2], v[3], v[4], v[5])
    if len(v) >= 12:
        idle = v[6] | (v[7] << 8)
        s += ", lights fade out after " + ("never" if idle == 0 else "%d s (%.1f min)" % (idle, idle / 60.0))
    return s


def describe_status(v: bytes) -> str:
    """Status line of the extended reply (12 bytes)."""
    st = v[8]
    bits = []
    bits.append("flash storage: " + ("yes" if st & ST_PERSISTENT else "NO (this build keeps nothing)"))
    bits.append("restored from flash at boot: " + ("yes" if st & ST_LOADED else "no (defaults)"))
    bits.append("save pending: " + ("YES" if st & ST_SAVE_PENDING else "no"))
    if v[9] == 0 and v[10] == 0 and v[11] == 0:
        bits.append("last save: none since boot")
    else:
        bits.append("last save: %s%s" % (SAVE_RESULTS.get(v[9], "code %d" % v[9]), " [FAILED]" if st & ST_SAVE_FAILED else ""))
    bits.append("saves ok/failed since boot: %d/%d" % (v[10], v[11]))
    bits.append("faded out by idle: " + ("yes" if st & ST_IDLE_OFF else "no"))
    return "; ".join(bits)


class LedClient(cfg.ConfigClient):
    """The settings client with this tool's request builder and the light feature check."""

    def call(self, op, args=b"", ok=(cfg.ST_OK,)):
        if not self.verified and op != cfg.OP_GET_INFO:
            raise cfg.ConfigError("firmware not verified yet")
        self.token = (self.token + 1) & 0xFF
        packet = build_request(op, self.token, args)
        last = None
        for _ in range(self.retries):
            try:
                rep = cfg.decode_reply(self.t.exchange(packet))
            except bk.BackupError as exc:
                last = exc
                continue
            if rep.op != op or rep.token != self.token:
                continue
            if rep.status not in ok:
                raise cfg.ConfigError("op 0x%02X refused: %s" % (op, rep.name), rep.status) from None
            return rep
        raise cfg.ConfigError("no answer to op 0x%02X: %s" % (op, last))

    def check_led(self):
        self.verify()
        if not self.info.features & FEAT_LED:
            raise cfg.ConfigError("this firmware build has no lights (no light feature in GET_INFO)")

    def get(self) -> bytes:
        """The six light bytes (plain LED_GET)."""
        return bytes(self.call(OP_LED_GET).payload)

    def set(self, v: bytes) -> bytes:
        """Plain LED_SET: live change only. Returns the six bytes now in force."""
        return bytes(self.call(OP_LED_SET, v).payload)

    def get_ex(self):
        """The 12 byte extended reply, or None when the firmware has no extended form (older build: it answers BAD_LENGTH)."""
        try:
            return bytes(self.call(OP_LED_GET, b"\x01").payload)
        except cfg.ConfigError as exc:
            if exc.status == ST_BAD_LENGTH:
                return None
            raise

    def set_ex(self, v: bytes, idle: int, save: bool) -> bytes:
        """Extended LED_SET: six light bytes, idle-off seconds (0 = never), optional SAVE. Returns the 12 byte reply."""
        if not 0 <= idle <= 0xFFFF:
            raise ValueError("idle time must be 0..65535 seconds")
        args = bytes(v) + bytes([idle & 0xFF, idle >> 8, FLAG_SAVE if save else 0])
        return bytes(self.call(OP_LED_SET, args).payload)


def run(client: LedClient, args, sleep=time.sleep) -> int:
    client.check_led()
    print("firmware:", client.build)
    ex = client.get_ex()
    cur = ex if ex is not None else client.get()
    print("now:   ", describe(cur))
    if ex is not None:
        print("status:", describe_status(ex))
    else:
        print("status: this firmware has only the plain light ops (live change, no idle-off time, no saving)")
    if args.cmd == "set":
        v = bytearray(cur[:6])
        if args.effect is not None:
            v[0] = EFFECTS.index(args.effect)
        if args.brightness is not None:
            v[1] = args.brightness
        if args.speed is not None:
            v[2] = args.speed
        if args.color is not None:
            v[3:6] = parse_color(args.color)
        want_ex = args.idle is not None or args.save
        if want_ex and ex is None:
            raise cfg.ConfigError("--idle / --save need a firmware with the extended light ops (image km_l51 or newer km_l41*)")
        if ex is None:
            print("after: ", describe(client.set(bytes(v))))
            return 0
        idle = args.idle if args.idle is not None else ex[6] | (ex[7] << 8)
        rep = client.set_ex(bytes(v), idle, args.save)
        print("after: ", describe(rep))
        print("status:", describe_status(rep))
        if args.save and args.wait:
            for _ in range(60):                       # up to about 30 s: the firmware waits 2-3 s of quiet, then writes a few units
                sleep(0.5)
                rep = client.get_ex()
                if not rep[8] & ST_SAVE_PENDING:
                    break
            print("status:", describe_status(rep))
            if rep[8] & ST_SAVE_PENDING:
                print("NOT written yet (the keyboard may be busy: a key is down?)")
                return 1
            if rep[8] & ST_SAVE_FAILED:
                print("SAVE FAILED (the live values are still in force, the previous stored record is intact)")
                return 1
            print("saved.")
    return 0


def self_test() -> int:
    ok = True

    def check(cond, what):
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + what)
        ok = ok and cond

    # the byte-exact example of CONFIG_PROTOCOL.md section 7 (also checked in firmware/tests/test_config.c)
    p = build_request(OP_LED_SET, 0x7E, bytes([3, 200, 90, 1, 2, 3]))
    check(p[:16].hex() == "55c600d208000000217e03c85a010203", "LED_SET example frame")
    for bad, why in ((lambda: build_request(0x05, 1, b"\0" * 10), "COMMIT is not in the light tool"),
                     (lambda: build_request(OP_LED_SET, 1, bytes([10, 1, 1, 1, 1, 1])), "effect out of range"),
                     (lambda: build_request(OP_LED_SET, 1, bytes([1, 1, 0, 1, 1, 1])), "speed 0"),
                     (lambda: build_request(OP_LED_SET, 1, b"\1\1\1"), "short LED_SET"),
                     (lambda: build_request(OP_LED_GET, 1, b"\0"), "long LED_GET")):
        try:
            bad()
            check(False, "refused: " + why)
        except bk.SafetyError:
            check(True, "refused: " + why)
    try:
        guard_outgoing(bytes([0x5F, 6]) + bytes(62))
        check(False, "raw '_' packets are refused")
    except bk.SafetyError:
        check(True, "raw '_' packets are refused")
    sim = SimulatedLedKeyboard()
    c = LedClient(sim)
    c.check_led()
    check(c.get() == bytes([6, 255, 128, 0, 150, 255]), "simulated LED_GET")
    check(c.set(bytes([3, 200, 90, 1, 2, 3])) == bytes([3, 200, 90, 1, 2, 3]), "simulated LED_SET")
    check(c.set(bytes([6, 1, 1, 1, 1, 1])) == bytes([6, 1, 1, 1, 1, 1]), "simulated LED_SET (demo effect)")
    stock = LedClient(cfg.SimulatedConfigKeyboard(stock=True))
    try:
        stock.check_led()
        check(False, "stock firmware refused")
    except cfg.ConfigError:
        check(True, "stock firmware refused")
    check(sim.flash_writes == 0, "no flash write in the simulator")

    # the extended form: byte-exact frames of CONFIG_PROTOCOL.md section 7 (also checked in firmware/tests/test_config.c)
    p = build_request(OP_LED_SET, 0x7E, bytes([3, 200, 90, 1, 2, 3, 0x58, 0x02, 0x01]))
    check(p[:19].hex() == "55c600300b000000217e03c85a0102035802" + "01", "extended LED_SET example frame")
    p = build_request(OP_LED_GET, 0x7F, b"\x01")
    check(p[:11].hex() == "55c600a303000000207f01", "extended LED_GET example frame")
    for bad, why in ((lambda: build_request(OP_LED_SET, 1, bytes([3, 200, 90, 1, 2, 3, 0, 0, 0x02])), "unknown flag bit"),
                     (lambda: build_request(OP_LED_SET, 1, bytes([10, 200, 90, 1, 2, 3, 0, 0, 0])), "effect out of range (extended)"),
                     (lambda: build_request(OP_LED_SET, 1, bytes([3, 200, 0, 1, 2, 3, 0, 0, 0])), "speed 0 (extended)"),
                     (lambda: build_request(OP_LED_SET, 1, bytes(7)), "7 argument bytes"),
                     (lambda: build_request(OP_LED_SET, 1, bytes(10)), "10 argument bytes"),
                     (lambda: build_request(OP_LED_GET, 1, b"\x02"), "extended LED_GET with another argument")):
        try:
            bad()
            check(False, "refused: " + why)
        except bk.SafetyError:
            check(True, "refused: " + why)
    sim = SimulatedLedKeyboard()
    c = LedClient(sim)
    c.check_led()
    ex = c.get_ex()
    check(ex is not None and len(ex) == 12 and ex[6] | (ex[7] << 8) == 600 and ex[8] == ST_PERSISTENT, "simulated extended LED_GET (idle 600 s, persistent)")
    rep = c.set_ex(bytes([3, 200, 90, 1, 2, 3]), 20, False)
    check(rep[:8] == bytes([3, 200, 90, 1, 2, 3, 20, 0]) and not rep[8] & ST_SAVE_PENDING and sim.saved is None, "live extended set: nothing saved")
    rep = c.set_ex(bytes([1, 120, 50, 0, 255, 0]), 0, True)
    check(rep[8] & ST_SAVE_PENDING, "extended set with SAVE: pending")
    rep = c.get_ex()
    rep = c.get_ex()
    check(not rep[8] & ST_SAVE_PENDING and sim.saved == (bytes([1, 120, 50, 0, 255, 0]), 0) and rep[10] == 1, "the simulated save finished after polling")
    out = []
    import io
    import contextlib

    class A:
        cmd, effect, brightness, speed, color, idle, save, wait = "set", "static", 90, None, "0000ff", 120, True, True
    sim2 = SimulatedLedKeyboard()
    with contextlib.redirect_stdout(io.StringIO()) as buf:
        rc = run(LedClient(sim2), A(), sleep=lambda s: None)
    check(rc == 0 and sim2.saved == (bytes([1, 90, 128, 0, 0, 255]), 120) and "saved." in buf.getvalue(), "set --idle 120 --save --wait on the simulator")
    nop = SimulatedLedKeyboard(persistent=False)
    nc = LedClient(nop)
    nc.check_led()
    try:
        nc.set_ex(bytes([1, 1, 1, 1, 1, 1]), 5, True)
        check(False, "a build without flash storage refuses SAVE")
    except cfg.ConfigError:
        check(nop.saved is None and bytes(nop.led) == bytes([6, 255, 128, 0, 150, 255]), "a build without flash storage refuses SAVE and applies nothing")
    old = SimulatedLedKeyboard(extended=False)
    oc = LedClient(old)
    oc.check_led()
    check(oc.get_ex() is None, "an older firmware (no extended form) is detected")
    A.save, A.idle = False, None
    with contextlib.redirect_stdout(io.StringIO()):
        rc = run(LedClient(old), A(), sleep=lambda s: None)
    check(rc == 0 and bytes(old.led)[:2] == bytes([1, 90]), "plain set still works against an older firmware")
    A.save = True
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            run(LedClient(SimulatedLedKeyboard(extended=False)), A(), sleep=lambda s: None)
        check(False, "--save refused for an older firmware")
    except cfg.ConfigError:
        check(True, "--save refused for an older firmware")
    return 0 if ok else 1


def main(argv=None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    sub = ap.add_subparsers(dest="cmd")
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--run", action="store_true", help="talk to the real keyboard (our firmware with lights only)")
    sub.add_parser("get", parents=[common], help="show the current light settings")
    p = sub.add_parser("set", parents=[common], help="change the light settings (live; with --save also kept in flash)")
    p.add_argument("--effect", choices=EFFECTS)
    p.add_argument("--brightness", type=int, choices=range(256), metavar="0-255")
    p.add_argument("--speed", type=int, choices=range(1, 256), metavar="1-255")
    p.add_argument("--color", help="rrggbb")
    p.add_argument("--idle", type=int, choices=range(65536), metavar="SECONDS",
                   help="lights fade out after this many seconds without a key press (0 = never; default 600)")
    p.add_argument("--save", action="store_true", help="also keep the settings in flash and restore them at every boot (km_l51)")
    p.add_argument("--wait", action="store_true", help="with --save: wait until the firmware has written it (a few seconds)")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.cmd is None:
        ap.print_help()
        return 2
    try:
        if args.run:
            t = LedTransport(bk.pick_interface()["path"])
            try:
                return run(LedClient(t, log=print), args)
            finally:
                t.close()
        print("Dry run: this is a demonstration on a SIMULATED keyboard; nothing was sent to a real one. --run talks to the real one.\n")
        return run(LedClient(SimulatedLedKeyboard()), args)
    except (bk.BackupError, bk.SafetyError, cfg.ConfigError, ValueError) as exc:
        print("ERROR:", exc)
        return 1


if __name__ == "__main__":
    sys.exit(main())
