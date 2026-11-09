#!/usr/bin/env python3
"""magger60_flash.py - flash an image through the Magger60's OFFICIAL bootloader update protocol (M2_PLAN.md M2-A).

This is the only tool in the repo that can write to the keyboard. It speaks the bootloader's update protocol
(BOOTLOADER_ANALYSIS.md): erase (0x81), write (0x80), verify (0x82), end (0x83), goto-app (0x84), 32 bytes per
packet, to the bootloader's USB device 19F5:AF1F.

STATUS: used on the author's keyboard with a stock-firmware round trip, a deliberately dead app and the bring-up
images (see M2_PLAN.md, M3_PLAN.md). The self-test runs against a simulated bootloader that follows
BOOTLOADER_ANALYSIS.md, not against hardware; other keyboards and hardware revisions are untested.

What the bootloader does (so what this tool relies on):
  * data is written sequentially from 0x08008000; the host cannot choose an address;
  * 0x81 erases the first app sector AND the sector with the app-ID (0x08028000), so a flash that
    stops half way leaves the keyboard in update mode (it is recoverable by running this again);
  * the last partial page is only written by the first 0x82, so verification is mandatory;
  * the bootloader itself also erases/writes its status sector 0x08006000-0x08007FFF.

Safety, in layers (all of them checked by --self-test):
  1. Dry run by default; --run needs --confirm FLASH-0x08008000 and a valid --backup directory
     (bootloader + settings backups with matching sha256 from magger60_backup.py).
  2. The image must pass every check of magger60_image.py (app-ID, boot flag, SP, reset vector, size).
  3. Only the bootloader device 19F5:AF1F is opened. A keyboard in normal mode (FB49) is refused.
  4. Every outgoing packet is re-parsed by guard_packet(): allowed commands only, chunk <= 32 bytes,
     zero padding, and the payload must equal the image bytes at that offset. Nothing else can be sent.
  5. Any error response, timeout or mismatch aborts at once and 0x84 (run the app) is never sent.

    python tools/magger60_flash.py --self-test
    python tools/magger60_flash.py --list
    python tools/magger60_flash.py --image images/custom.bin --backup backup2                   # dry run
    python tools/magger60_flash.py --image images/custom.bin --backup backup2 --run --confirm FLASH-0x08008000
"""
import argparse
import hashlib
import importlib.util
import json
import struct
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("magger60_image", HERE / "magger60_image.py")
img = importlib.util.module_from_spec(_spec)
sys.modules["magger60_image"] = img
_spec.loader.exec_module(img)

VID = 0x19F5
BOOT_PID = 0xAF1F
NORMAL_PID = 0xFB49
REPORT_LEN = 64
CHUNK = 32
CONFIRM = "FLASH-0x08008000"

CMD_WRITE, CMD_ERASE, CMD_VERIFY, CMD_END, CMD_DONE = 0x80, 0x81, 0x82, 0x83, 0x84

FLASH_BASE = 0x08000000
BOOT_END = 0x08008000
STATUS_SECTOR = 0x08006000
SECTOR = 0x2000
STATUS_ERASE_START, STATUS_VERIFY_ERR, STATUS_DONE = 0xFABC0001, 0xFABC0002, 0xFABCFFFF


class FlashError(Exception):
    """Refused or aborted; when raised after the erase the keyboard is still in update mode."""


# --------------------------------------------------------------------------- packets

def build_erase(vid: int = VID, pid: int = BOOT_PID) -> bytes:
    p = bytearray(REPORT_LEN)
    p[0], p[1] = CMD_ERASE, 7
    p[2], p[3], p[4], p[5], p[6] = vid & 0xFF, vid >> 8, pid & 0xFF, pid >> 8, 0
    return guard_packet(bytes(p), None)


def build_chunk(cmd: int, offset: int, data: bytes, image: bytes) -> bytes:
    p = bytearray(REPORT_LEN)
    p[0], p[1] = cmd, len(data)
    p[2:6] = struct.pack("<I", offset)
    p[6:6 + len(data)] = data
    return guard_packet(bytes(p), image)


def build_finish(cmd: int) -> bytes:
    p = bytearray(REPORT_LEN)
    p[0], p[1] = cmd, 1
    return guard_packet(bytes(p), None)


