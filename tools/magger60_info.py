#!/usr/bin/env python3
"""magger60_info.py - ask a running Magger60 for its version string (cmd 0x03, read-only).

Sends exactly one request, the same one magger60_backup.py sends first, through the
backup tool's read-only allow-list and its second safety check. Works on the stock firmware and on our own
images that implement cmd 3 (M2-D3).

    python tools/magger60_info.py            # print the reply
    python tools/magger60_info.py --self-test
"""
import importlib.util
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("magger60_backup", HERE / "magger60_backup.py")
bk = importlib.util.module_from_spec(_spec)
sys.modules["magger60_backup"] = bk
_spec.loader.exec_module(bk)


def decode(resp: bytes) -> dict:
    """Split a cmd 3 reply: 0xAA, cmd, 0, checksum, len, offset(3), then bcd version (LE16) and the ASCII build string."""
    if len(resp) < 8 or resp[0] != 0xAA or resp[1] != 0x03:
        raise bk.BackupError("unexpected reply: %s" % resp[:16].hex())
    n = resp[4]
    data = resp[8:8 + n]
    if (n + resp[5] + resp[6] + resp[7] + sum(data)) & 0xFF != resp[3]:
        raise bk.BackupError("reply checksum does not match: %s" % resp[:16].hex())
    if n < 2:
        raise bk.BackupError("reply too short")
    return {"version": "%X" % (data[0] | (data[1] << 8)),
            "build": bytes(data[2:]).split(b"\x00")[0].decode("ascii", "replace")}


def self_test() -> int:
    payload = bytes([0x04, 0x01]) + b"Jan  1 2026,00:00:00STOCK-SIM"            # BCD version 1.04 + build string
    head = bytes([0xAA, 0x03, 0x00, bk.checksum(len(payload), 0, payload), len(payload), 0, 0, 0])
    stock = head + payload + bytes(64 - len(head) - len(payload))
    info = decode(stock)
    ok = info == {"version": "104", "build": "Jan  1 2026,00:00:00STOCK-SIM"}
    bad = bytearray(stock)
    bad[3] ^= 1
    try:
        decode(bytes(bad))
        ok = False
    except bk.BackupError:
        pass
    try:
        decode(b"\xAB" + stock[1:])
        ok = False
    except bk.BackupError:
        pass
    print("SELF-TEST", "OK" if ok else "FAILED")
    return 0 if ok else 1


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    if "--self-test" in sys.argv:
        return self_test()
    try:
        iface = bk.pick_interface()
        t = bk.HidapiTransport(iface["path"])
        try:
            resp = t.exchange(bk.build_request(bk.CMD_GET_INFO, 0, 0))
        finally:
            t.close()
        print("reply:", resp[:40].hex())
        info = decode(resp)
        print("firmware version %s, build '%s'" % (info["version"], info["build"]))
        return 0
    except bk.BackupError as exc:
        print("ERROR:", exc)
        return 1


if __name__ == "__main__":
    sys.exit(main())
