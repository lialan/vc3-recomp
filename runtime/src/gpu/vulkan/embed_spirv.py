#!/usr/bin/env python3
"""Embed aligned SPIR-V words for C builds, independent of host byte order."""
import pathlib
import struct
import sys
output=pathlib.Path(sys.argv[1])
parts=[]
for name, filename in zip(('vert', 'frag', 'textured', 'combiner', 'effect', 'fixed'), sys.argv[2:]):
    data=pathlib.Path(filename).read_bytes()
    if len(data)%4 or data[:4]!=b'\x03\x02\x23\x07':
        raise SystemExit(f'Invalid SPIR-V: {filename}')
    words=struct.unpack('<'+'I'*(len(data)//4), data)
    parts.append('static const uint32_t color_'+name+'_spv[]={'+','.join(hex(w) for w in words)+'};\n')
if len(parts) not in (3,4,5,6):
    raise SystemExit('Expected vertex, untextured fragment and textured fragment inputs')
output.write_text(''.join(parts))