def guard_packet(p: bytes, image) -> bytes:
    """Independent check of the exact 64 bytes about to be sent. Raises FlashError, never repairs."""
    if len(p) != REPORT_LEN:
        raise FlashError("packet is not 64 bytes")
    cmd, n = p[0], p[1]
    if cmd == CMD_ERASE:
        if n != 7:
            raise FlashError("erase packet length field must be 7")
        if struct.unpack_from("<HH", p, 2) != (VID, BOOT_PID) or p[6] != 0:
            raise FlashError("erase packet must carry 19F5:AF1F and a zero flag")
        if any(p[7:]):
            raise FlashError("erase packet has unexpected payload bytes")
    elif cmd in (CMD_WRITE, CMD_VERIFY):
        if image is None:
            raise FlashError("data packets need the image to be checked against")
        if not 1 <= n <= CHUNK:
            raise FlashError("chunk length %d is outside 1..%d" % (n, CHUNK))
        off = struct.unpack_from("<I", p, 2)[0]
        if off + n > len(image):
            raise FlashError("chunk 0x%X+%d is beyond the image (%d bytes)" % (off, n, len(image)))
        if bytes(p[6:6 + n]) != image[off:off + n]:
            raise FlashError("chunk payload at 0x%X does not match the image" % off)
        if any(p[6 + n:]):
            raise FlashError("chunk is not zero padded")
    elif cmd in (CMD_END, CMD_DONE):
        if n != 1 or any(p[2:]):
            raise FlashError("0x%02X packet must be '%02X 01 00 ...'" % (cmd, cmd))
    else:
        raise FlashError("command 0x%02X is not part of the update protocol" % cmd)
    return p


# --------------------------------------------------------------------------- transports

class Transport:
    def exchange(self, packet: bytes, expect_reply: bool = True):
        raise NotImplementedError


class HidapiTransport(Transport):
    def __init__(self, path: bytes, timeout_ms: int = 3000):
        try:
            import hid  # type: ignore
        except ImportError as exc:
            raise FlashError("the 'hidapi' package is missing: pip install hidapi") from exc
        self._timeout = timeout_ms
        if hasattr(hid, "device"):
            self._dev = hid.device()
            self._dev.open_path(path)
        else:
            self._dev = hid.Device(path=path)

    def exchange(self, packet, expect_reply=True):
        self._dev.write(b"\x00" + packet)
        if not expect_reply:
            return None
        data = self._dev.read(REPORT_LEN, self._timeout)
        if not data or len(data) < 2:
            raise FlashError("no response from the bootloader")
        return bytes(data)

    def close(self):
        self._dev.close()


def find_bootloader_interfaces():
    try:
        import hid  # type: ignore
    except ImportError as exc:
        raise FlashError("the 'hidapi' package is missing: pip install hidapi") from exc
    return [{"pid": d["product_id"], "path": d["path"], "usage_page": d.get("usage_page"),
             "usage": d.get("usage"), "interface": d.get("interface_number"), "product": d.get("product_string")}
            for d in hid.enumerate(VID, 0)]


def pick_bootloader(ifaces):
    boot = [i for i in ifaces if i["pid"] == BOOT_PID]
    if not boot:
        if any(i["pid"] == NORMAL_PID for i in ifaces):
            raise FlashError("the keyboard is in normal mode (19F5:FB49). Unplug it, hold ESC + left CTRL, plug it "
                             "in again and wait until --list shows PID AF1F.")
        raise FlashError("no 19F5:AF1F bootloader device found")
    vendor = [i for i in boot if i["usage_page"] == 0xFF00]
    if len(vendor) == 1:
        return vendor[0]
    if len(boot) == 1:
        return boot[0]
    raise FlashError("cannot tell which AF1F interface to use; run --list and send me the output:\n  " +
                     "\n  ".join(str(i) for i in boot))


# --------------------------------------------------------------------------- the flash procedure

def reply_ok(reply) -> bool:
    return reply is not None and len(reply) >= 2 and reply[0] == 0 and reply[1] == 0


