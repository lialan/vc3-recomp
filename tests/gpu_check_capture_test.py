#!/usr/bin/env python3
"""Exercise the real lazy-skip decision for checked versus normal draws."""
from pathlib import Path
import subprocess
import tempfile
source = (Path(__file__).resolve().parents[1] / 'runtime/src/gpu/gl_backend.c').read_text()
a = source.index('static int lazy_skip(Surf *f, const Nv2aBatch *b)\n{')
function = source[a:source.index('/* 2D covering', a)]
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define LAZY_FRAMES 120
static uint32_t s_frame = 1000;
typedef struct { float *check_out; } Raw;
typedef struct { uint32_t offset; } Texture;
typedef struct { const Raw *raw; const Texture *texture; } Nv2aBatch;
typedef struct { uint32_t first_drawn, not_copy, fbo, st, cpu_owned, last_sampled, copy_src, skipping, va; } Surf;
static Surf src = { .fbo = 1 };
static int lazy_on(void) { return 1; }
static int batch_flat(const Nv2aBatch *b) { (void)b; return 1; }
static Surf *surf_find(uint32_t offset) { (void)offset; return &src; }
static int batch_fullscreen(const Nv2aBatch *b, const Surf *f) { (void)b; (void)f; return 1; }
static int surf_shown(const Surf *f) { (void)f; return 0; }
static const char *genv(const char *key) { (void)key; return NULL; }
'''
checks = r'''
int main(void) {
    float buffer[4] = {278,455,0,1}; /* a previous captured font quad */
    Texture texture = { .offset = 128 };
    Raw raw = {0};
    Nv2aBatch batch = { .raw = &raw, .texture = &texture };
    Surf target = { .first_drawn = 1, .fbo = 1, .st = 1 };
    assert(lazy_skip(&target, &batch) == 1); /* production optimization remains */
    raw.check_out = buffer;
    assert(lazy_skip(&target, &batch) == 0); /* must reach fresh transform feedback */
    raw.check_out = NULL;
    assert(lazy_skip(&target, &batch) == 1);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='check-capture-') as temp:
    temp = Path(temp)
    (temp / 'test.c').write_text(harness + function + checks)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(temp / 'test.c'), '-o', str(temp / 'test')], check=True)
    subprocess.run([str(temp / 'test')], check=True)
print('checked copies bypass lazy skip; normal copies retain it: PASS')
