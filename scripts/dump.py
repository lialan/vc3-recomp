#!/usr/bin/env python3
"""Dump little-endian dwords from vc3.xbe at a guest VA.
Usage: dump.py <vc3.xbe> VA [count]
"""
import struct
import sys


def sections(data):
    base = struct.unpack_from("<I", data, 0x104)[0]
    nsec, hdr = struct.unpack_from("<II", data, 0x11C)
    for i in range(nsec):
        _, va, vsize, raw, rsize = struct.unpack_from(
            "<IIIII", data, hdr - base + i * 56)
        yield va, vsize, raw, rsize


def main():
    data = open(sys.argv[1], "rb").read()
    va = int(sys.argv[2], 16)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 16
    for sva, vsize, raw, rsize in sections(data):
        if sva <= va < sva + vsize:
            for i in range(count):
                off = va - sva + i * 4
                word = (struct.unpack_from("<I", data, raw + off)[0]
                        if off + 4 <= rsize else 0)
                print(f"{va + i * 4:08x}  [{i:2d}]  {word:08x}")
            return
    sys.exit(f"VA {va:#x} not in any section")


if __name__ == "__main__":
    main()
