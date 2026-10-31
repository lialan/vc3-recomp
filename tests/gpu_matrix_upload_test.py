#!/usr/bin/env python3
"""Compile the actual cached matrix upload wrapper against GL mocks, on both APIs."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / "runtime/src/gpu/gl_backend.c").read_text()
start = source.index("static void cuMatrix4fv(")
end = source.index("static uint32_t s_frame", start)
wrapper = source[start:end]
harness = r"""
#include <assert.h>
#include <stddef.h>
#include <string.h>
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
#define GL_FALSE 0
#define GL_TRUE 1
static GLfloat cached[64], uploaded[64];
static size_t cached_bytes;
static int calls;
static GLboolean uploaded_transpose;
static int uc_same(GLint loc, const void *p, size_t bytes) {
    (void)loc;
    assert(bytes <= sizeof cached);
    if (bytes == cached_bytes && !memcmp(cached, p, bytes)) return 1;
    memcpy(cached, p, bytes);
    cached_bytes = bytes;
    return 0;
}
static void glUniformMatrix4fv(GLint loc, GLsizei n, GLboolean t, const GLfloat *p) {
    (void)loc;
    calls++;
    uploaded_transpose = t;
    memcpy(uploaded, p, (size_t)n * 64);
}
"""
checks = r"""
int main(void) {
    GLfloat input[32], original[32];
    int k, r, c;
    for (k = 0; k < 32; k++) input[k] = (GLfloat)(k + 1);
    memcpy(original, input, sizeof input);
    cuMatrix4fv(3, 2, GL_TRUE, input);
    assert(calls == 1);
#if defined(VC3_GLES)
    assert(uploaded_transpose == GL_FALSE);
    for (k = 0; k < 2; k++)
        for (r = 0; r < 4; r++)
            for (c = 0; c < 4; c++)
                assert(uploaded[k * 16 + c * 4 + r] == input[k * 16 + r * 4 + c]);
#else
    (void)r; (void)c;
    assert(uploaded_transpose == GL_TRUE);
    assert(!memcmp(uploaded, input, sizeof input));
#endif
    assert(!memcmp(input, original, sizeof input));
    cuMatrix4fv(3, 2, GL_TRUE, input);
    assert(calls == 1); /* unchanged guest matrices still hit the cache */
    input[0] = 101;
    cuMatrix4fv(3, 2, GL_FALSE, input);
    assert(calls == 2 && uploaded_transpose == GL_FALSE);
    assert(!memcmp(uploaded, input, sizeof input));
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="matrix-upload-") as temp:
    cfile = pathlib.Path(temp) / "test.c"
    cfile.write_text(harness + wrapper + checks)
    for api, flags in (("desktop", []), ("GLES", ["-DVC3_GLES"])):
        binary = pathlib.Path(temp) / api
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", *flags,
                        str(cfile), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        print(api + " matrix uploads: PASS")
