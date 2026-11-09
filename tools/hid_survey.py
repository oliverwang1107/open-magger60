"""Read-only HID survey (Windows, stdlib only).

Opens every HID interface with access 0 (query only: no read, no write) and prints
VID/PID, strings, top-level usage and report sizes. Nothing is sent to any device.
"""
import ctypes
import re
import sys
from ctypes import wintypes as w

setupapi = ctypes.WinDLL("setupapi", use_last_error=True)
hid = ctypes.WinDLL("hid", use_last_error=True)
k32 = ctypes.WinDLL("kernel32", use_last_error=True)

HANDLE = ctypes.c_void_p
INVALID = HANDLE(-1).value


class GUID(ctypes.Structure):
    _fields_ = [("D1", w.DWORD), ("D2", w.WORD), ("D3", w.WORD), ("D4", ctypes.c_ubyte * 8)]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [("cbSize", w.DWORD), ("Guid", GUID), ("Flags", w.DWORD), ("Reserved", ctypes.c_void_p)]


class HIDD_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("Size", w.ULONG), ("VendorID", w.USHORT), ("ProductID", w.USHORT), ("VersionNumber", w.USHORT)]


class HIDP_CAPS(ctypes.Structure):
    _fields_ = [
        ("Usage", w.USHORT), ("UsagePage", w.USHORT),
        ("InputReportByteLength", w.USHORT), ("OutputReportByteLength", w.USHORT),
        ("FeatureReportByteLength", w.USHORT), ("Reserved", w.USHORT * 17),
        ("NumberLinkCollectionNodes", w.USHORT),
        ("NumberInputButtonCaps", w.USHORT), ("NumberInputValueCaps", w.USHORT), ("NumberInputDataIndices", w.USHORT),
        ("NumberOutputButtonCaps", w.USHORT), ("NumberOutputValueCaps", w.USHORT), ("NumberOutputDataIndices", w.USHORT),
        ("NumberFeatureButtonCaps", w.USHORT), ("NumberFeatureValueCaps", w.USHORT), ("NumberFeatureDataIndices", w.USHORT),
    ]


setupapi.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(GUID), w.LPCWSTR, w.HWND, w.DWORD]
setupapi.SetupDiGetClassDevsW.restype = HANDLE
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [HANDLE, ctypes.c_void_p, ctypes.POINTER(GUID), w.DWORD, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)]
setupapi.SetupDiEnumDeviceInterfaces.restype = w.BOOL
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [HANDLE, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA), ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD), ctypes.c_void_p]
setupapi.SetupDiGetDeviceInterfaceDetailW.restype = w.BOOL
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [HANDLE]
k32.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p, w.DWORD, w.DWORD, HANDLE]
k32.CreateFileW.restype = HANDLE
k32.CloseHandle.argtypes = [HANDLE]
hid.HidD_GetAttributes.argtypes = [HANDLE, ctypes.POINTER(HIDD_ATTRIBUTES)]
hid.HidD_GetPreparsedData.argtypes = [HANDLE, ctypes.POINTER(ctypes.c_void_p)]
hid.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]
hid.HidP_GetCaps.argtypes = [ctypes.c_void_p, ctypes.POINTER(HIDP_CAPS)]
for name in ("HidD_GetManufacturerString", "HidD_GetProductString", "HidD_GetSerialNumberString"):
    getattr(hid, name).argtypes = [HANDLE, ctypes.c_wchar_p, w.ULONG]

HID_GUID = GUID(0x4D1E55B2, 0xF16F, 0x11CF, (ctypes.c_ubyte * 8)(0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30))
DIGCF_PRESENT, DIGCF_DEVICEINTERFACE = 0x2, 0x10
SHARE_RW = 3
OPEN_EXISTING = 3

USAGE_PAGES = {0x01: "GenericDesktop", 0x0C: "Consumer", 0x0D: "Digitizer", 0x20: "Sensor", 0xFF00: "Vendor", 0xFF01: "Vendor", 0xFF02: "Vendor"}
GD_USAGES = {0x01: "Pointer", 0x02: "Mouse", 0x04: "Joystick", 0x05: "Gamepad", 0x06: "Keyboard", 0x07: "Keypad", 0x80: "SystemControl", 0x08: "MultiAxis"}


