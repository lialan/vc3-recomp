#!/usr/bin/env python3
"""Compile the GLES ring wait against deterministic GL sync responses.

No GL context or device is needed. Exercise the actual production function,
including repeated timeouts and both wait-failure paths.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "runtime/src/gpu/gl_backend.c").read_text()
start = source.index("static void ring_enter(")
end = source.index("\n#endif", start)
prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <assert.h>
typedef long GLsizeiptr; typedef unsigned GLenum; typedef int GLsync;
#define RING_SEGS 4
#define GL_SYNC_GPU_COMMANDS_COMPLETE 1
#define GL_TIMEOUT_EXPIRED 2
#define GL_SYNC_FLUSH_COMMANDS_BIT 4
#define GL_WAIT_FAILED 3
#define GL_ALREADY_SIGNALED 5
typedef struct {GLsizeiptr size; int seg; GLsync fence[4]; unsigned target;} Ring;
static unsigned s_ring_waits; static uint64_t s_ring_wait_ns;
static int seq[8], at, finishes, deleted;
static void glDeleteSync(GLsync s) {(void)s; deleted++;}
static GLsync glFenceSync(int a,int b) {(void)a;(void)b;return 9;}
static GLenum glClientWaitSync(GLsync a,unsigned b,uint64_t c) {(void)a;(void)b;(void)c;return seq[at++];}
static void glFinish(void) {finishes++;}
'''

checks = r'''

static void run(int poll,int wait,int again,int calls,int finish) {
 Ring r={400,0,{0,7,0,0},1}; at=finishes=deleted=0;
 seq[0]=poll; seq[1]=wait; seq[2]=again;
 ring_enter(&r,100,110);
 assert(at==calls); assert(finishes==finish); assert(r.fence[1]==0);
}
int main(void) {
 run(GL_ALREADY_SIGNALED,0,0,1,0);
 run(GL_TIMEOUT_EXPIRED,GL_ALREADY_SIGNALED,0,2,0);
 run(GL_TIMEOUT_EXPIRED,GL_TIMEOUT_EXPIRED,GL_ALREADY_SIGNALED,3,0);
 run(GL_TIMEOUT_EXPIRED,GL_WAIT_FAILED,0,2,1);
 run(GL_WAIT_FAILED,0,0,1,1);
 puts("ring waits: five scenarios passed");
}
'''

with tempfile.TemporaryDirectory(prefix="chihiro-ring-test-") as directory:
    test = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    test.write_text(prelude + source[start:end] + checks)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                    "-Werror", str(test), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
