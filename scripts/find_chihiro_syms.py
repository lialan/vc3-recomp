#!/usr/bin/env python3
"""Map Chihiro hardware strings (JVS, media board, baseboard) to the functions
that reference them, so the HLE layer knows which guest addresses to replace.

Usage: find_chihiro_syms.py <game.xbe> <games/<id>/analysis dir> [regex]
"""
import json
import re
import struct
import sys
from bisect import bisect_right

PATTERN = re.compile(r"jvs|mbcom|baseboard|media ?board|i/o board|eeprom|"
                     r"backup|rtc|firmware|gun|coin|\\device", re.I)


def load_image(xbe_path):
    data = open(xbe_path, "rb").read()
    base = struct.unpack_from("<I", data, 0x104)[0]
    nsec, sec_hdr = struct.unpack_from("<II", data, 0x11C)
    secs = []
    for i in range(nsec):
        off = sec_hdr - base + i * 56
        _, va, vsize, raw, rsize, name_va = struct.unpack_from("<IIIIII", data, off)
        name_off = name_va - base
        name = data[name_off:data.index(b"\0", name_off)].decode("latin-1")
        secs.append((va, vsize, data[raw:raw + rsize], name))
    return secs


# Sections that hold data, not code: the string references are searched for
# in every other section (.text and the libraries: D3D, XPP, ...).
DATA_SECTIONS = {".rdata", ".data", ".data1", ".rodata", ".bss"}


def main():
    xbe, out = sys.argv[1], sys.argv[2]
    pattern = re.compile(sys.argv[3], re.I) if len(sys.argv) > 3 else PATTERN
    secs = load_image(xbe)
    strings = {}
    for sva, _, raw, _ in secs:
        for m in re.finditer(rb"[\x20-\x7e]{4,}", raw):
            txt = m.group().decode()
            if pattern.search(txt):
                strings[sva + m.start()] = txt
    funcs = sorted((int(f["start"], 16), int(f["end"], 16), f["name"])
                   for f in json.load(open(f"{out}/disasm/functions.json")))
    starts = [f[0] for f in funcs]

    # Any 4-byte little-endian occurrence of a string address in a code
    # section counts: it also catches code the disassembler never reached.
    refs = {}  # string addr -> set of function starts (or -site for orphans)
    code = [(va, raw) for va, _, raw, name in secs if name not in DATA_SECTIONS]
    for saddr in strings:
        needle = struct.pack("<I", saddr)
        for va, raw in code:
            for m in re.finditer(re.escape(needle), raw):
                site = va + m.start()
                i = bisect_right(starts, site) - 1
                f = funcs[i] if i >= 0 else None
                key = f[0] if f and f[0] <= site < f[1] else -site
                refs.setdefault(saddr, set()).add(key)

    by_func = {}
    for saddr, fset in refs.items():
        for f in fset:
            by_func.setdefault(f, []).append(strings[saddr])
    names = {f[0]: f for f in funcs}
    for f in sorted(by_func, key=abs):
        if f < 0:
            head = f"0x{-f:08X} (no function)"
        else:
            s, e, _ = names[f]
            head = f"0x{s:08X} size={e - s:5d}"
        print(f"{head}  " +
              " | ".join(sorted(set(by_func[f])))[:300])
    unref = sorted(a for a in strings if a not in refs)
    print(f"\n{len(by_func)} functions reference {len(refs)} of "
          f"{len(strings)} matching strings; unreferenced: "
          + ", ".join(repr(strings[a]) for a in unref[:20]))


if __name__ == "__main__":
    main()
