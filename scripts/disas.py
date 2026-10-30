#!/usr/bin/env python3
"""Disassemble guest functions from vc3.xbe by VA, using the disassembler's
function bounds. Usage: disas.py <vc3.xbe> <functions.json> VA [VA ...] [-n N]
"""
import argparse
import json
import struct

from capstone import CS_ARCH_X86, CS_MODE_32, Cs


def sections(data):
    base = struct.unpack_from("<I", data, 0x104)[0]
    nsec, hdr = struct.unpack_from("<II", data, 0x11C)
    for i in range(nsec):
        _, va, vsize, raw, rsize = struct.unpack_from(
            "<IIIII", data, hdr - base + i * 56)
        yield va, vsize, raw, rsize


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("xbe")
    ap.add_argument("functions")
    ap.add_argument("va", nargs="+", type=lambda s: int(s, 16))
    ap.add_argument("-n", type=int, default=80, help="max instructions")
    a = ap.parse_args()
    data = open(a.xbe, "rb").read()
    secs = list(sections(data))
    funcs = {int(f["start"], 16): f for f in json.load(open(a.functions))}
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    for va in a.va:
        f = funcs.get(va)
        size = f["size"] if f else 256
        raw = next(raw + va - sva for sva, vs, raw, rs in secs
                   if sva <= va < sva + vs)
        print(f"--- {va:#010x} size={size} callers="
              f"{(f or {}).get('called_by', [])[:8]}")
        for n, i in enumerate(md.disasm(data[raw:raw + size], va)):
            if n >= a.n:
                print("   ...")
                break
            print(f"  {i.address:08x}  {i.mnemonic:7s} {i.op_str}")


if __name__ == "__main__":
    main()
