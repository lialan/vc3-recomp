#!/usr/bin/env python3
"""Exercise actual per-program projection cache with A/B/A shader switches."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/src/gpu/gl_backend.c').read_text()
a = source.index('static void raw_projection(')
function = source[a:source.index('static void raw_uniforms(', a)]
harness = r'''
#include <assert.h>
#include <stdint.h>
#include "host/vr_math.h"
typedef struct { float composite[16], mv[4][16], proj[16], c59[4]; uint32_t use_composite; } Nv2aFixedState;
typedef struct { const Nv2aFixedState *ff; float aa_sx, aa_sy; } Nv2aRawGeometry;
typedef struct { Nv2aFixedState ff_last; int ff_valid; VrGameProj ff_gp; float ff_aa[2]; int ff_gp_ok; } Prog;
static Prog *U;
static VrGameProj s_bgp;
static int s_bgp_ok;
static uint32_t s_gp_good, s_gp_bad;
'''
checks = r'''
static void draw(Prog *program, const Nv2aRawGeometry *g) {
    U = program;
    raw_projection(g);
    program->ff_last = *g->ff; /* uniform cache update following projection */
    program->ff_valid = 1;
}
int main(void) {
    Prog a = {0}, b = {0}, invalid = {0};
    Nv2aFixedState fa = {0}, fb = {0}, fi = {0};
    fa.proj[0] = 300; fa.proj[5] = -300; fa.proj[14] = 1;
    fb = fa; fb.proj[0] = 600;
    Nv2aRawGeometry ga = {&fa, 1, 1}, gb = {&fb, 1, 1}, gi = {&fi, 1, 1};
    draw(&a, &ga); assert(s_bgp_ok && s_bgp.kx == 300);
    draw(&b, &gb); assert(s_bgp_ok && s_bgp.kx == 600);
    draw(&a, &ga); assert(s_bgp_ok && s_bgp.kx == 300);
    assert(s_gp_good == 2); /* restoring A should not recompute */
    ga.aa_sx = 2;
    draw(&a, &ga); assert(s_bgp.kx == 600);
    draw(&b, &gb); assert(s_bgp.kx == 600);
    ga.aa_sx = 1;
    draw(&a, &ga); assert(s_bgp.kx == 300);
    draw(&invalid, &gi); assert(!s_bgp_ok);
    draw(&b, &gb); assert(s_bgp_ok);
    draw(&invalid, &gi); assert(!s_bgp_ok);
    assert(s_gp_bad == 1); /* cached invalidity must be restored too */
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='projection-cache-') as temp:
    temp = Path(temp)
    (temp / 'test.c').write_text(harness + function + checks)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(root / 'runtime/src'),
                    str(temp / 'test.c'), '-o', str(temp / 'test')], check=True)
    subprocess.run([str(temp / 'test')], check=True)
print('projection A/B/A restore, AA changes, cached invalidity: PASS')