def flash(image: bytes, tr: Transport, log=print, reboot: bool = True) -> int:
    """Run the vendor update sequence. Returns the number of packets sent. Raises FlashError on any problem."""
    problems = img.check_image(image)
    if problems:
        raise FlashError("image refused: " + "; ".join(problems))
    sent = 0

    def send(pkt, what, reply=True):
        nonlocal sent
        r = tr.exchange(pkt, expect_reply=reply)
        sent += 1
        if reply and not reply_ok(r):
            raise FlashError("bootloader reported an error during %s (reply %s)" % (what, r[:4].hex() if r else None))

    offsets = range(0, len(image), CHUNK)
    log("erase (the keyboard has no valid app from here until the end)")
    send(build_erase(), "erase")
    for phase, cmd, label in (("write", CMD_WRITE, "writing"), ("verify", CMD_VERIFY, "verifying")):
        for k, off in enumerate(offsets):
            send(build_chunk(cmd, off, image[off:off + CHUNK], image), "%s at 0x%X" % (phase, off))
            if k % 1024 == 0:
                log("  %s %d/%d bytes" % (label, off, len(image)))
        log("%s done" % phase)
    send(build_finish(CMD_END), "end")
    if reboot:
        send(build_finish(CMD_DONE), "goto app", reply=False)
        log("sent 'goto app'; the keyboard restarts into the new app (or stays in update mode if it is invalid)")
    return sent


# --------------------------------------------------------------------------- preparation / refusals

def check_backup(backup_dir: Path):
    """The backup directory must hold a plausible bootloader and settings dump with matching sha256."""
    try:
        manifest = json.loads((backup_dir / "manifest.json").read_text(encoding="utf-8"))
        boot = (backup_dir / "bootloader_0x08000000.bin").read_bytes()
        sett = (backup_dir / "settings_0x08028000.bin").read_bytes()
    except (OSError, ValueError) as exc:
        raise FlashError("backup directory %s is not usable (%s): run tools/magger60_backup.py --run first" % (backup_dir, exc))
    if len(boot) != 0x8000 or len(sett) != img.SETTINGS_SIZE:
        raise FlashError("backup files have the wrong size")
    if set(boot) <= {0, 0xFF}:
        raise FlashError("the bootloader backup is empty (all 00 or FF)")
    sp, rst = struct.unpack_from("<II", boot, 0)
    if not (0x20000000 <= sp <= 0x20028000 and rst & 1 and FLASH_BASE <= rst < BOOT_END):
        raise FlashError("the bootloader backup does not start with a plausible vector table")
    for name, data in (("bootloader_0x08000000.bin", boot), ("settings_0x08028000.bin", sett)):
        want = manifest.get("files", {}).get(name, {}).get("sha256")
        if want != hashlib.sha256(data).hexdigest():
            raise FlashError("sha256 of %s does not match manifest.json" % name)
    return boot, sett


def prepare(image_path: Path, backup_dir: Path, confirm, ifaces, allow_settings_diff: bool = False):
    """Everything that must hold before a single byte is sent. Returns (image, interface)."""
    image = Path(image_path).read_bytes()
    _, sett = check_backup(Path(backup_dir))
    problems = img.check_image(image, sett[:img.APP_ID_LEN])
    if problems:
        raise FlashError("image refused: " + "; ".join(problems))
    off = img.SETTINGS_BASE - img.APP_BASE
    if image[off:off + img.SETTINGS_SIZE] != sett and not allow_settings_diff:
        n = sum(1 for a, b in zip(image[off:off + img.SETTINGS_SIZE], sett) if a != b)
        raise FlashError("the image's settings area differs from the backup in %d bytes: rebuild the image with "
                         "magger60_image.py from this backup (or pass --allow-settings-diff on purpose)" % n)
    if confirm != CONFIRM:
        raise FlashError("--confirm %s is required for --run" % CONFIRM)
    return image, pick_bootloader(ifaces)


def plan(image: bytes) -> str:
    n = (len(image) + CHUNK - 1) // CHUNK
    return ("Dry run. With --run this tool would send, to 19F5:AF1F only:\n"
            "  1 x erase (0x81), %d x write (0x80), %d x verify (0x82), end (0x83), goto-app (0x84)\n"
            "  image: %s\n"
            "  the bootloader erases sectors 0x08008000 and 0x08028000 first, then writes the image sequentially;\n"
            "  it also rewrites its own status sector 0x08006000-0x08007FFF. Nothing else is touched.\n"
            "Nothing was sent. Add --run --confirm %s to flash." % (n, n, img.describe(image), CONFIRM))


# --------------------------------------------------------------------------- simulated bootloader

