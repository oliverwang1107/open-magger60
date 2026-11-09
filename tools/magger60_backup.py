#!/usr/bin/env python3
"""magger60_backup.py - READ-ONLY flash backup for the LUMINKEY Magger60 HE (CUSTOM_FIRMWARE_PLAN.md M0-5 / M1).

Purpose: before anything is ever flashed, save the parts of flash that no update file
contains and that cannot be rebuilt: the bootloader (0x08000000-0x08007FFF) and the
settings sectors (0x08028000-0x0803FFFF, including the per-key calibration at 0x0802E000).

STATUS: used on the author's keyboard (see M2_PLAN.md). The response layout of the flash-read command
(0xE0) was worked out from the keyboard's behaviour, which is why the tool checks the first bytes of app
flash before dumping anything else (see sanity_check).

Safety, in layers:
  1. Dry run by default: nothing touches a device unless --run is given.
  2. build_request() can only build commands on a read-only allow-list, and refuses a
     deny-list of every command that writes, resets, switches state or boots (0xE1 and 0x06
     among them).
  3. guard_outgoing() independently re-parses the final 64 bytes just before they are sent:
     header 'U', no XOR, allow-listed command, and an all-zero payload (a read request
     carries no data, so no write data can ever be sent).
  4. The tool only opens the vendor interface of a *normal-mode* Magger60 (19F5:FB49);
     the bootloader-mode PID (19F5:AF1F) is refused.

    python tools/magger60_backup.py --self-test          # no hardware: simulator + safety checks
    python tools/magger60_backup.py --list               # show matching HID interfaces
    python tools/magger60_backup.py --plan               # print what --run would request (default)
    python tools/magger60_backup.py --run --out backup1  # really read (needs: pip install hidapi)
"""
import argparse
import hashlib
import json
import re
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

VID = 0x19F5
NORMAL_PIDS = {0xFB49}
BOOT_PIDS = {0xAF1F}
FLASH_BASE = 0x08000000
FLASH_SIZE = 0x80000
MAX_CHUNK = 56
REPORT_LEN = 64

HDR_NORMAL = 0x55
CMD_GET_INFO = 0x03
CMD_FLASH_READ = 0xE0

# Only these commands may ever be built or sent (FIRMWARE_Magger60.md section 6).
READ_ONLY_CMDS = {0x03, 0x04, 0x05, 0x07, 0x08, 0x0A, 0x0C, 0xA0, 0xA2, 0xA4, 0xA6, 0xAA, 0xDE, 0xE0, 0xF1}
# Everything that writes, resets, changes state or boots. Redundant with the allow-list on purpose.
NEVER_CMDS = ({0x01, 0x02, 0x06, 0x09, 0x0B, 0x0D, 0x0E, 0xA1, 0xA3, 0xA5, 0xA7, 0xA8, 0xA9, 0xB1, 0xDD,
               0xE1, 0xEE, 0xF2} | set(range(0x80, 0x85)))

REGIONS = {
    # name: (flash offset from 0x08000000, length)
    "bootloader": (0x00000, 0x08000),   # not in any update file
    "settings":   (0x28000, 0x18000),   # ID block, profiles, calibration, triggers, keymap, macros, DKS...
    "app":        (0x08000, 0x20000),   # reproducible from the update image; optional
}
DEFAULT_REGIONS = ["bootloader", "settings"]

def synthetic_app_image(size: int = 0x38128) -> bytes:
    """A stand-in app image for the self-test: a plausible vector table, then deterministic filler.

    The self-test must not need any file from outside the repository (in particular no stock firmware image).
    """
    out = bytearray((i * 31 + 7) & 0xFF for i in range(size))
    out[0:4] = (0x20010000).to_bytes(4, "little")        # initial SP
    out[4:8] = (0x08008201).to_bytes(4, "little")        # reset vector (Thumb bit set)
    return bytes(out)


