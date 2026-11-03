#!/usr/bin/env python3
"""Embed aligned SPIR-V words for C builds, independent of host byte order."""
import pathlib
import struct
import sys
output=pathlib.Path(sys.argv[1])
parts=[]
for name, filename in zip(('vert', 'frag', 'textured', 'combiner', 'effect', 'fixed',
                          'frag_noat', 'textured_noat', 'combiner_noat', 'effect_noat',
                          'combiner_specialized', 'combiner_specialized_noat',
                          'frag_depth', 'textured_depth', 'combiner_depth', 'effect_depth',
                          'frag_noat_depth', 'textured_noat_depth', 'combiner_noat_depth', 'effect_noat_depth',
                          'combiner_specialized_depth', 'combiner_specialized_noat_depth', 'program', 'program_loop', 'program_s16', 'program_s32', 'program_s64'), sys.argv[2:]):
    data=pathlib.Path(filename).read_bytes()
    if len(data)%4 or data[:4]!=b'\x03\x02\x23\x07':
        raise SystemExit(f'Invalid SPIR-V: {filename}')
    words=struct.unpack('<'+'I'*(len(data)//4), data)
    parts.append('static const uint32_t color_'+name+'_spv[]={'+','.join(hex(w) for w in words)+'};\n')
if len(parts) not in (3,4,5,6,10,12,22,23,27) or len(parts)!=len(sys.argv[2:]):
    raise SystemExit('Expected 3..6 base shaders, 10 with no-alpha variants 12 with specialization, 22 with fragment-depth variants, 23 with vertex programs, or 27 with their loop and sized forms')
output.write_text(''.join(parts))