class SimBootloader(Transport):
    """Behaves like the real bootloader's update handler as worked out in BOOTLOADER_ANALYSIS.md."""

    def __init__(self, flash_init: bytes):
        self.flash = bytearray(b"\xFF" * 0x80000)
        self.flash[:len(flash_init)] = flash_init
        self.ops = []               # (kind, addr, length, who)
        self.violations = 0         # programming over non-erased flash
        self.packets = 0
        self.fail_at = None         # raise "unplug" after this many packets
        self.corrupt_verify = False  # make flash differ from what verify expects
        self._reset_session()
        self.done = False

    def _reset_session(self):
        self.wptr = self.vptr = img.APP_BASE
        self.last_off = None
        self.verify_started = False
        self.buf = bytearray()

    # -- flash primitives
    def _erase(self, addr, who):
        a = addr & ~(SECTOR - 1)
        self.flash[a - FLASH_BASE:a - FLASH_BASE + SECTOR] = b"\xFF" * SECTOR
        self.ops.append(("erase", a, SECTOR, who))

    def _program(self, addr, data, who):
        o = addr - FLASH_BASE
        for i, b in enumerate(data):
            cur = self.flash[o + i]
            if cur != 0xFF and b != 0xFF and cur != b:
                self.violations += 1
            self.flash[o + i] = cur & b
        self.ops.append(("program", addr, len(data), who))

    def _status(self, word):
        self._erase(STATUS_SECTOR, "status")
        self._program(STATUS_SECTOR, struct.pack("<II", word, word), "status")

    def _program_page(self, addr, data):
        self._erase(addr, "host")
        self._program(addr, bytes(data), "host")

    # -- protocol
    def exchange(self, packet, expect_reply=True):
        self.packets += 1
        if self.fail_at is not None and self.packets > self.fail_at:
            raise FlashError("simulated unplug")
        cmd, n = packet[0], packet[1]
        off = struct.unpack_from("<I", packet, 2)[0]
        data = packet[6:6 + n]
        err = 0
        if cmd == CMD_ERASE:
            self._reset_session()
            self._erase(img.APP_BASE, "host")
            self._erase(img.SETTINGS_BASE, "host")
            self._status(STATUS_ERASE_START)
        elif cmd == CMD_WRITE:
            if self.last_off != off:
                self.last_off = off
                self.buf += data
                if len(self.buf) >= SECTOR:
                    self._program_page(self.wptr, self.buf[:SECTOR])
                    del self.buf[:SECTOR]
                    self.wptr += SECTOR
        elif cmd == CMD_VERIFY:
            if not self.verify_started:
                self.verify_started = True
                if self.buf:
                    self._program_page(self.wptr, bytes(self.buf) + b"\xFF" * (SECTOR - len(self.buf)))
                    self.buf.clear()
            if self.last_off != off:
                self.last_off = off
                want = bytes(data)
                have = bytes(self.flash[self.vptr - FLASH_BASE:self.vptr - FLASH_BASE + n])
                if self.corrupt_verify and self.vptr == img.APP_BASE + 0x100:
                    have = bytes([have[0] ^ 1]) + have[1:]
                if want != have:
                    self._status(STATUS_VERIFY_ERR)
                    err = 1
                self.vptr += n
        elif cmd == CMD_END:
            self.verify_started = False
            self._status(STATUS_DONE)
        elif cmd == CMD_DONE:
            self.done = True
            return None
        else:
            err = 1
        return bytes([0, err]) + bytes(REPORT_LEN - 2)

    def word(self, addr):
        return struct.unpack_from("<I", self.flash, addr - FLASH_BASE)[0]

    def boot_decision(self, boot_key=False):
        """What the bootloader does on power-up (BOOTLOADER_ANALYSIS.md): update mode, or run the app."""
        if self.word(img.SETTINGS_BASE) != img.APP_ID_MAGIC:
            return "update: app-ID block invalid"
        if self.flash[img.FLAG_ADDR - FLASH_BASE] & 1:
            return "update: flag set by the app"
        if boot_key:
            return "update: rescue keys held"
        sp, rst = self.word(img.APP_BASE), self.word(img.APP_BASE + 4)
        if not (0x20000000 <= sp <= 0x20028000):
            return "update: stack pointer outside SRAM"
        if not (img.APP_BASE <= rst <= 0x08080000):
            return "update: reset vector outside flash"
        return "app"


# --------------------------------------------------------------------------- CLI