class SafetyError(Exception):
    """A request was refused because it could write to or change the keyboard."""


class BackupError(Exception):
    """The keyboard did not behave as the analysis predicted; nothing further was read."""


# --------------------------------------------------------------------------- protocol

def checksum(length: int, offset: int, data: bytes = b"") -> int:
    return (length + (offset & 0xFF) + ((offset >> 8) & 0xFF) + ((offset >> 16) & 0xFF) + sum(data)) & 0xFF


def build_request(cmd: int, offset: int = 0, length: int = 0) -> bytes:
    """Build a read request: 'U' header, no XOR, no payload."""
    if cmd in NEVER_CMDS or cmd not in READ_ONLY_CMDS:
        raise SafetyError("command 0x%02X is not on the read-only allow-list" % cmd)
    if not 0 <= length <= MAX_CHUNK:
        raise SafetyError("length %d out of range" % length)
    if not 0 <= offset <= 0xFFFFFF:
        raise SafetyError("offset out of range")
    if cmd == CMD_FLASH_READ:
        if offset + length > FLASH_SIZE:
            raise SafetyError("flash read beyond 512 KB")
    elif offset > 0xFFFF:
        raise SafetyError("offset above 16 bits for a non-flash command")
    p = bytearray(REPORT_LEN)
    p[0] = HDR_NORMAL
    p[1] = cmd
    p[2] = 0
    p[3] = checksum(length, offset)
    p[4] = length
    p[5] = offset & 0xFF
    p[6] = (offset >> 8) & 0xFF
    p[7] = (offset >> 16) & 0xFF
    return guard_outgoing(bytes(p))


def guard_outgoing(packet: bytes) -> bytes:
    """Second, independent check on the bytes that are actually about to go out."""
    if len(packet) != REPORT_LEN:
        raise SafetyError("packet is not 64 bytes")
    if packet[0] != HDR_NORMAL:
        raise SafetyError("only 'U' packets may be sent (raw '_' packets can reboot the keyboard)")
    if packet[2] != 0:
        raise SafetyError("XOR-masked packets are not allowed")
    cmd = packet[1]
    if cmd in NEVER_CMDS or cmd not in READ_ONLY_CMDS:
        raise SafetyError("command 0x%02X is not read-only" % cmd)
    if packet[4] > MAX_CHUNK:
        raise SafetyError("length field above %d" % MAX_CHUNK)
    if any(packet[8:]):
        raise SafetyError("a read request must not carry payload bytes")
    if packet[3] != checksum(packet[4], packet[5] | (packet[6] << 8) | (packet[7] << 16)):
        raise SafetyError("checksum does not match the header")
    return packet


# --------------------------------------------------------------------------- transports

class Transport:
    def exchange(self, packet: bytes) -> bytes:
        raise NotImplementedError


class HidapiTransport(Transport):
    """Real device through the `hidapi` package (pip install hidapi)."""

    def __init__(self, path: bytes, timeout_ms: int = 500):
        try:
            import hid  # type: ignore
        except ImportError as exc:
            raise BackupError("the 'hidapi' package is missing: pip install hidapi") from exc
        self._timeout = timeout_ms
        if hasattr(hid, "device"):
            self._dev = hid.device()
            self._dev.open_path(path)
        else:  # the other 'hid' package
            self._dev = hid.Device(path=path)

    def exchange(self, packet: bytes) -> bytes:
        packet = guard_outgoing(packet)            # last line of defence, right before the write
        self._dev.write(b"\x00" + packet)          # report ID 0 + 64 bytes
        data = self._dev.read(REPORT_LEN, self._timeout)
        if not data or len(data) < REPORT_LEN:
            raise BackupError("no (or short) response from the keyboard")
        return bytes(data[:REPORT_LEN])

    def close(self) -> None:
        self._dev.close()


