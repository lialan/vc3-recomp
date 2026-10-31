#!/usr/bin/env python3
"""Check effect reduction preserves framebuffer bindings on allocation and resize.

No GL context or device is needed. Exercise the actual production function,
including initial allocation, resize, and reuse.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "runtime/src/gpu/gl_backend.c").read_text()
start = source.index("static GLuint fx_small(const Surf *rt)\n{")
end = source.index("\n/* L onto S:", start)
prelude = r'''
#include <stdint.h>
#include <assert.h>
typedef unsigned GLuint; typedef int GLint; typedef int GLsizei;
#define GL_TEXTURE_BINDING_2D 1
#define GL_READ_FRAMEBUFFER_BINDING 2
#define GL_DRAW_FRAMEBUFFER_BINDING 3
#define GL_FRAMEBUFFER 4
#define GL_READ_FRAMEBUFFER 5
#define GL_DRAW_FRAMEBUFFER 6
#define GL_TEXTURE_2D 7
#define GL_RGBA8 8
#define GL_RGBA 9
#define GL_UNSIGNED_BYTE 10
#define GL_TEXTURE_MIN_FILTER 11
#define GL_TEXTURE_MAG_FILTER 12
#define GL_LINEAR 13
#define GL_COLOR_ATTACHMENT0 14
#define GL_SCISSOR_TEST 15
#define GL_COLOR_BUFFER_BIT 16
typedef struct {int pw,ph,st,fxlow;unsigned color,va,fbo;} Surf;
static struct {unsigned small_run,runs,small_va,small_tex,small_fbo;int small_w,small_h;uint64_t t_small;} s_fx;
static int rd=31,dr=32,tex=33;
static int fx_scale(void){return 4;}
static uint64_t tm_now(void){return 1;}
static void glGetIntegerv(int name,int *out){*out=name==1?tex:name==2?rd:dr;}
static void glGenTextures(int n,unsigned *t){(void)n;*t=41;}
static void glGenFramebuffers(int n,unsigned *t){(void)n;*t=42;}
static void glBindTexture(int t,unsigned v){(void)t;tex=v;}
static void glBindFramebuffer(int t,unsigned v){if(t==4||t==5)rd=v;if(t==4||t==6)dr=v;}
#define glTexImage2D(...) ((void)0)
#define glTexParameteri(...) ((void)0)
#define glFramebufferTexture2D(...) ((void)0)
#define glDisable(...) ((void)0)
#define glBlitFramebuffer(...) ((void)0)
'''
checks = r'''

int main(void){
 Surf s={800,600,1,0,50,60,70};s_fx.runs=1;
 assert(fx_small(&s)==41);assert(rd==31&&dr==32&&tex==33);
 s_fx.runs++;s.pw=900;
 assert(fx_small(&s)==41);assert(rd==31&&dr==32&&tex==33);
 s_fx.runs++;
 assert(fx_small(&s)==41);assert(rd==31&&dr==32&&tex==33);
}
'''
with tempfile.TemporaryDirectory(prefix="chihiro-fx-state-") as directory:
    test = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    test.write_text(prelude + source[start:end] + checks)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                    "-Werror", str(test), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