def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--list", action="store_true", help="show the 19F5 HID interfaces (sends nothing)")
    ap.add_argument("--image")
    ap.add_argument("--backup", help="directory written by magger60_backup.py --run")
    ap.add_argument("--run", action="store_true")
    ap.add_argument("--confirm", default="")
    ap.add_argument("--no-reboot", action="store_true", help="do not send 0x84; stay in update mode afterwards")
    ap.add_argument("--allow-settings-diff", action="store_true")
    args = ap.parse_args()
    try:
        if args.self_test:
            return self_test()
        if args.list:
            ifaces = find_bootloader_interfaces()
            if not ifaces:
                print("no HID interface with VID 19F5 found")
            for i in ifaces:
                print("PID %04X  %-30s usage %s/%s  if %s  %s" % (
                    i["pid"], i["product"] or "", i["usage_page"], i["usage"], i["interface"],
                    "bootloader" if i["pid"] == BOOT_PID else "normal" if i["pid"] == NORMAL_PID else "other"))
            return 0
        if not args.image:
            ap.error("--image is required")
        if not args.run:
            image = Path(args.image).read_bytes()
            problems = img.check_image(image)
            print(plan(image))
            if problems:
                print("\nWARNING: this image would be REFUSED:\n  - " + "\n  - ".join(problems))
                return 1
            return 0
        if not args.backup:
            ap.error("--backup is required with --run")
        image, iface = prepare(Path(args.image), Path(args.backup), args.confirm,
                               find_bootloader_interfaces(), args.allow_settings_diff)
        tr = HidapiTransport(iface["path"])
        try:
            n = flash(image, tr, reboot=not args.no_reboot)
        finally:
            tr.close()
        print("done: %d packets. Now re-run tools/magger60_backup.py --run --out <new dir> and compare." % n)
        return 0
    except FlashError as exc:
        print("STOPPED:", exc)
        print("If this happened after the erase step the keyboard is still in update mode and the app is "
              "not valid; running this tool again is the way to recover. Do not start another updater in between.")
        return 1


# --------------------------------------------------------------------------- self-test

def _synthetic_flash(image_for_app: bytes, bootloader_code_end: int = 0x5C9C) -> bytes:
    """A factory-like flash: bootloader code, empty status sector, an app + settings."""
    f = bytearray(b"\xFF" * 0x80000)
    f[0:8] = struct.pack("<II", 0x20007A68, 0x0800022D)
    for i in range(8, bootloader_code_end):
        f[i] = (i * 131 + 17) & 0xFF
    f[img.APP_BASE - FLASH_BASE:img.APP_BASE - FLASH_BASE + len(image_for_app)] = image_for_app
    return bytes(f)


