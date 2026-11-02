#!/usr/bin/env python3
"""Check actual fragment texture-decoder state helpers with GL mocks."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "runtime/src/gpu/gl_texdec.c").read_text()
start = source.index("typedef struct {\n    GLint dfbo")
state = source[start:source.index("} Saved;", start) + len("} Saved;")]
start = source.index("static void save_raster(Saved *s)\n{")
helpers = source[start:source.index("static void lvl_dims", start)]
harness = r"""
#include <assert.h>
#include <string.h>
typedef int GLint;
typedef unsigned int GLuint;
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
#define GL_TRUE 1
#define GL_FALSE 0
enum { GL_DRAW_FRAMEBUFFER_BINDING, GL_VERTEX_ARRAY_BINDING, GL_VIEWPORT,
       GL_COLOR_WRITEMASK, GL_BLEND, GL_SCISSOR_TEST, GL_CULL_FACE, GL_DITHER,
       GL_DRAW_FRAMEBUFFER };
static int s_clipext = 1;
static GLuint s_vao;
static GLboolean enabled[0x4000];
static void glGetIntegerv(GLenum p, GLint *v) { memset(v, 0, (p == GL_VIEWPORT ? 4 : 1) * sizeof *v); }
static void glGetBooleanv(GLenum p, GLboolean *v) { (void)p; memset(v, 1, 4); }
static GLboolean glIsEnabled(GLenum p) { return enabled[p]; }
static void glEnable(GLenum p) { enabled[p] = 1; }
static void glDisable(GLenum p) { enabled[p] = 0; }
static void glColorMask(GLboolean a, GLboolean b, GLboolean c, GLboolean d) { (void)a;(void)b;(void)c;(void)d; }
static void glBindVertexArray(GLuint a) { (void)a; }
static void glBindFramebuffer(GLenum t, GLuint a) { (void)t;(void)a; }
static void glViewport(GLint x, GLint y, GLint w, GLint h) { (void)x;(void)y;(void)w;(void)h; }
"""
checks = r"""
int main(void) {
    Saved s;
    int mask, i;
    for (mask = 0; mask < 8; mask++) {
        memset(enabled, 0, sizeof enabled);
        for (i = 0; i < 3; i++) enabled[0x3000 + i] = (mask >> i) & 1;
        enabled[GL_BLEND] = 1;
        save_raster(&s);
        for (i = 0; i < 3; i++) assert(!enabled[0x3000 + i]);
        assert(!enabled[GL_BLEND]);
        restore_raster(&s);
        for (i = 0; i < 3; i++) assert(enabled[0x3000 + i] == ((mask >> i) & 1));
        assert(enabled[GL_BLEND]);
    }
    s_clipext = 0;
    memset(enabled, 0, sizeof enabled);
    save_raster(&s);
    restore_raster(&s);
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="texdec-state-") as temp:
    cfile = Path(temp) / "test.c"
    binary = Path(temp) / "test"
    cfile.write_text(harness + state + helpers + checks)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(cfile), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("fragment texture decode preserves all three clip enables: PASS")
