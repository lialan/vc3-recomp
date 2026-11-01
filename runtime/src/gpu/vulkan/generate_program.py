#!/usr/bin/env python3
"""Specialization constants for the shared NV2A vertex instruction encoding."""
import pathlib,sys
out=['layout(constant_id=0) const uint vp_count=1u;']
for i in range(544):
    out.append(f'layout(constant_id={i+1}) const uint vp_{i}=0u;')
pathlib.Path(sys.argv[1]).write_text('\n'.join(out)+'\n')
steps=[]
for i in range(136):
    words=','.join(f'vp_{i*4+j}' for j in range(4))
    steps.append(f'if(vp_count>{i}u) vp_step(uvec4({words}),v,R,O,a0);')
pathlib.Path(sys.argv[1]).with_name('vertex_steps.glsl').write_text('\n'.join(steps)+'\n')
