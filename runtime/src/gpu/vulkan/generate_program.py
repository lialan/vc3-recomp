#!/usr/bin/env python3
"""Specialization constants for the shared NV2A vertex instruction encoding."""
import pathlib,sys
out=['#ifndef CH_VERTEX_PROGRAM_GLSL', '#define CH_VERTEX_PROGRAM_GLSL', 'layout(constant_id=0) const uint vp_count=1u;']
for i in range(544):
    out.append(f'layout(constant_id={i+1}) const uint vp_{i}=0u;')
out.append('layout(constant_id=545) const uint vp_fetch_specialized=0u;')
for i in range(16):
    out.append(f'layout(constant_id={546+i}) const uint vp_format_{i}=0u;')
out += ['uint vp_attribute_format(uint slot) {', '    switch(slot) {']
for i in range(16):
    out.append(f'    case {i}u: return vp_format_{i};')
out += ['    default: return 0u;', '    }', '}']
out += ['layout(constant_id=562) const uint vp_input_mask=65535u;', '#endif']
pathlib.Path(sys.argv[1]).write_text('\n'.join(out)+'\n')
# CH_VP_STEPS: a module with fewer steps (Qualcomm's proprietary compiler
# takes far longer the more steps there are, vk_fixed.h)
steps=['#ifndef CH_VP_STEPS', '#define CH_VP_STEPS 136', '#endif']
for i in range(136):
    words=','.join(f'vp_{i*4+j}' for j in range(4))
    steps += [f'#if CH_VP_STEPS>{i}', f'if(vp_count>{i}u) vp_step(uvec4({words}),v,R,O,a0);', '#endif']
pathlib.Path(sys.argv[1]).with_name('vertex_steps.glsl').write_text('\n'.join(steps)+'\n')