def wstr(fn, h):
    buf = ctypes.create_unicode_buffer(256)
    return buf.value if fn(h, buf, ctypes.sizeof(buf)) else ""


def survey():
    out = []
    info = setupapi.SetupDiGetClassDevsW(ctypes.byref(HID_GUID), None, None, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    i = 0
    while True:
        d = SP_DEVICE_INTERFACE_DATA()
        d.cbSize = ctypes.sizeof(d)
        if not setupapi.SetupDiEnumDeviceInterfaces(info, None, ctypes.byref(HID_GUID), i, ctypes.byref(d)):
            break
        i += 1
        need = w.DWORD()
        setupapi.SetupDiGetDeviceInterfaceDetailW(info, ctypes.byref(d), None, 0, ctypes.byref(need), None)
        buf = ctypes.create_string_buffer(need.value)
        ctypes.c_uint32.from_buffer(buf).value = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6  # cbSize of the detail struct
        if not setupapi.SetupDiGetDeviceInterfaceDetailW(info, ctypes.byref(d), buf, need.value, None, None):
            continue
        path = ctypes.wstring_at(ctypes.addressof(buf) + 4)
        h = k32.CreateFileW(path, 0, SHARE_RW, None, OPEN_EXISTING, 0, None)
        if h in (None, INVALID):
            out.append(dict(path=path, err=ctypes.get_last_error()))
            continue
        attr = HIDD_ATTRIBUTES(); attr.Size = ctypes.sizeof(attr)
        hid.HidD_GetAttributes(h, ctypes.byref(attr))
        rec = dict(path=path, vid=attr.VendorID, pid=attr.ProductID, rev=attr.VersionNumber,
                   mfr=wstr(hid.HidD_GetManufacturerString, h), prod=wstr(hid.HidD_GetProductString, h),
                   ser=wstr(hid.HidD_GetSerialNumberString, h))
        pp = ctypes.c_void_p()
        if hid.HidD_GetPreparsedData(h, ctypes.byref(pp)):
            caps = HIDP_CAPS()
            hid.HidP_GetCaps(pp, ctypes.byref(caps))
            rec.update(page=caps.UsagePage, usage=caps.Usage, inlen=caps.InputReportByteLength,
                       outlen=caps.OutputReportByteLength, featlen=caps.FeatureReportByteLength,
                       inbtn=caps.NumberInputButtonCaps, inval=caps.NumberInputValueCaps)
            hid.HidD_FreePreparsedData(pp)
        k32.CloseHandle(h)
        out.append(rec)
    setupapi.SetupDiDestroyDeviceInfoList(info)
    return out


def main():
    want = None
    if len(sys.argv) > 1:
        want = sys.argv[1].lower()  # e.g. 0c45:8076
    rows = survey()
    for r in rows:
        if "vid" not in r:
            continue
        key = f"{r['vid']:04x}:{r['pid']:04x}"
        if want and key != want:
            continue
        m = re.search(r"mi_(\d+)", r["path"], re.I)
        c = re.search(r"col(\d+)", r["path"], re.I)
        page = r.get("page")
        pname = USAGE_PAGES.get(page, "") if page is not None else "?"
        uname = GD_USAGES.get(r.get("usage"), "") if page == 1 else ""
        print(f"{key}  MI={m.group(1) if m else '-':>2} COL={c.group(1) if c else '-':>2}  "
              f"page=0x{page:04X}({pname}) usage=0x{r['usage']:02X}({uname})  "
              f"in/out/feat={r['inlen']}/{r['outlen']}/{r['featlen']}  "
              f"mfr='{r['mfr']}' prod='{r['prod']}' ser='{r['ser'][:12]}' rev=0x{r['rev']:04X}")
    denied = [r for r in rows if "err" in r]
    if denied:
        print(f"\n({len(denied)} interface(s) could not be opened even for query, e.g. error {denied[0]['err']})")


if __name__ == "__main__":
    main()
