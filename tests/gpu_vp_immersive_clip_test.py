#!/usr/bin/env python3
"""Run the actual VP epilogue through desktop GL transform feedback (macOS)."""
import ast
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/src/gpu/gl_backend.c').read_text()

def literal(name):
    text = source.split('static const char *' + name + ' =', 1)[1].split('\n\n', 1)[0]
    return ''.join(ast.literal_eval(x) for x in re.findall(r'"(?:[^"\\]|\\.)*"', text))

shader = '#version 410\n' + literal('VS_VP_HEAD') + '''
vec4 v[16];
void nv2a_vsh(in vec4 inputs[16], out vec4 p, out vec4 O[13]) {
    float w = gl_VertexID == 0 ? -2.0 : gl_VertexID == 1 ? 1.0 : 2.0;
    p = vec4(float(gl_VertexID + 2), 4.0, -1.0, w);
    for (int i = 0; i < 13; i++) O[i] = vec4(0.0);
}
void main() {
''' + literal('VS_VP_MAIN')
harness = r'''
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static void check(GLuint o, int program) {
    GLint ok;
    char log[4096];
    if (program) glGetProgramiv(o, GL_LINK_STATUS, &ok);
    else glGetShaderiv(o, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        if (program) glGetProgramInfoLog(o, sizeof log, NULL, log);
        else glGetShaderInfoLog(o, sizeof log, NULL, log);
        fprintf(stderr, "%s\n", log);
        exit(1);
    }
}
int main(int argc, char **argv) {
    (void)argc;
    CGLPixelFormatAttribute attrs[] = { kCGLPFAOpenGLProfile,
        (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core, (CGLPixelFormatAttribute)0 };
    CGLPixelFormatObj pf;
    CGLContextObj ctx;
    GLint np;
    assert(CGLChoosePixelFormat(attrs, &pf, &np) == kCGLNoError);
    assert(CGLCreateContext(pf, NULL, &ctx) == kCGLNoError);
    CGLDestroyPixelFormat(pf);
    CGLSetCurrentContext(ctx);
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *src = calloc((size_t)len + 1, 1);
    assert(fread(src, 1, (size_t)len, f) == (size_t)len);
    fclose(f);
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, (const char **)&src, NULL);
    glCompileShader(vs);
    check(vs, 0);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    const char *varyings[] = { "gl_Position", "gl_ClipDistance" };
    glTransformFeedbackVaryings(prog, 2, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(prog);
    check(prog, 1);
    glUseProgram(prog);
    float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
#define LOC(n) glGetUniformLocation(prog, n)
    glUniformMatrix4fv(LOC("u_immvp"), 1, GL_FALSE, identity);
    glUniform2f(LOC("u_aa"), 1, 1);
    glUniform2f(LOC("u_surf"), 640, 480);
    glUniform2f(LOC("u_depth"), 0, 1);
    glUniform2f(LOC("u_gw"), 1, 1);
    glUniform4f(LOC("u_gp"), 1, 1, 0, 0);
    glUniform1i(LOC("u_nearz"), 1);
    GLuint fbo, tex;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    GLuint vao, buffer;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 3 * 7 * sizeof(float), NULL, GL_STREAM_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
    glEnable(GL_RASTERIZER_DISCARD);
    float result[2][21];
    for (int mode = 0; mode < 2; mode++) {
        glUniform1i(LOC("u_vp_imm_clip"), mode);
        glBeginTransformFeedback(GL_POINTS);
        glDrawArrays(GL_POINTS, 0, 3);
        glEndTransformFeedback();
        glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof result[mode], result[mode]);
        assert(glGetError() == GL_NO_ERROR);
    }
    for (int i = 0; i < 3; i++) {
        float w = i == 0 ? -2 : i == 1 ? 1 : 2;
        const float *r = result[1] + i * 7;
        assert(fabsf(r[0] - (float)(i + 2) * w) < 1e-5f);
        assert(fabsf(r[1] + 4 * w) < 1e-5f);
        assert(r[2] == -w && r[3] == 1);
        assert(r[4] == 1 && r[5] == 1);
    }
    assert(result[0][3] == -2 && result[0][10] == 1); /* legacy clip w */
    assert(result[0][4] < 0 && result[0][19] < 0); /* guest planes reject */
    puts("actual VP GLSL: consistent immersive reconstruction/headset clipping PASS");
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ctx);
    free(src);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='vp-clip-') as temp:
    temp = Path(temp)
    (temp / 'shader.glsl').write_text(shader)
    (temp / 'test.c').write_text(harness)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(temp / 'test.c'),
                    '-framework', 'OpenGL', '-o', str(temp / 'test')], check=True)
    subprocess.run([str(temp / 'test'), str(temp / 'shader.glsl')], check=True)