def self_test() -> int:
    failures = []

    def check(cond, msg):
        if not cond:
            failures.append(msg)
            print("FAIL:", msg)

    def must_raise(label, fn, needle=None):
        try:
            fn()
        except FlashError as exc:
            if needle and needle not in str(exc):
                check(False, "%s raised %r, expected it to mention %r" % (label, str(exc), needle))
            return
        check(False, "%s was accepted" % label)

    settings = img._fake_settings()
    old_app = img._fake_app(size=0x6000)
    new_app = img._fake_app(reset=0x0800922D, size=0x5123)       # not a multiple of 8 KB on purpose
    old_image = img.build_image(old_app, settings)
    new_image = img.build_image(new_app, settings, tail=bytes(range(0x80)) * 2 + bytes(0x28))
    check(len(new_image) % SECTOR != 0, "test image should end on a partial page")

    # --- packet formats equal the update protocol's hand-computed frames (BOOTLOADER_ANALYSIS.md)
    check(build_erase()[:7].hex() == "8107f5191faf00", "erase packet differs from the documented frame")
    check(build_finish(CMD_END)[:5].hex() == "8301000000", "end packet differs from the documented frame")
    check(build_finish(CMD_DONE)[:5].hex() == "8401000000", "goto-app packet differs from the documented frame")
    c = build_chunk(CMD_WRITE, 0x40, new_image[0x40:0x60], new_image)
    check(c[:6].hex() == "802040000000" and c[6:38] == new_image[0x40:0x60], "write packet differs")
    check(build_chunk(CMD_VERIFY, 0, new_image[:5], new_image)[:2].hex() == "8205", "verify packet differs")

    # --- guard_packet refuses everything that is not part of the update protocol
    for cmd in (0x00, 0x03, 0x06, 0x55, 0x5F, 0xB1, 0xE0, 0xE1, 0xEE, 0x85, 0xFF):
        p = bytearray(REPORT_LEN)
        p[0], p[1] = cmd, 1
        must_raise("command 0x%02X" % cmd, lambda p=p: guard_packet(bytes(p), new_image), "not part")
    must_raise("33-byte chunk", lambda: build_chunk(CMD_WRITE, 0, new_image[:33], new_image), "outside")
    must_raise("empty chunk", lambda: build_chunk(CMD_WRITE, 0, b"", new_image), "outside")
    must_raise("chunk past image end", lambda: build_chunk(CMD_WRITE, len(new_image) - 4, new_image[-4:] + b"xx", new_image),
               "beyond the image")
    must_raise("payload differs from image", lambda: build_chunk(CMD_WRITE, 0, b"\x00" * 32, new_image), "does not match")
    bad = bytearray(build_chunk(CMD_WRITE, 0, new_image[:8], new_image))
    bad[40] = 1
    must_raise("dirty padding", lambda: guard_packet(bytes(bad), new_image), "zero padded")
    bad = bytearray(build_erase())
    bad[4:6] = struct.pack("<H", NORMAL_PID)
    must_raise("erase with the wrong PID", lambda: guard_packet(bytes(bad), None), "19F5:AF1F")
    bad = bytearray(build_erase())
    bad[20] = 1
    must_raise("erase with extra payload", lambda: guard_packet(bytes(bad), None), "payload")
    must_raise("short packet", lambda: guard_packet(b"\x81\x07", None), "64")

    # --- full flash against the simulated bootloader
    sim = SimBootloader(_synthetic_flash(old_image))
    boot_before = bytes(sim.flash[:BOOT_END - FLASH_BASE])
    check(sim.boot_decision() == "app", "the synthetic keyboard should start in the old app")
    sent = flash(new_image, sim, log=lambda *_: None)
    check(sent == 1 + 2 * ((len(new_image) + 31) // 32) + 2, "unexpected packet count %d" % sent)
    check(bytes(sim.flash[0:STATUS_SECTOR - FLASH_BASE]) == boot_before[:STATUS_SECTOR - FLASH_BASE],
          "bootloader code area 0x08000000-0x08005FFF changed")
    check(sim.word(STATUS_SECTOR) == STATUS_DONE and sim.word(STATUS_SECTOR + 4) == STATUS_DONE, "status word is not 'done'")
    check(bytes(sim.flash[img.APP_BASE - FLASH_BASE:img.APP_BASE - FLASH_BASE + len(new_image)]) == new_image,
          "flash does not equal the image (the last partial page must have been written by the verify pass)")
    check(sim.done, "0x84 was not sent")
    check(sim.violations == 0, "the sim saw programming over non-erased flash")
    check(sim.boot_decision() == "app", "the flashed keyboard does not boot the new app")
    check(sim.boot_decision(boot_key=True) == "update: rescue keys held", "boot key should force update mode")
    host_ops = [o for o in sim.ops if o[3] == "host"]
    check(all(o[1] >= img.APP_BASE for o in host_ops), "a host-driven write went below 0x08008000")
    check(all(o[1] == STATUS_SECTOR for o in sim.ops if o[3] == "status"), "the status writes are not confined to 0x08006000")
    check(max(o[1] + o[2] for o in host_ops) <= img.SETTINGS_END + 0x2000, "host writes ran past the end of the image")

    # --- --no-reboot stays in update mode's loop (no 0x84)
    sim2 = SimBootloader(_synthetic_flash(old_image))
    flash(new_image, sim2, log=lambda *_: None, reboot=False)
    check(not sim2.done, "--no-reboot still sent 0x84")

    # --- interrupted flashes. Packet order: erase, N writes, N verifies, end, goto-app.
    #     Before the erase the old app survives. After it the keyboard is in update mode until the sector
    #     with the app-ID has been rewritten; from then on it may boot a half-written image (still
    #     recoverable by flashing again), and once the first verify has flushed the last page it is complete.
    nchunks = (len(new_image) + 31) // 32
    npk = 1 + 2 * nchunks + 2
    id_page_done = 1 + (img.SETTINGS_BASE - img.APP_BASE + SECTOR) // CHUNK      # packets until the ID page is programmed
    partial_but_bootable = 0
    for cut in (0, 1, 2, 50, 2000, id_page_done - 1, id_page_done, nchunks // 2 + 700, nchunks, nchunks + 1,
                nchunks + 2, nchunks + 700, npk - 3, npk - 2, npk - 1):
        s = SimBootloader(_synthetic_flash(old_image))
        s.fail_at = cut
        try:
            flash(new_image, s, log=lambda *_: None)
            check(False, "an interrupted flash (cut=%d) reported success" % cut)
        except FlashError:
            pass
        got = s.boot_decision()
        complete = cut >= nchunks + 2
        have = bytes(s.flash[img.APP_BASE - FLASH_BASE:img.APP_BASE - FLASH_BASE + len(new_image)])
        if cut == 0:
            check(got == "app" and s.word(img.APP_BASE) == 0x20009F50, "cut 0: the old app should be untouched")
        elif complete:
            check(got == "app" and have == new_image, "cut %d: the image should be complete after the first verify" % cut)
        elif cut < id_page_done:
            check(got == "update: app-ID block invalid", "cut %d: expected update mode, got %r" % (cut, got))
        else:
            check(got in ("app", "update: app-ID block invalid"), "cut %d: unexpected decision %r" % (cut, got))
            if got == "app" and have != new_image:
                partial_but_bootable += 1
        check(bytes(s.flash[:STATUS_SECTOR - FLASH_BASE]) == bytes(_synthetic_flash(old_image)[:STATUS_SECTOR - FLASH_BASE]),
              "cut %d: bootloader code changed" % cut)
        check(all(o[1] >= img.APP_BASE for o in s.ops if o[3] == "host"), "cut %d: host write below 0x08008000" % cut)
    check(partial_but_bootable >= 1, "expected at least one cut to leave a bootable half-written image (documented hazard)")

    # --- a verify mismatch aborts, reports it, and never sends 0x84
    s = SimBootloader(_synthetic_flash(old_image))
    s.corrupt_verify = True
    must_raise("verify mismatch", lambda: flash(new_image, s, log=lambda *_: None), "error during verify")
    check(not s.done, "0x84 was sent after a verify error")
    check(s.word(STATUS_SECTOR) == STATUS_VERIFY_ERR, "the sim did not record the verify error")
    check(s.boot_decision() == "update: app-ID block invalid" or s.boot_decision() == "app",
          "unexpected state after a verify error")

    # --- a transport that answers with an error byte, and one that stays silent
    class ErrTransport(Transport):
        def exchange(self, packet, expect_reply=True):
            return bytes([0, 1]) + bytes(62)
    must_raise("error reply", lambda: flash(new_image, ErrTransport(), log=lambda *_: None), "error")

    class Silent(Transport):
        def exchange(self, packet, expect_reply=True):
            return None if expect_reply else None
    must_raise("silent device", lambda: flash(new_image, Silent(), log=lambda *_: None), "error")

    # --- quirk documented in the bootloader: without any 0x82 the last partial page is never written
    s = SimBootloader(_synthetic_flash(old_image))
    s.exchange(build_erase())
    for off in range(0, len(new_image), CHUNK):
        s.exchange(build_chunk(CMD_WRITE, off, new_image[off:off + CHUNK], new_image))
    full_pages = len(new_image) // SECTOR * SECTOR
    check(bytes(s.flash[img.APP_BASE - FLASH_BASE:img.APP_BASE - FLASH_BASE + full_pages]) == new_image[:full_pages],
          "full pages should already be written before verification")
    check(bytes(s.flash[img.APP_BASE - FLASH_BASE + full_pages:img.APP_BASE - FLASH_BASE + len(new_image)]) !=
          new_image[full_pages:], "the partial last page should still be missing before the first verify")

    # --- the bootloader ignores a data packet whose offset equals the previous one (a retransmission is harmless)
    s = SimBootloader(_synthetic_flash(old_image))
    s.exchange(build_erase())
    pkt = build_chunk(CMD_WRITE, 0, new_image[:32], new_image)
    s.exchange(pkt)
    s.exchange(pkt)
    check(len(s.buf) == 32, "the sim accepted a repeated offset twice")

    # --- images the flasher must not send at all
    bad_images = {
        "bad SP": img.build_image(img._fake_app(sp=0x40000000), settings),
        "even reset": img.build_image(img._fake_app(reset=0x0800922C), settings),
        "no app-ID": img.build_image(new_app, img._fake_settings(magic=0x12345678)),
        "erased app-ID": img.build_image(new_app, b"\xFF" * img.SETTINGS_SIZE),
        "boot flag": img.build_image(new_app, img._fake_settings(flag=0x01)),
        "too short": new_image[:0x20000],
    }
    for label, bi in bad_images.items():
        s = SimBootloader(_synthetic_flash(old_image))
        must_raise("image with " + label, lambda bi=bi, s=s: flash(bi, s, log=lambda *_: None), "refused")
        check(s.packets == 0, "image with %s: packets were sent before it was refused" % label)
        check(s.boot_decision() == "app", "image with %s: the old app was damaged" % label)

    # --- run-time refusals: backup, confirmation, interface, settings mismatch
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        boot = bytearray(0x8000)
        boot[0:8] = struct.pack("<II", 0x20007A68, 0x0800022D)
        boot[8:0x5000] = bytes((i * 3 + 1) & 0xFF for i in range(0x5000 - 8))
        bk = td / "bk"
        bk.mkdir()
        (bk / "bootloader_0x08000000.bin").write_bytes(bytes(boot))
        (bk / "settings_0x08028000.bin").write_bytes(settings)
        (bk / "manifest.json").write_text(json.dumps({"files": {
            "bootloader_0x08000000.bin": {"sha256": hashlib.sha256(bytes(boot)).hexdigest()},
            "settings_0x08028000.bin": {"sha256": hashlib.sha256(settings).hexdigest()}}}), encoding="utf-8")
        ipath = td / "i.bin"
        ipath.write_bytes(new_image)
        boot_if = [{"pid": BOOT_PID, "path": b"x", "usage_page": 0xFF00, "usage": 1, "interface": 1, "product": "bl"}]
        normal_if = [{"pid": NORMAL_PID, "path": b"y", "usage_page": 1, "usage": 0, "interface": 1, "product": "kb"}]

        got_image, got_if = prepare(ipath, bk, CONFIRM, boot_if)
        check(got_image == new_image and got_if["path"] == b"x", "a fully valid run was refused")
        must_raise("no --confirm", lambda: prepare(ipath, bk, "", boot_if), "--confirm")
        must_raise("wrong --confirm", lambda: prepare(ipath, bk, "yes", boot_if), "--confirm")
        must_raise("normal-mode keyboard", lambda: prepare(ipath, bk, CONFIRM, normal_if), "normal mode")
        must_raise("no device", lambda: prepare(ipath, bk, CONFIRM, []), "no 19F5:AF1F")
        must_raise("missing backup", lambda: prepare(ipath, td / "none", CONFIRM, boot_if), "not usable")
        two_ifs = boot_if + [dict(boot_if[0], path=b"z")]
        must_raise("ambiguous interfaces", lambda: prepare(ipath, bk, CONFIRM, two_ifs), "cannot tell")

        (bk / "bootloader_0x08000000.bin").write_bytes(b"\xFF" * 0x8000)
        must_raise("empty bootloader backup", lambda: prepare(ipath, bk, CONFIRM, boot_if), "empty")
        altered = bytearray(boot)
        altered[0x100] ^= 0xFF
        (bk / "bootloader_0x08000000.bin").write_bytes(bytes(altered))
        must_raise("bootloader backup altered after the manifest", lambda: prepare(ipath, bk, CONFIRM, boot_if), "sha256")
        (bk / "bootloader_0x08000000.bin").write_bytes(bytes(boot))

        other = bytearray(settings)
        other[0x6000] ^= 0xFF
        ipath2 = td / "j.bin"
        ipath2.write_bytes(img.build_image(new_app, bytes(other)))
        must_raise("settings differ from the backup", lambda: prepare(ipath2, bk, CONFIRM, boot_if), "differs from the backup")
        ok_img, _ = prepare(ipath2, bk, CONFIRM, boot_if, allow_settings_diff=True)
        check(ok_img == img.build_image(new_app, bytes(other)), "--allow-settings-diff did not allow it")

    # --- the module really has no other way to send: HidapiTransport.exchange is only reached through flash()
    import inspect
    src = inspect.getsource(flash)
    check("build_erase()" in src and "build_chunk(" in src and "build_finish(" in src and "_dev.write" not in src,
          "flash() builds packets some other way than the guarded builders")

    print("SELF-TEST", "FAILED (%d)" % len(failures) if failures else "OK")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