def find_interfaces():
    try:
        import hid  # type: ignore
    except ImportError as exc:
        raise BackupError("the 'hidapi' package is missing: pip install hidapi") from exc
    found = []
    for d in hid.enumerate(VID, 0):
        pid = d["product_id"]
        found.append({
            "pid": pid, "path": d["path"], "usage_page": d.get("usage_page"), "usage": d.get("usage"),
            "interface": d.get("interface_number"), "product": d.get("product_string"),
            "normal": pid in NORMAL_PIDS, "bootloader": pid in BOOT_PIDS,
        })
    return found


def format_interface_list(found):
    if not found:
        return ["no HID interface with VID 19F5 found - is the keyboard plugged in (directly, not via a hub)? "
                "tools/hid_survey.py lists every HID device."]
    return ["PID %04X  %-40s usage %s/%s  if %s  %s" % (
        i["pid"], i["product"] or "", i["usage_page"], i["usage"], i["interface"],
        "normal" if i["normal"] else "bootloader" if i["bootloader"] else "other") for i in found]


def pick_interface(pid_filter=None):
    cands = [i for i in find_interfaces()
             if i["normal"] and i["usage_page"] == 0x01 and i["usage"] == 0x00
             and (pid_filter is None or i["pid"] == pid_filter)]
    if not cands:
        raise BackupError("no normal-mode Magger60 vendor interface found (VID 19F5, PID FB49, "
                          "usage page 0x01 usage 0x00). Is the keyboard in normal mode and plugged in directly?")
    if len(cands) > 1:
        raise BackupError("several interfaces match; pass --pid")
    return cands[0]


# --------------------------------------------------------------------------- reading

def read_chunk(t: Transport, cmd: int, offset: int, length: int) -> bytes:
    resp = t.exchange(build_request(cmd, offset, length))
    if resp[0] == 0xAB:
        raise BackupError("the keyboard reported a checksum error at offset 0x%X" % offset)
    if resp[0] != 0xAA:
        raise BackupError("unexpected response byte 0x%02X at offset 0x%X" % (resp[0], offset))
    return resp[8:8 + length]


def read_flash(t: Transport, offset: int, length: int, delay_s: float = 0.0, progress=None) -> bytes:
    out = bytearray()
    while len(out) < length:
        n = min(MAX_CHUNK, length - len(out))
        out += read_chunk(t, CMD_FLASH_READ, offset + len(out), n)
        if progress:
            progress(len(out), length)
        if delay_s:
            time.sleep(delay_s)
    return bytes(out)


def sanity_check(t: Transport, reference: bytes | None) -> dict:
    """Prove that the response layout matches the analysis before reading anything important."""
    info = read_chunk(t, CMD_GET_INFO, 0, 31)
    version = "%X" % (info[0] | (info[1] << 8))
    build = bytes(info[2:31]).split(b"\x00")[0].decode("ascii", "replace")
    head = read_flash(t, 0x8000, MAX_CHUNK)           # start of the app: vector table
    sp = int.from_bytes(head[0:4], "little")
    reset = int.from_bytes(head[4:8], "little")
    if not (0x20000000 <= sp <= 0x20030000 and (reset & 1) and 0x08008000 <= reset < 0x08040000):
        raise BackupError("the first app bytes (SP 0x%08X, Reset 0x%08X) do not look like a vector table: the "
                          "flash-read response is not laid out as predicted, or the flash is read-protected. "
                          "Nothing else was read." % (sp, reset))
    matches = None
    if reference is not None and len(reference) >= MAX_CHUNK:
        matches = reference[:MAX_CHUNK] == head
    return {"firmware_version_hex": version, "build_string": build, "app_head_matches_reference": matches,
            "initial_sp": "0x%08X" % sp, "reset_vector": "0x%08X" % reset}


