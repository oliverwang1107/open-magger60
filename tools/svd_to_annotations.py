#!/usr/bin/env python3
"""Turn a CMSIS-SVD register description into a flat register label table (TSV), e.g. for a disassembler.

usage: tools/svd_to_annotations.py <chip.svd> [out.tsv] [PERIPH ...]
       tools/svd_to_annotations.py --self-test
  Use the SVD file for your chip (the N32H48x ones come from the chip maker's device pack). It is NOT stored in this
  repository and neither is a table made from it: run this yourself on your own copy of the SVD. Without a
  peripheral list, the peripherals this project's firmware touches are used.
Output (TSV, columns addr/kind/name/comment) has one `label` row per register, named
<PERIPH>_<REGISTER without its generic family prefix>, e.g. 0x4002b008 -> ADC1_CTRL2,
0x4002c430 -> GPIOB_POD, 0x40002c48 -> GTIM1_CNT. Register arrays (<dim>) are expanded.
Only the Python standard library is needed.
"""
import sys
import xml.etree.ElementTree as ET

USED = ["PWR", "IWDG", "GTIM1", "GTIM2", "GTIM3", "SPI5", "DMA1", "RCC", "FLASH",
        "ADC1", "ADC2", "ADC3", "ADC4", "GPIOA", "GPIOB", "GPIOC", "GPIOD", "GPIOE", "GPIOF",
        "AFIO", "EXTI", "USB_HS", "USB_HS_HOST", "USB_HS_DEVICE", "USB_HS_PWRCLK", "DBG"]


def label(periph, reg):
    """ADC_CTRL2 on ADC1 -> ADC1_CTRL2; PWR_CTRL on PWR -> PWR_CTRL; names without '_' keep the prefix."""
    return "%s_%s" % (periph, reg.split("_", 1)[1]) if "_" in reg else "%s_%s" % (periph, reg)


def convert(root, wanted):
    """Return the TSV text for the peripherals in `wanted` found in the parsed SVD `root`."""
    per = {p.findtext("name"): p for p in root.iter("peripheral")}
    rows = []
    for name in wanted:
        p = per.get(name)
        if p is None:
            print("warning: %s not in SVD" % name, file=sys.stderr)
            continue
        base = int(p.findtext("baseAddress"), 0)
        src = per[p.get("derivedFrom")] if p.get("derivedFrom") and p.find("registers") is None else p
        for r in src.iter("register"):
            off = int(r.findtext("addressOffset"), 0)
            rn = r.findtext("name")
            dim = r.findtext("dim")
            if dim:
                inc = int(r.findtext("dimIncrement"), 0)
                idx = (r.findtext("dimIndex") or "").split(",")
                for i in range(int(dim, 0)):
                    suffix = idx[i] if i < len(idx) and idx[i] else str(i)
                    rows.append((base + off + i * inc, label(name, rn.replace("%s", suffix))))
            else:
                rows.append((base + off, label(name, rn)))
    rows.sort()
    seen, text = set(), ["addr\tkind\tname\tcomment"]
    for a, n in rows:
        if a in seen:
            continue
        seen.add(a)
        text.append("0x%08x\tlabel\t%s\t" % (a, n))
    return text


SELF_TEST_SVD = """<device><peripherals>
 <peripheral><name>TIMX</name><baseAddress>0x40000000</baseAddress><registers>
  <register><name>TIM_CTRL1</name><addressOffset>0x0</addressOffset></register>
  <register><name>TIM_CCDAT%s</name><addressOffset>0x10</addressOffset><dim>2</dim><dimIncrement>4</dimIncrement>
   <dimIndex>A,B</dimIndex></register>
 </registers></peripheral>
 <peripheral derivedFrom="TIMX"><name>TIMY</name><baseAddress>0x40001000</baseAddress></peripheral>
</peripherals></device>"""


def self_test():
    rows = convert(ET.fromstring(SELF_TEST_SVD), ["TIMX", "TIMY", "NOPE"])
    want = ["addr\tkind\tname\tcomment",
            "0x40000000\tlabel\tTIMX_CTRL1\t", "0x40000010\tlabel\tTIMX_CCDATA\t", "0x40000014\tlabel\tTIMX_CCDATB\t",
            "0x40001000\tlabel\tTIMY_CTRL1\t", "0x40001010\tlabel\tTIMY_CCDATA\t", "0x40001014\tlabel\tTIMY_CCDATB\t"]
    ok = rows == want
    print("SELF-TEST", "OK" if ok else "FAILED: %r" % rows)
    return 0 if ok else 1


def main(argv):
    if len(argv) > 1 and argv[1] == "--self-test":
        return self_test()
    if len(argv) < 2:
        print(__doc__)
        return 2
    svd = argv[1]
    out = argv[2] if len(argv) > 2 else "-"
    text = convert(ET.parse(svd).getroot(), argv[3:] or USED)
    data = "\n".join(text) + "\n"
    if out == "-":
        sys.stdout.write(data)
    else:
        open(out, "w", encoding="utf-8").write(data)
        print("%d labels -> %s" % (len(text) - 1, out), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