def run_backup(t: Transport, out_dir: Path, regions, verify=True, delay_s=0.002, reference=None, log=print,
               show_progress=True) -> dict:
    meta = sanity_check(t, reference)
    log("keyboard firmware %s, built '%s'" % (meta["firmware_version_hex"], meta["build_string"]))
    log("app vector table plausible (SP %s, Reset %s)" % (meta["initial_sp"], meta["reset_vector"]))
    if meta["app_head_matches_reference"] is False:
        log("note: the app header differs from the --reference image (a different version is fine)")

    out_dir.mkdir(parents=True, exist_ok=True)
    files = {}
    for name in regions:
        off, size = REGIONS[name]

        def prog(done, total, _n=name):
            if done == total or done % (MAX_CHUNK * 100) < MAX_CHUNK:
                print("\r  %-10s %6d/%d bytes" % (_n, done, total), end="", flush=True)

        data = read_flash(t, off, size, delay_s, prog if show_progress else None)
        if show_progress:
            print()
        if verify:
            again = read_flash(t, off, size, delay_s)
            if again != data:
                raise BackupError("second read of region '%s' differs from the first: not saving it" % name)
        if len(set(data)) == 1:
            log("warning: region '%s' is a single repeated byte 0x%02X - the chip may be read-protected" % (name, data[0]))
        fname = "%s_0x%08X.bin" % (name, FLASH_BASE + off)
        (out_dir / fname).write_bytes(data)
        files[fname] = {"flash_address": "0x%08X" % (FLASH_BASE + off), "bytes": len(data),
                        "sha256": hashlib.sha256(data).hexdigest(), "verified_twice": verify}
        log("  saved %s (%d bytes, sha256 %s...)" % (fname, len(data), files[fname]["sha256"][:12]))
    manifest = {"tool": "magger60_backup.py", "created": datetime.now(timezone.utc).isoformat(),
                "device": meta, "files": files,
                "note": "Read-only backup. Keep these files; the bootloader and calibration cannot be rebuilt."}
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return manifest


def plan(regions) -> None:
    print("Dry run. With --run this tool would send ONLY these read requests (header 'U', no payload):")
    print("  cmd 0x03 (firmware info), cmd 0xE0 at 0x08008000 x1 (sanity check)")
    total = 0
    for name in regions:
        off, size = REGIONS[name]
        n = -(-size // MAX_CHUNK)
        total += n
        print("  cmd 0xE0 '%s': %d requests of up to %d bytes, flash 0x%08X-0x%08X" %
              (name, n, MAX_CHUNK, FLASH_BASE + off, FLASH_BASE + off + size - 1))
    print("  (each region is read twice for verification unless --no-verify)  total ~%d requests" % (2 * total + 2))
    print("Nothing was sent. Add --run to read from the keyboard.")


# --------------------------------------------------------------------------- self test

class SimulatedKeyboard(Transport):
    """Pretends to be a keyboard whose 0xE0 reply is laid out as the firmware analysis predicts."""

    def __init__(self, image: bytes, garbage: bool = False):
        self.image = image
        self.garbage = garbage
        self.seen_cmds = []

    def _flash(self, off: int) -> int:
        if off < 0x8000:                                     # pretend bootloader contents
            return (off * 7 + 3) & 0xFF
        i = off - 0x8000
        return self.image[i] if i < len(self.image) else 0xFF

    def exchange(self, packet: bytes) -> bytes:
        packet = guard_outgoing(packet)
        self.seen_cmds.append(packet[1])
        resp = bytearray(packet)
        resp[0] = 0xAA
        n = packet[4]
        if self.garbage:
            resp[8:8 + n] = bytes([0x00] * n)
        elif packet[1] == CMD_GET_INFO:
            info = bytes([0x04, 0x01]) + b"Jan  1 2026,00:00:00SIMULATED"
            resp[8:8 + len(info)] = info
        elif packet[1] == CMD_FLASH_READ:
            off = packet[5] | (packet[6] << 8) | (packet[7] << 16)
            resp[8:8 + n] = bytes(self._flash(off + i) for i in range(n))
        return bytes(resp)


# Read requests worked out by hand from the frame layout (firmware/hid/include/km/km_hid.h):
#   55 cmd 00 checksum len off_lo off_hi 00 ... zeros, checksum = (len + off_lo + off_hi) & 0xFF
READ_REQUEST_VECTORS = [
    ("read base 0/56", 0x04, 0, 56, bytes.fromhex("55040038" "38000000")),
    ("read trigger 1024/56", 0xA0, 1024, 56, bytes.fromhex("55a0003c" "38000400")),
    ("read trigger 4080/16", 0xA0, 4080, 16, bytes.fromhex("55a0000f" "10f00f00")),
    ("read keymap 4608/56", 0x08, 4608, 56, bytes.fromhex("5508004a" "38001200")),
    ("read calibration 0/56", 0xAA, 0, 56, bytes.fromhex("55aa0038" "38000000")),
]


def parse_js_read_vectors():
    """The read-request packets the self-test expects: (name, cmd, offset, length, 64-byte packet)."""
    return [(name, cmd, off, length, head + bytes(64 - len(head)))
            for name, cmd, off, length, head in READ_REQUEST_VECTORS]


def self_test() -> int:
    failures = []

    def check(cond, msg):
        if not cond:
            failures.append(msg)
            print("  FAIL:", msg)

    # 1. packets equal the hand-computed ones
    vectors = parse_js_read_vectors()
    for name, cmd, off, length, want in vectors:
        if cmd in READ_ONLY_CMDS:
            check(build_request(cmd, off, length) == want, "request '%s' differs from the hand-computed packet" % name)
    print("read-request vectors checked: %d" % len(vectors))

    # 2. every unsafe request is refused, at both layers
    for cmd in range(256):
        if cmd in READ_ONLY_CMDS:
            continue
        try:
            build_request(cmd, 0, 0)
            check(False, "build_request accepted unsafe cmd 0x%02X" % cmd)
        except SafetyError:
            pass
    for cmd in NEVER_CMDS:
        p = bytearray(REPORT_LEN)
        p[0], p[1] = HDR_NORMAL, cmd
        try:
            guard_outgoing(bytes(p))
            check(False, "guard_outgoing accepted unsafe cmd 0x%02X" % cmd)
        except SafetyError:
            pass
    bad_packets = {
        "raw enter-boot packet": bytes([0x5F, 0x06, 0x00, 0x52, 0x01, 0, 0, 0, 0x51]) + bytes(55),
        "read with payload": bytes(build_request(CMD_FLASH_READ, 0, 8)[:8]) + b"\x01" + bytes(55),
        "xor key": bytes([0x55, 0xE0, 0x5A]) + bytes(61),
        "bad checksum": bytes([0x55, 0xE0, 0, 0x00, 8, 0, 0, 0]) + bytes(56),
        "short packet": bytes([0x55, 0xE0]),
        "length 57": bytes([0x55, 0xE0, 0, 57, 57, 0, 0, 0]) + bytes(56),
    }
    for label, pkt in bad_packets.items():
        try:
            guard_outgoing(pkt)
            check(False, "guard_outgoing accepted: " + label)
        except SafetyError:
            pass
    try:
        build_request(CMD_FLASH_READ, FLASH_SIZE - 10, 56)
        check(False, "read past the end of flash was accepted")
    except SafetyError:
        pass
    print("safety layers checked")

    # 3. full backup against the simulator
    image = synthetic_app_image()
    sim = SimulatedKeyboard(image)
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "bk"
        manifest = run_backup(sim, out, DEFAULT_REGIONS + ["app"], verify=True, delay_s=0,
                              reference=image, log=lambda m: None, show_progress=False)
        for name in DEFAULT_REGIONS + ["app"]:
            off, size = REGIONS[name]
            f = out / ("%s_0x%08X.bin" % (name, FLASH_BASE + off))
            data = f.read_bytes()
            check(len(data) == size, "%s has the wrong size" % name)
            check(hashlib.sha256(data).hexdigest() == manifest["files"][f.name]["sha256"], "%s hash mismatch" % name)
            if name == "app":
                check(data == image[:size], "app region is not byte-identical to the image")
        check((out / "manifest.json").exists(), "manifest missing")
    check(all(c in READ_ONLY_CMDS for c in sim.seen_cmds), "the simulator saw a non-read command")
    check(set(sim.seen_cmds) == {CMD_GET_INFO, CMD_FLASH_READ}, "unexpected command set: %r" % set(sim.seen_cmds))
    print("simulated backup: %d requests, commands %s" % (len(sim.seen_cmds), sorted("0x%02X" % c for c in set(sim.seen_cmds))))

    # 4. a keyboard that does not answer as predicted aborts before saving anything
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "bk"
        try:
            run_backup(SimulatedKeyboard(image, garbage=True), out, DEFAULT_REGIONS, delay_s=0,
                       log=lambda m: None, show_progress=False)
            check(False, "garbage responses were not detected")
        except BackupError:
            pass
        check(not out.exists(), "files were written despite a failed sanity check")

    # 5. --list says something useful when nothing is attached, and keeps its row format otherwise
    empty = format_interface_list([])
    check(len(empty) == 1 and "19F5" in empty[0], "--list prints no hint when no interface is found")
    rows = format_interface_list([{"pid": 0xFB49, "product": "Magger 60", "usage_page": 1, "usage": 0,
                                   "interface": 1, "normal": True, "bootloader": False}])
    check(len(rows) == 1 and rows[0].startswith("PID FB49") and rows[0].endswith("normal"), "--list row format changed")

    print("SELF-TEST", "FAILED (%d)" % len(failures) if failures else "OK")
    return 1 if failures else 0


# --------------------------------------------------------------------------- cli

def main() -> int:
    ap = argparse.ArgumentParser(description="READ-ONLY flash backup for the Luminkey Magger60 HE (reads only; never writes).")
    ap.add_argument("--self-test", action="store_true", help="no hardware: simulator and safety checks")
    ap.add_argument("--list", action="store_true", help="list matching HID interfaces")
    ap.add_argument("--run", action="store_true", help="really read from the keyboard (default is a dry run)")
    ap.add_argument("--out", default="magger60_backup", help="output directory")
    ap.add_argument("--regions", nargs="+", choices=sorted(REGIONS), default=DEFAULT_REGIONS)
    ap.add_argument("--pid", type=lambda s: int(s, 16), help="PID in hex when several keyboards are attached")
    ap.add_argument("--reference", metavar="IMAGE",
                    help="optional: an update image of the firmware you expect to be installed; only used to print "
                         "a note when the app header differs (default: no comparison)")
    ap.add_argument("--no-verify", action="store_true", help="read each region only once")
    ap.add_argument("--delay-ms", type=float, default=2.0, help="pause between requests (default 2)")
    args = ap.parse_args()

    try:
        if args.self_test:
            return self_test()
        if args.list:
            for line in format_interface_list(find_interfaces()):
                print(line)
            return 0
        if not args.run:
            plan(args.regions)
            return 0
        iface = pick_interface(args.pid)
        print("using PID %04X interface %s" % (iface["pid"], iface["interface"]))
        t = HidapiTransport(iface["path"])
        try:
            ref = Path(args.reference).read_bytes() if args.reference else None
            run_backup(t, Path(args.out), args.regions, verify=not args.no_verify,
                       delay_s=args.delay_ms / 1000.0, reference=ref)
        finally:
            t.close()
        print("done. Keep the backup folder somewhere safe; do not flash before it is copied off this PC.")
        return 0
    except (BackupError, SafetyError) as exc:
        print("ERROR:", exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
