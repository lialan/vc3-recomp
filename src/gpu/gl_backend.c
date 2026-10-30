/*
 * OpenGL render back end for the NV2A pushbuffer executor (nv2a_backend.h).
 *
 * The executor's own rasteriser draws every pixel on the CPU, which holds VC3
 * at about 7 fps -- and the game's logic ticks once per frame, so the whole
 * game runs in slow motion. This back end takes the batches the executor has
 * already decoded and transformed and draws them on the GPU instead.
 *
 * It renders off screen: a windowless CGL context (OpenGL 4.1 core), made
 * current on the NV2A thread the executor calls from, so nothing here touches
 * Cocoa or the main thread. Each guest colour surface gets a framebuffer
 * object; at the flip every surface drawn since the last one is read back
 * into guest memory, so the toolkit's window, frame dumps and anything the
 * title reads see the frame exactly as the software path would have left it.
 * A texture whose address is a surface drawn here samples that framebuffer
 * directly (render to texture).
 *
 * Coordinates: vertices arrive in surface pixels. Row 0 of a surface is row 0
 * of its framebuffer, so the readback comes out in guest order without a
 * flip; the picture is upside down only as GL sees it, which nothing notices.
 *
 * ponytail: the interface passes texture stage 0 and the diffuse colour, not
 * the register combiners, so a batch is drawn as texture * diffuse. Culling is
 * off (VC3_GL_CULL=1 enables it) until its winding is checked against the
 * game. Palettised textures are not decoded by nv2a_backend_decode_texture
 * and draw untextured.
 */
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nv2a_backend.h"
#include "../d3d/d3d8_swizzle.h"   /* relative to src/kernel, as the executor */

/* getenv, looked up once per name: several debug switches are tested per
 * draw, and getenv walks the whole environment each time. They do not change
 * while the game runs, and this all runs on the executor thread. */
static const char *genv(const char *name)
{
    static struct { const char *name, *value; } cache[32];
    static int n;
    int i;
    for (i = 0; i < n; i++)
        if (!strcmp(cache[i].name, name))
            return cache[i].value;
    if (n < (int)(sizeof cache / sizeof cache[0])) {
        cache[n].name = name;
        cache[n].value = getenv(name);
        return cache[n++].value;
    }
    return getenv(name);
}

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* ── Context and shaders ───────────────────────────────────── */

static CGLContextObj s_ctx;
static int s_failed;
static GLuint s_prog, s_vao, s_vbo, s_vbo_ext, s_white;
static GLint u_surf, u_depth, u_texsize, u_textured, u_atest, u_afunc, u_aref;
static GLint u_rc, u_cicw, u_aicw, u_cocw, u_aocw, u_f0, u_f1, u_fin0, u_fin1,
             u_ctl, u_sprog, u_fc0, u_fc1, u_fogc, u_on, u_ts;
static uint32_t s_frame = 1;

/* VC3_GL_FTRACE=<flag file>: once the file exists, log every clear and batch
 * of the next frame (0 idle, 1 tracing, 2 done). */
static int s_ftr;

/* VC3_GL_STATS=1: every 300 flips, what the batches asked for. */
static struct {
    int on;
    uint32_t draws, tris, textured, rt, uploads, untex, rt_logged, rc_logged;
    uint32_t zpass_batches, zpass_pixels;
    uint32_t fail[256];        /* failed decodes by format code */
    uint32_t stage0_empty;     /* no stage-0 texture, a later stage has one */
    uint32_t stage0_off;       /* combiners present, stage 0 program off */
    uint32_t stage_mode[32];   /* batches per shader stage program mode */
    uint32_t fmt[256];         /* textured batches by format code */
} s_st;

static const char *VS =
    "#version 410\n"
    "layout(location=0) in vec4 a_pos;\n"     /* x, y, z, rhw */
    "layout(location=1) in vec4 a_col;\n"     /* B, G, R, A bytes */
    "layout(location=2) in vec2 a_uv;\n"      /* texels */
    "layout(location=3) in vec4 a_spec;\n"    /* Nv2aVertexEx: B, G, R, A */
    "layout(location=4) in float a_fog;\n"
    "layout(location=5) in vec4 a_tc0;\n"
    "layout(location=6) in vec4 a_tc1;\n"
    "layout(location=7) in vec4 a_tc2;\n"
    "layout(location=8) in vec4 a_tc3;\n"
    "uniform vec2 u_surf;\n"                  /* framebuffer size */
    "uniform vec2 u_depth;\n"                 /* clip min, max */
    "uniform vec2 u_texsize;\n"
    "out vec4 v_col;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_spec;\n"
    "out float v_fog;\n"
    "out vec4 v_tc0, v_tc1, v_tc2, v_tc3;\n"
    "void main() {\n"
    "  float w = (a_pos.w > 0.0 && a_pos.w < 1e30) ? 1.0 / a_pos.w : 1.0;\n"
    "  vec2 ndc = a_pos.xy / u_surf * 2.0 - 1.0;\n"
    "  float z = clamp((a_pos.z - u_depth.x) / (u_depth.y - u_depth.x), 0.0, 1.0) * 2.0 - 1.0;\n"
    "  gl_Position = vec4(ndc * w, z * w, w);\n"
    "  v_col = a_col.zyxw;\n"
    "  v_uv = a_uv / u_texsize;\n"
    "  v_spec = a_spec.zyxw;\n"
    "  v_fog = a_fog;\n"
    "  v_tc0 = a_tc0; v_tc1 = a_tc1; v_tc2 = a_tc2; v_tc3 = a_tc3;\n"
    "}\n";

/* The register combiners: xboxrecomp's nv2a_combiner.c (which follows xemu's
 * psh.c) transcribed to GLSL, its state in uniforms. Kept line for line with
 * the C so a difference between the two is a bug in one of them. */
static const char *FS =
    "#version 410\n"
    "in vec4 v_col;\n"
    "in vec2 v_uv;\n"
    "in vec4 v_spec;\n"
    "in float v_fog;\n"
    "in vec4 v_tc0, v_tc1, v_tc2, v_tc3;\n"
    "uniform sampler2D u_tex;\n"
    "uniform sampler2D u_t1, u_t2, u_t3;\n"   /* stages 1-3; stage 0 is u_tex */
    "uniform samplerCube u_c0, u_c1, u_c2, u_c3;\n"  /* cube-map stages, units 4-7 */
    "uniform int u_iscube[4];\n"
    "uniform int u_textured;\n"
    "uniform int u_atest;\n"
    "uniform int u_afunc;\n"                  /* GL_NEVER 0x200 .. GL_ALWAYS 0x207 */
    "uniform float u_aref;\n"
    "uniform int u_rc;\n"
    "uniform int u_lmap;\n"                   /* texture * diffuse * stage-1 lightmap */                     /* evaluate the combiners */
    "uniform uint u_cicw[8], u_aicw[8], u_cocw[8], u_aocw[8];\n"
    "uniform vec4 u_f0[8], u_f1[8];\n"
    "uniform uint u_fin0, u_fin1, u_ctl, u_sprog;\n"
    "uniform vec4 u_fc0, u_fc1;\n"
    "uniform vec3 u_fogc;\n"
    "uniform int u_nofog;\n"                  /* VC3_GL_NOFOG=1: debug */
    "uniform int u_rcdbg;\n"                  /* VC3_GL_RCDBG: 1 texel, 2 uv, 3 split */
    "uniform int u_adbg;\n"                   /* VC3_GL_ADBG: force an alpha input to 1 */
    "uniform vec3 u_dbgcol;\n"
    "uniform int u_dbgon;\n"                  /* debug views skip composites */                /* RCDBG 6: colour per combiner setup */
    "uniform vec2 u_surf;\n"
    "uniform int u_on[4];\n"                  /* stage has a usable texture */
    "uniform vec2 u_ts[4];\n"                 /* coordinate -> GL [0,1] scale */
    "out vec4 o_col;\n"
    "vec4 R[16];\n"
    "float map_in(float x, uint b) {\n"
    "  float p = max(x, 0.0);\n"
    "  uint m = (b >> 5) & 7u;\n"
    "  if (m == 0u) return p;\n"
    "  if (m == 1u) return 1.0 - clamp(x, 0.0, 1.0);\n"
    "  if (m == 2u) return 2.0 * p - 1.0;\n"
    "  if (m == 3u) return -2.0 * p + 1.0;\n"
    "  if (m == 4u) return p - 0.5;\n"
    "  if (m == 5u) return -p + 0.5;\n"
    "  if (m == 6u) return x;\n"
    "  return -x;\n"
    "}\n"
    "float map_out(float x, uint m) {\n"
    "  if (m == 0x08u) return x - 0.5;\n"
    "  if (m == 0x10u) return x * 2.0;\n"
    "  if (m == 0x18u) return (x - 0.5) * 2.0;\n"
    "  if (m == 0x20u) return x * 4.0;\n"
    "  if (m == 0x30u) return x * 0.5;\n"
    "  return x;\n"
    "}\n"
    "vec3 rd_rgb(uint b) {\n"
    "  vec4 r = R[b & 15u];\n"
    "  vec3 v = ((b & 0x10u) != 0u) ? vec3(r.a) : r.rgb;\n"
    "  return vec3(map_in(v.x, b), map_in(v.y, b), map_in(v.z, b));\n"
    "}\n"
    "float rd_a(uint b) {\n"
    "  vec4 r = R[b & 15u];\n"
    "  return map_in(((b & 0x10u) != 0u) ? r.a : r.b, b);\n"
    "}\n"
    /* What texture stage st contributes, by its SHADER_STAGE_PROGRAM mode
     * (rc_stage_fetch). An unusable stage reads white, like rc_texel. */
    "vec4 stage(int st, vec4 tc, vec4 texel) {\n"
    "  uint mode = (u_sprog >> uint(st * 5)) & 31u;\n"
    "  if (mode == 0u) return vec4(0.0, 0.0, 0.0, 1.0);\n"
    "  if (mode == 4u) return clamp(tc, 0.0, 1.0);\n"
    "  if (mode <= 3u) return u_on[st] != 0 ? texel : vec4(1.0);\n"
    "  return vec4(0.0);\n"
    "}\n"
    "vec2 stage_uv(int st, vec4 tc) {\n"
    "  uint mode = (u_sprog >> uint(st * 5)) & 31u;\n"
    "  float q = tc.w != 0.0 ? tc.w : 1.0;\n"
    "  return (mode == 3u ? tc.xy : tc.xy / q) * u_ts[st];\n"
    "}\n"
    "vec4 combiners() {\n"
    "  vec4 t0 = stage(0, v_tc0, u_iscube[0] != 0 ? texture(u_c0, v_tc0.xyz) : texture(u_tex, stage_uv(0, v_tc0)));\n"
    "  vec4 t1 = stage(1, v_tc1, u_iscube[1] != 0 ? texture(u_c1, v_tc1.xyz) : texture(u_t1, stage_uv(1, v_tc1)));\n"
    "  vec4 t2 = stage(2, v_tc2, u_iscube[2] != 0 ? texture(u_c2, v_tc2.xyz) : texture(u_t2, stage_uv(2, v_tc2)));\n"
    "  vec4 t3 = stage(3, v_tc3, u_iscube[3] != 0 ? texture(u_c3, v_tc3.xyz) : texture(u_t3, stage_uv(3, v_tc3)));\n"
    "  uint n = min(u_ctl & 0xFFu, 8u), flags = u_ctl >> 8;\n"
    "  for (int i = 0; i < 16; i++) R[i] = vec4(0.0);\n"
    "  R[3] = vec4(u_fogc, u_nofog != 0 ? 1.0 : v_fog);\n"
    "  if (u_adbg == 1) t1.a = 1.0;\n"
    "  if (u_adbg == 2) t0.a = 1.0;\n"
    "  R[4] = v_col; R[5] = v_spec;\n"
    "  if (u_adbg == 3) R[4].a = 1.0;\n"
    "  R[8] = t0; R[9] = t1; R[10] = t2; R[11] = t3;\n"
    "  R[12].a = ((u_sprog & 31u) != 0u) ? t0.a : 1.0;\n"
    "  for (uint s = 0u; s < 8u; s++) {\n"
    "    if (s >= n) break;\n"
    "    uint icw = u_cicw[s], ocw = u_cocw[s], aicw = u_aicw[s], aocw = u_aocw[s];\n"
    "    uint fl = ocw >> 12, afl = aocw >> 12;\n"
    "    R[1] = u_f0[(flags & 0x010u) != 0u ? s : 0u];\n"
    "    R[2] = u_f1[(flags & 0x100u) != 0u ? s : 0u];\n"
    "    bool mux_cd = (flags & 1u) != 0u ? R[12].a >= 0.5\n"
    "                                      : ((int(R[12].a * 255.0) & 1) != 0);\n"
    "    vec3 A = rd_rgb(icw >> 24), B = rd_rgb(icw >> 16);\n"
    "    vec3 C = rd_rgb(icw >> 8),  D = rd_rgb(icw);\n"
    "    float aA = rd_a(aicw >> 24), aB = rd_a(aicw >> 16);\n"
    "    float aC = rd_a(aicw >> 8),  aD = rd_a(aicw);\n"
    "    vec3 pab = ((fl & 2u) != 0u) ? vec3(dot(A, B)) : A * B;\n"
    "    vec3 pcd = ((fl & 1u) != 0u) ? vec3(dot(C, D)) : C * D;\n"
    "    vec3 sum = ((fl & 4u) != 0u) ? (mux_cd ? pcd : pab) : pab + pcd;\n"
    "    uint om = fl & 0x38u;\n"
    "    vec3 ab = clamp(vec3(map_out(pab.x, om), map_out(pab.y, om), map_out(pab.z, om)), -1.0, 1.0);\n"
    "    vec3 cd = clamp(vec3(map_out(pcd.x, om), map_out(pcd.y, om), map_out(pcd.z, om)), -1.0, 1.0);\n"
    "    vec3 ms = clamp(vec3(map_out(sum.x, om), map_out(sum.y, om), map_out(sum.z, om)), -1.0, 1.0);\n"
    "    float apab = aA * aB, apcd = aC * aD;\n"
    "    float asum = ((afl & 4u) != 0u) ? (mux_cd ? apcd : apab) : apab + apcd;\n"
    "    uint aom = afl & 0x38u;\n"
    "    float aab = clamp(map_out(apab, aom), -1.0, 1.0);\n"
    "    float acd = clamp(map_out(apcd, aom), -1.0, 1.0);\n"
    "    float ams = clamp(map_out(asum, aom), -1.0, 1.0);\n"
    "    uint d;\n"
    "    d = (ocw >> 4) & 15u;\n"
    "    if (d != 0u) { R[d].rgb = ab; if ((fl & 0x80u) != 0u) R[d].a = ab.z; }\n"
    "    d = ocw & 15u;\n"
    "    if (d != 0u) { R[d].rgb = cd; if ((fl & 0x40u) != 0u) R[d].a = cd.z; }\n"
    "    d = (ocw >> 8) & 15u;\n"
    "    if (d != 0u) R[d].rgb = ms;\n"
    "    d = (aocw >> 4) & 15u; if (d != 0u) R[d].a = aab;\n"
    "    d = aocw & 15u;        if (d != 0u) R[d].a = acd;\n"
    "    d = (aocw >> 8) & 15u; if (d != 0u) R[d].a = ams;\n"
    "    R[0] = vec4(0.0);\n"
    "  }\n"
    "  vec4 o;\n"
    "  if (u_fin0 != 0u || u_fin1 != 0u) {\n"
    "    uint ff = u_fin1 & 0xFFu;\n"
    "    R[1] = u_fc0; R[2] = u_fc1;\n"
    "    vec3 a = ((ff & 0x40u) != 0u) ? 1.0 - R[5].rgb : R[5].rgb;\n"
    "    vec3 b = ((ff & 0x20u) != 0u) ? 1.0 - R[12].rgb : R[12].rgb;\n"
    "    R[14] = vec4(((ff & 0x80u) != 0u) ? clamp(a + b, 0.0, 1.0) : a + b, 0.0);\n"
    "    vec3 E = rd_rgb(u_fin1 >> 24), F = rd_rgb(u_fin1 >> 16);\n"
    "    R[15] = vec4(E * F, 0.0);\n"
    "    vec3 A = rd_rgb(u_fin0 >> 24), B = rd_rgb(u_fin0 >> 16);\n"
    "    vec3 C = rd_rgb(u_fin0 >> 8),  D = rd_rgb(u_fin0);\n"
    "    o = vec4(D + C * (1.0 - A) + B * A, rd_a(u_fin1 >> 8));\n"
    "  } else {\n"
    "    o = R[12];\n"
    "  }\n"
    "  return clamp(o, 0.0, 1.0);\n"
    "}\n"
    "void main() {\n"
    "  vec4 c = v_col;\n"
    "  if (u_rc != 0) c = combiners();\n"
    "  if (u_rc != 0 && u_dbgon != 0 && u_rcdbg == 1) c = vec4(texture(u_tex, stage_uv(0, v_tc0)).rgb, 1.0);\n"
    "  if (u_rc != 0 && u_dbgon != 0 && u_rcdbg == 2) c = vec4(fract(stage_uv(0, v_tc0)), float(u_on[0]), 1.0);\n"
    "  if (u_rc != 0 && u_dbgon != 0 && u_rcdbg == 6) c = vec4(u_dbgcol, 1.0);\n"
    "  if (u_rc != 0 && u_dbgon != 0 && u_rcdbg == 7) c = vec4(vec3(v_fog), 1.0);\n"
    "  if (u_rc != 0 && u_dbgon != 0 && u_rcdbg == 8) c = vec4(texture(u_t1, stage_uv(1, v_tc1)).rgb, 1.0);\n"
    "  if (u_rc != 0 && u_dbgon != 0 && u_rcdbg == 9) c = vec4(vec3(texture(u_t1, stage_uv(1, v_tc1)).a), 1.0);\n"
    "  if (u_rc != 0 && u_dbgon != 0 && u_rcdbg == 10) c = vec4(vec3(combiners().a), 1.0);\n"
    "  if (u_rc != 0 && u_dbgon != 0 && u_rcdbg == 12) c = vec4(combiners().a, texture(u_tex, stage_uv(0, v_tc0)).a, texture(u_t1, stage_uv(1, v_tc1)).a, 1.0);\n"
    "  if (u_rc != 0 && u_dbgon != 0 && u_rcdbg == 13) c = vec4(v_col.a, float(u_on[0]), float(u_on[1]), 1.0);\n"
    "  if (u_rc != 0 && u_dbgon != 0 && u_rcdbg == 3 && gl_FragCoord.x < u_surf.x * 0.5)\n"
    "    c = v_col * (u_textured != 0 ? texture(u_tex, v_uv) : vec4(1.0));\n"
    "  else if (u_textured != 0) {\n"
    "    c *= texture(u_tex, v_uv);\n"
    "    if (u_lmap != 0) c.rgb *= texture(u_t1, stage_uv(1, v_tc1)).rgb;\n"
    "  }\n"
    "  if (u_atest != 0) {\n"
    /* In 8 bits, as the hardware and the software rasteriser compare: a
     * modulated 0x80 lands at 0.5, below 128/255 as a float. */
    "    float a = floor(clamp(c.a, 0.0, 1.0) * 255.0 + 0.5); bool pass = true;\n"
    "    int f = u_afunc - 0x200;\n"
    "    if (f == 0) pass = false;\n"
    "    else if (f == 1) pass = a <  u_aref;\n"
    "    else if (f == 2) pass = a == u_aref;\n"
    "    else if (f == 3) pass = a <= u_aref;\n"
    "    else if (f == 4) pass = a >  u_aref;\n"
    "    else if (f == 5) pass = a != u_aref;\n"
    "    else if (f == 6) pass = a >= u_aref;\n"
    "    if (!pass) discard;\n"
    "  }\n"
    "  o_col = c;\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    char log[1024];

    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[GL] shader: %s\n", log);
    }
    return s;
}

/* Lazily, on the thread the executor calls from; every entry point makes the
 * context current in case that is ever a different thread. */
static int gl_ready(void)
{
    if (s_failed)
        return 0;
    if (s_ctx) {
        if (CGLGetCurrentContext() != s_ctx)
            CGLSetCurrentContext(s_ctx);
        return 1;
    }
    {
        CGLPixelFormatAttribute attrs[] = {
            kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
            kCGLPFAAccelerated, kCGLPFAAllowOfflineRenderers,
            (CGLPixelFormatAttribute)0
        };
        CGLPixelFormatObj pf = NULL;
        GLint npf = 0;
        GLint ok = 0;
        GLuint vs, fs;
        uint32_t white = 0xFFFFFFFFu;

        if (CGLChoosePixelFormat(attrs, &pf, &npf) != kCGLNoError || !pf
            || CGLCreateContext(pf, NULL, &s_ctx) != kCGLNoError) {
            fprintf(stderr, "[GL] no OpenGL 4.1 context; back end off\n");
            s_failed = 1;
            return 0;
        }
        CGLDestroyPixelFormat(pf);
        CGLSetCurrentContext(s_ctx);
        fprintf(stderr, "[GL] %s, %s\n", glGetString(GL_RENDERER),
                glGetString(GL_VERSION));

        vs = compile(GL_VERTEX_SHADER, VS);
        fs = compile(GL_FRAGMENT_SHADER, FS);
        s_prog = glCreateProgram();
        glAttachShader(s_prog, vs);
        glAttachShader(s_prog, fs);
        glLinkProgram(s_prog);
        glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            fprintf(stderr, "[GL] program link failed; back end off\n");
            s_failed = 1;
            return 0;
        }
        glUseProgram(s_prog);
        u_surf = glGetUniformLocation(s_prog, "u_surf");
        u_depth = glGetUniformLocation(s_prog, "u_depth");
        u_texsize = glGetUniformLocation(s_prog, "u_texsize");
        u_textured = glGetUniformLocation(s_prog, "u_textured");
        u_atest = glGetUniformLocation(s_prog, "u_atest");
        u_afunc = glGetUniformLocation(s_prog, "u_afunc");
        u_aref = glGetUniformLocation(s_prog, "u_aref");
        glUniform1i(glGetUniformLocation(s_prog, "u_tex"), 0);
        glUniform1i(glGetUniformLocation(s_prog, "u_t1"), 1);
        glUniform1i(glGetUniformLocation(s_prog, "u_t2"), 2);
        glUniform1i(glGetUniformLocation(s_prog, "u_t3"), 3);
        glUniform1i(glGetUniformLocation(s_prog, "u_c0"), 4);
        glUniform1i(glGetUniformLocation(s_prog, "u_c1"), 5);
        glUniform1i(glGetUniformLocation(s_prog, "u_c2"), 6);
        glUniform1i(glGetUniformLocation(s_prog, "u_c3"), 7);
        glUniform1i(glGetUniformLocation(s_prog, "u_adbg"),
                    genv("VC3_GL_ADBG") ? atoi(genv("VC3_GL_ADBG")) : 0);
        glUniform1i(glGetUniformLocation(s_prog, "u_nofog"),
                    genv("VC3_GL_NOFOG") != NULL);
        glUniform1i(glGetUniformLocation(s_prog, "u_rcdbg"),
                    genv("VC3_GL_RCDBG") ? atoi(genv("VC3_GL_RCDBG")) : 0);
        u_rc = glGetUniformLocation(s_prog, "u_rc");
        u_cicw = glGetUniformLocation(s_prog, "u_cicw");
        u_aicw = glGetUniformLocation(s_prog, "u_aicw");
        u_cocw = glGetUniformLocation(s_prog, "u_cocw");
        u_aocw = glGetUniformLocation(s_prog, "u_aocw");
        u_f0 = glGetUniformLocation(s_prog, "u_f0");
        u_f1 = glGetUniformLocation(s_prog, "u_f1");
        u_fin0 = glGetUniformLocation(s_prog, "u_fin0");
        u_fin1 = glGetUniformLocation(s_prog, "u_fin1");
        u_ctl = glGetUniformLocation(s_prog, "u_ctl");
        u_sprog = glGetUniformLocation(s_prog, "u_sprog");
        u_fc0 = glGetUniformLocation(s_prog, "u_fc0");
        u_fc1 = glGetUniformLocation(s_prog, "u_fc1");
        u_fogc = glGetUniformLocation(s_prog, "u_fogc");
        u_on = glGetUniformLocation(s_prog, "u_on");
        u_ts = glGetUniformLocation(s_prog, "u_ts");

        glGenVertexArrays(1, &s_vao);
        glBindVertexArray(s_vao);
        glGenBuffers(1, &s_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Nv2aVertex),
                              (void *)offsetof(Nv2aVertex, x));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Nv2aVertex),
                              (void *)offsetof(Nv2aVertex, diffuse));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Nv2aVertex),
                              (void *)offsetof(Nv2aVertex, u));
        /* Combiner inputs, enabled only for batches that carry them. */
        glGenBuffers(1, &s_vbo_ext);
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo_ext);
        glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Nv2aVertexEx),
                              (void *)offsetof(Nv2aVertexEx, specular));
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(Nv2aVertexEx),
                              (void *)offsetof(Nv2aVertexEx, fog));
        {
            GLuint k;
            for (k = 0; k < 4; k++)
                glVertexAttribPointer(5 + k, 4, GL_FLOAT, GL_FALSE,
                                      sizeof(Nv2aVertexEx),
                                      (void *)(offsetof(Nv2aVertexEx, tex)
                                               + k * 4 * sizeof(float)));
        }
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo);

        glGenTextures(1, &s_white);
        glBindTexture(GL_TEXTURE_2D, s_white);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA,
                     GL_UNSIGNED_INT_8_8_8_8_REV, &white);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
    }
    return 1;
}

/* ── Surfaces ──────────────────────────────────────────────── */

#define MAX_SURFS 32
typedef struct {
    uint32_t va, pitch, bpp;
    GLsizei w, h;
    GLuint fbo, color, depth;
    int dirty;                 /* drawn since the last readback */
    /* Guest memory as the GPU last left it, to spot the CPU writing the
     * surface: VC3's movie decoder writes frames straight into a surface the
     * menu once cleared, so the GPU copy stays black. */
    int memq_valid, cpu_owned;
    uint64_t memq;
} Surf;
static Surf s_surfs[MAX_SURFS];
static int s_nsurfs;

static void surf_alloc(Surf *f, GLsizei w, GLsizei h)
{
    if (f->fbo) {
        glDeleteFramebuffers(1, &f->fbo);
        glDeleteTextures(1, &f->color);
        glDeleteRenderbuffers(1, &f->depth);
    }
    f->w = w;
    f->h = h;
    glGenTextures(1, &f->color);
    glBindTexture(GL_TEXTURE_2D, f->color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA,
                 GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenRenderbuffers(1, &f->depth);
    glBindRenderbuffer(GL_RENDERBUFFER, f->depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glGenFramebuffers(1, &f->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, f->color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, f->depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "[GL] surface %08X %dx%d incomplete\n", f->va, w, h);
}

static Surf *surf_find(uint32_t va)
{
    int i;
    for (i = 0; i < s_nsurfs; i++)
        if (s_surfs[i].va == va)
            return &s_surfs[i];
    return NULL;
}

/* The framebuffer for a guest surface, created or grown to fit, and bound. */
static Surf *surf_bind(const Nv2aSurface *s)
{
    Surf *f = surf_find(s->color_va);
    uint32_t bpp = s->bytes_per_pixel ? s->bytes_per_pixel : 4;
    GLsizei w = (GLsizei)(s->pitch / bpp), h = (GLsizei)s->height;

    if (w <= 0 || h <= 0)
        return NULL;
    if (!f) {
        if (s_nsurfs == MAX_SURFS) {
            fprintf(stderr, "[GL] more than %d surfaces\n", MAX_SURFS);
            return NULL;
        }
        f = &s_surfs[s_nsurfs++];
        memset(f, 0, sizeof *f);
        f->va = s->color_va;
    }
    if (!f->fbo || w > f->w || h > f->h)
        surf_alloc(f, w > f->w ? w : f->w, h > f->h ? h : f->h);
    f->pitch = s->pitch;
    f->bpp = bpp;
    glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
    glViewport(0, 0, f->w, f->h);
    return f;
}

/* Write a surface back into guest memory, row 0 first. */
static void surf_readback(Surf *f)
{
    uint8_t *dst = (uint8_t *)xbox_GetMemoryOffset() + f->va;
    GLsizei rows = f->h;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, f->fbo);
    glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)(f->pitch / f->bpp));
    if (f->bpp == 2)
        glReadPixels(0, 0, f->w, rows, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, dst);
    else
        glReadPixels(0, 0, f->w, rows, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, dst);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    f->dirty = 0;
    f->memq_valid = 0;
}

/* The presented surface, read back without waiting: each flip starts a read
 * into one pixel-pack buffer and copies the other -- started a flip earlier,
 * long finished -- into guest memory. A synchronous glReadPixels waited for
 * everything queued on the GPU, the window's vsync'd present included, so the
 * title waited on the display; this shows frames one flip late instead. */
typedef struct {
    GLuint pbo;
    GLsizeiptr cap;
    uint32_t va, bytes;          /* where and how much, 0 when none pending */
} Readback;

static Readback s_rb[2];
static int s_rb_next;

static void surf_readback_async(Surf *f)
{
    Readback *r = &s_rb[s_rb_next], *prev = &s_rb[s_rb_next ^ 1];
    GLsizeiptr bytes = (GLsizeiptr)f->pitch * f->h;

    if (!r->pbo)
        glGenBuffers(1, &r->pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, r->pbo);
    if (r->cap < bytes) {
        glBufferData(GL_PIXEL_PACK_BUFFER, bytes, NULL, GL_STREAM_READ);
        r->cap = bytes;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, f->fbo);
    glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)(f->pitch / f->bpp));
    if (f->bpp == 2)
        glReadPixels(0, 0, f->w, f->h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
    else
        glReadPixels(0, 0, f->w, f->h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    r->va = f->va;
    r->bytes = (uint32_t)bytes;
    f->dirty = 0;

    if (prev->pbo && prev->bytes) {
        const void *src;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, prev->pbo);
        src = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, prev->bytes, GL_MAP_READ_BIT);
        if (src) {
            Surf *pf = surf_find(prev->va);
            memcpy((uint8_t *)xbox_GetMemoryOffset() + prev->va, src, prev->bytes);
            if (pf)
                pf->memq_valid = 0;
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        prev->bytes = 0;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    s_rb_next ^= 1;
}

/* ── Textures ──────────────────────────────────────────────── */

#define TEX_SLOTS 2048
typedef struct {
    uint32_t offset, w, h, fmt, pitch;
    uint64_t hash;
    uint64_t quick;            /* sampled hash, checked on every bind */
    uint32_t checked;          /* frame of the last content check */
    uint32_t used;             /* frame of the last use */
    GLuint tex;
} TexEntry;
static TexEntry s_tex[TEX_SLOTS];
static uint32_t *s_decode;
static size_t s_decode_cap;

/* Bytes a texture's level 0 occupies, for the change check. */
static size_t tex_bytes(const Nv2aTexture *t)
{
    uint32_t bb = d3d8_format_dxt_block_bytes(t->color);
    size_t bpp;

    if (bb)
        return (size_t)((t->width + 3) / 4) * ((t->height + 3) / 4) * bb;
    switch (t->color) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1F:
        bpp = 1;
        break;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11:
    case 0x1A: case 0x1C: case 0x1D: case 0x24: case 0x25:
        bpp = 2;
        break;
    default:
        bpp = 4;
        break;
    }
    if (t->pitch)
        return (size_t)t->pitch * t->height;
    return (size_t)t->width * t->height * bpp;
}

static uint64_t hash_bytes(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i;

    for (i = 0; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

/* Bytes of one w x h level of a swizzled or DXT texture. */
static size_t level_bytes(uint32_t color, uint32_t w, uint32_t h)
{
    uint32_t bb = d3d8_format_dxt_block_bytes(color);
    if (bb)
        return (size_t)((w + 3) / 4) * ((h + 3) / 4) * bb;
    switch (color) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1F:
        return (size_t)w * h;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11:
    case 0x1A: case 0x1C: case 0x1D: case 0x24: case 0x25:
        return (size_t)w * h * 2;
    default:
        return (size_t)w * h * 4;
    }
}

/* One cube face: its whole mip chain, padded to 128 bytes. */
static size_t cube_face_bytes(const Nv2aTexture *t)
{
    uint32_t w = t->width, h = t->height, l, levels = t->levels ? t->levels : 1;
    size_t n = 0;
    for (l = 0; l < levels; l++) {
        n += level_bytes(t->color, w, h);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return (n + 127) & ~(size_t)127;
}

/* 64 slices of 16 bytes spread over the texture: cheap enough for every
 * bind. VC3 streams textures through the same memory within a frame -- a
 * face, then a jacket of the same size and format at one address -- and a
 * once-per-frame check drew the face with the jacket. */
static uint64_t hash_sampled(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    size_t k;

    if (n <= 64 * 16)
        return hash_bytes(p, n);
    for (k = 0; k < 64; k++) {
        size_t at = (n - 16) / 63 * k;
        h ^= hash_bytes(p + at, 16) + k;
        h *= 0x100000001b3ull;
    }
    return h;
}

/* Formats with no alpha channel read alpha as 1. A render target sampled
 * through one -- VC3 composites its X8R8G8B8 3D frame onto the screen with
 * source-alpha blending -- must too, whatever alpha the passes left in the
 * framebuffer, or the blend keeps the clear colour and the world vanishes. */
static int fmt_has_alpha(uint32_t fmt)
{
    switch (fmt) {
    case 0x03: case 0x05: case 0x07:     /* SZ_X1R5G5B5, SZ_R5G6B5, SZ_X8R8G8B8 */
    case 0x11: case 0x1C: case 0x1E:     /* LIN_R5G6B5, LIN_X1R5G5B5, LIN_X8R8G8B8 */
        return 0;
    default:
        return 1;
    }
}

static GLint gl_wrap(uint32_t mode)
{
    switch (mode) {
    case 2:  return GL_MIRRORED_REPEAT;
    /* 4 is BORDER, but the border colour applies only when the format word
     * asks for it (Texture.border_color, not passed to back ends); otherwise
     * the hardware clamps, as the software sampler's wrap_coord does. A
     * transparent GL border zeroed lightmap alpha, and VC3's alpha test then
     * threw away every lightmapped wall. */
    case 3:
    case 4:  return GL_CLAMP_TO_EDGE;
    default: return GL_REPEAT;
    }
}

/* Cube maps, cached apart from the 2D textures and checked the same way
 * (sampled hash per bind, full hash on a change or a new frame). */
#define CUBE_SLOTS 256
typedef struct {
    uint32_t offset, w, fmt, levels;
    uint64_t hash, quick;
    uint32_t checked;
    GLuint tex;
} CubeEntry;
static CubeEntry s_cube[CUBE_SLOTS];

/* Bind cube map t to the active unit; 0 if it cannot be decoded. */
static int tex_bind_cube(const Nv2aTexture *t)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    size_t face = cube_face_bytes(t), need = (size_t)t->width * t->height;
    CubeEntry *e = &s_cube[((t->offset >> 7) ^ (t->width * 31u)) % CUBE_SLOTS];
    uint64_t q;
    int f;

    if (!t->width || t->width != t->height || t->width > 2048)
        return 0;
    if (!e->tex || e->offset != t->offset || e->w != t->width || e->fmt != t->color
        || e->levels != t->levels) {
        if (!e->tex)
            glGenTextures(1, &e->tex);
        e->offset = t->offset;
        e->w = t->width;
        e->fmt = t->color;
        e->levels = t->levels;
        e->hash = e->quick = 0;
        e->checked = 0;
    }
    glBindTexture(GL_TEXTURE_CUBE_MAP, e->tex);
    q = hash_sampled(mem + t->offset, face * 6);
    if (e->checked != s_frame || q != e->quick) {
        uint64_t h = hash_bytes(mem + t->offset, face * 6);
        e->checked = s_frame;
        e->quick = q;
        if (h != e->hash || !e->hash) {
            if (need > s_decode_cap) {
                free(s_decode);
                s_decode = (uint32_t *)malloc(need * 4);
                s_decode_cap = s_decode ? need : 0;
            }
            if (!s_decode)
                return 0;
            for (f = 0; f < 6; f++) {
                Nv2aTexture tf = *t;
                tf.offset = t->offset + (uint32_t)(face * (size_t)f);
                tf.cube = 0;
                tf.levels = 1;
                if (!nv2a_backend_decode_texture(&tf, s_decode)) {
                    s_st.fail[t->color & 0xFF]++;
                    e->hash = 0;
                    return 0;
                }
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8,
                             (GLsizei)t->width, (GLsizei)t->height, 0, GL_BGRA,
                             GL_UNSIGNED_INT_8_8_8_8_REV, s_decode);
            }
            s_st.uploads++;
            e->hash = h;
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        }
    }
    return 1;
}

/* Bind the texture a batch samples to unit 0; returns 0 to draw untextured.
 * *sw, *sh receive the size its texel coordinates are relative to. */
static int tex_bind(const Nv2aTexture *t, float *sw, float *sh)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    Surf *rt = surf_find(t->offset);
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    size_t slot, i;
    TexEntry *e = NULL;

    s_st.fmt[t->color & 0xFF]++;
    if (rt && rt->fbo && !rt->cpu_owned) {
        /* Memory changed since the GPU last wrote it: the CPU owns it now. */
        size_t n = (size_t)rt->pitch * (size_t)rt->h;
        uint64_t q = hash_sampled(mem + rt->va, n);
        if (!rt->memq_valid) {
            rt->memq = q;
            rt->memq_valid = 1;
        } else if (q != rt->memq) {
            rt->cpu_owned = 1;
        }
    }
    if (rt && rt->fbo && !rt->cpu_owned) {
        s_st.rt++;
        glBindTexture(GL_TEXTURE_2D, rt->color);
        *sw = (float)rt->w;
        *sh = (float)rt->h;
    } else {
        if (!t->width || !t->height || t->width > 4096 || t->height > 4096)
            return 0;
        slot = ((t->offset >> 4) ^ (t->width * 31u) ^ (t->color * 131u)) % TEX_SLOTS;
        for (i = 0; i < 8; i++) {
            TexEntry *c = &s_tex[(slot + i) % TEX_SLOTS];
            if (c->tex && c->offset == t->offset && c->w == t->width
                && c->h == t->height && c->fmt == t->color && c->pitch == t->pitch) {
                e = c;
                break;
            }
        }
        if (!e) {
            /* A free slot, or the least recently used of the eight. */
            TexEntry *victim = &s_tex[slot];
            for (i = 0; i < 8; i++) {
                TexEntry *c = &s_tex[(slot + i) % TEX_SLOTS];
                if (!c->tex) { victim = c; break; }
                if (c->used < victim->used) victim = c;
            }
            e = victim;
            if (!e->tex)
                glGenTextures(1, &e->tex);
            e->offset = t->offset;
            e->w = t->width;
            e->h = t->height;
            e->fmt = t->color;
            e->pitch = t->pitch;
            e->hash = 0;
            e->quick = 0;
            e->checked = 0;
        }
        glBindTexture(GL_TEXTURE_2D, e->tex);
        /* Full check once a frame; within a frame, whenever the sampled hash
         * says the memory now holds something else. */
        uint64_t q = hash_sampled(mem + t->offset, tex_bytes(t));
        if (e->checked != s_frame || q != e->quick) {
            uint64_t h = hash_bytes(mem + t->offset, tex_bytes(t));
            e->checked = s_frame;
            e->quick = q;
            if (h != e->hash || !e->hash) {
                size_t need = (size_t)t->width * t->height;
                if (need > s_decode_cap) {
                    free(s_decode);
                    s_decode = (uint32_t *)malloc(need * 4);
                    s_decode_cap = s_decode ? need : 0;
                }
                if (!s_decode || !nv2a_backend_decode_texture(t, s_decode)) {
                    s_st.fail[t->color & 0xFF]++;
                    e->hash = 0;
                    return 0;
                }
                s_st.uploads++;
                /* VC3_GL_TEXDUMP=<hex offset>[,<hex offset>...]: write those
                 * textures as the back end decoded them (colour and alpha PPMs
                 * in the working directory), to tell a bad decode from bad
                 * shading. */
                char want[16];
                snprintf(want, sizeof want, "%08X", t->offset);
                if (s_ftr == 1)
                    fprintf(stderr, "[GLTR]   upload %08X %ux%u fmt %02X\n", t->offset,
                            t->width, t->height, t->color & 0xFF);
                if (genv("VC3_GL_TEXDUMP") && strstr(genv("VC3_GL_TEXDUMP"), want)) {
                    char name[64];
                    FILE *o;
                    size_t n = (size_t)t->width * t->height, p;
                    int a;
                    for (a = 0; a < 2; a++) {
                        /* In the traced frame, a separate name: what the
                         * frame actually drew with, not the run's last. */
                        snprintf(name, sizeof name, "texdump_%08X%s%s.ppm", t->offset,
                                 s_ftr == 1 ? "_ftr" : "", a ? "_alpha" : "");
                        if (!(o = fopen(name, "wb")))
                            continue;
                        fprintf(o, "P6\n%u %u\n255\n", t->width, t->height);
                        for (p = 0; p < n; p++) {
                            uint32_t c = s_decode[p];
                            unsigned char px[3] = { (unsigned char)(c >> 16),
                                                    (unsigned char)(c >> 8),
                                                    (unsigned char)c };
                            if (a)
                                px[0] = px[1] = px[2] = (unsigned char)(c >> 24);
                            fwrite(px, 1, 3, o);
                        }
                        fclose(o);
                    }
                    fprintf(stderr, "[GL] dumped texture %08X %ux%u fmt %02X pitch %u\n",
                            t->offset, t->width, t->height, t->color & 0xFF, t->pitch);
                }
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)t->width,
                             (GLsizei)t->height, 0, GL_BGRA,
                             GL_UNSIGNED_INT_8_8_8_8_REV, s_decode);
                e->hash = h;
            }
        }
        e->used = s_frame;
        *sw = (float)t->width;
        *sh = (float)t->height;
    }
    /* SET_TEXTURE_FILTER: 1 is point sampling, anything else filters. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    mag == 1 ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    min == 1 ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_wrap(t->addr_u));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wrap(t->addr_v));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A,
                    fmt_has_alpha(t->color) ? GL_ALPHA : GL_ONE);
    return 1;
}

/* ── State ─────────────────────────────────────────────────── */

/* A batch that samples the surface it draws into: VC3's full-screen tint
 * and blur passes redraw the frame through itself. The NV2A reads what is
 * there; GL forbids sampling a bound attachment, so take a snapshot first
 * and sample that. Left as white, each pass flattened the whole picture to
 * its modulation colour. */
static GLuint s_self_tex;
static GLsizei s_self_w, s_self_h;

static int tex_bind_for(const Surf *f, const Nv2aTexture *t, float *sw, float *sh)
{
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;

    if (t->offset != f->va)
        return tex_bind(t, sw, sh);
    if (!s_self_tex)
        glGenTextures(1, &s_self_tex);
    glBindTexture(GL_TEXTURE_2D, s_self_tex);
    if (s_self_w != f->w || s_self_h != f->h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, f->w, f->h, 0, GL_BGRA,
                     GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
        s_self_w = f->w;
        s_self_h = f->h;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, f->fbo);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, f->w, f->h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    mag == 1 ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    min == 1 ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_wrap(t->addr_u));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wrap(t->addr_v));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A,
                    fmt_has_alpha(t->color) ? GL_ALPHA : GL_ONE);
    *sw = (float)f->w;
    *sh = (float)f->h;
    return 1;
}

static void apply_state(const Nv2aRenderState *rs)
{
    static int cull_mode = -1;
    uint32_t m = rs->color_mask;

    if (cull_mode < 0) {
        const char *e = genv("VC3_GL_CULL");
        cull_mode = e ? atoi(e) : 0;
    }
    if (rs->blend_enable) {
        glEnable(GL_BLEND);
        glBlendFunc(rs->blend_src, rs->blend_dst);
        glBlendEquation(rs->blend_eq ? rs->blend_eq : GL_FUNC_ADD);
        glBlendColor(((rs->blend_color >> 16) & 0xFF) / 255.0f,
                     ((rs->blend_color >> 8) & 0xFF) / 255.0f,
                     (rs->blend_color & 0xFF) / 255.0f,
                     (rs->blend_color >> 24) / 255.0f);
    } else {
        glDisable(GL_BLEND);
    }
    if (rs->depth_test_enable && rs->zeta_va) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(rs->depth_func);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(rs->depth_write && rs->zeta_va ? GL_TRUE : GL_FALSE);
    glColorMask((m & 0x00010000u) != 0, (m & 0x100u) != 0, (m & 0x1u) != 0,
                (m & 0x01000000u) != 0);
    if (rs->stencil_enable && rs->zeta_va) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(rs->stencil_func, (GLint)rs->stencil_ref,
                      rs->stencil_func_mask);
        glStencilMask(rs->stencil_write_mask);
        glStencilOp(rs->stencil_fail, rs->stencil_zfail, rs->stencil_zpass);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    if (cull_mode && rs->cull_enable) {
        glEnable(GL_CULL_FACE);
        glCullFace(rs->cull_face);
        /* Rows are not flipped (see the header), so GL sees the title's
         * triangles mirrored; mode 2 takes the winding as given. */
        if (cull_mode == 2)
            glFrontFace(rs->front_face);
        else
            glFrontFace(rs->front_face == GL_CW ? GL_CCW : GL_CW);
    } else {
        glDisable(GL_CULL_FACE);
    }
    glUniform1i(u_atest, rs->alpha_test_enable && !genv("VC3_GL_NOATEST") ? 1 : 0);
    glUniform1i(u_afunc, (GLint)rs->alpha_func);
    glUniform1f(u_aref, (float)(rs->alpha_ref & 0xFF));
    {
        float dmin = rs->depth_min, dmax = rs->depth_max;
        if (!(dmax > dmin)) {
            dmin = 0.0f;
            dmax = 16777215.0f;
        }
        glUniform2f(u_depth, dmin, dmax);
    }
}

/* ── Back end entry points ─────────────────────────────────── */

static void gl_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    Surf *f;
    GLbitfield bits = 0;

    (void)rs;
    if (!gl_ready() || !(f = surf_bind(s)))
        return;
    glDisable(GL_SCISSOR_TEST);
    if (flags & 0xF0) {
        glColorMask((flags & 0x10) != 0, (flags & 0x20) != 0,
                    (flags & 0x40) != 0, (flags & 0x80) != 0);
        glClearColor(((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f,
                     (argb & 0xFF) / 255.0f, (argb >> 24) / 255.0f);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if (flags & 0x1) {
        glDepthMask(GL_TRUE);
        glClearDepth((double)(zstencil >> 8) / 16777215.0);
        bits |= GL_DEPTH_BUFFER_BIT;
    }
    if (flags & 0x2) {
        glStencilMask(0xFF);
        glClearStencil((GLint)(zstencil & 0xFF));
        bits |= GL_STENCIL_BUFFER_BIT;
    }
    if (s_ftr == 1)
        fprintf(stderr, "[GLTR] clear surf %08X flags %X colour %08X zstencil %08X "
                "zeta %08X\n", f->va, flags, argb, zstencil, rs->zeta_va);
    if (bits)
        glClear(bits);
    f->dirty = 1;
    f->memq_valid = 0;
    f->cpu_owned = 0;
}

/* VC3_GL_RCCHECK=1: for a few alpha-tested combiner batches, evaluate the
 * toolkit's C combiners (nv2a_rc_eval) at the first vertex with the real
 * texels and colours, and log inputs and result -- the reference the GLSL
 * port must match. */
static void rc_check(const Nv2aBatch *b)
{
    static int n;
    float v0[4], v1[4], fog[4] = {0, 0, 0, 1}, t[4][4], out[4];
    int st;

    if (n >= 8 || !b->rc || !b->ext || !b->state->alpha_test_enable
        || (b->state->alpha_ref & 0xFF) != 0x80 || b->rc->alpha_icw[1] != 0x191C0000u)
        return;
    n++;
    nv2a_rc_unpack(b->vertices[0].diffuse, v0);
    nv2a_rc_unpack(b->ext[0].specular, v1);
    fog[3] = b->ext[0].fog;
    for (st = 0; st < 4; st++) {
        const Nv2aTexture *tx = b->stage[st];
        uint32_t mode = (b->rc->stage_program >> (st * 5)) & 0x1F;
        t[st][0] = t[st][1] = t[st][2] = 0.0f;
        t[st][3] = 1.0f;
        if (!mode)
            continue;
        t[st][0] = t[st][1] = t[st][2] = t[st][3] = 1.0f;
        if (tx && tx->width && tx->height && tx->width <= 1024 && tx->height <= 1024) {
            uint32_t *px = (uint32_t *)malloc((size_t)tx->width * tx->height * 4);
            if (px && nv2a_backend_decode_texture(tx, px)) {
                int norm = d3d8_format_is_swizzled(tx->color)
                        || d3d8_format_dxt_block_bytes(tx->color);
                const float *c = b->ext[0].tex[st];
                float q = c[3] != 0.0f ? c[3] : 1.0f;
                float u = c[0] / q, v = c[1] / q;
                int x, y;
                if (norm) { u *= (float)tx->width; v *= (float)tx->height; }
                x = ((int)floorf(u) % (int)tx->width + (int)tx->width) % (int)tx->width;
                y = ((int)floorf(v) % (int)tx->height + (int)tx->height) % (int)tx->height;
                nv2a_rc_unpack(px[(size_t)y * tx->width + x], t[st]);
            }
            free(px);
        }
    }
    nv2a_rc_eval(b->rc, v0, v1, fog, (const float (*)[4])t, out);
    fprintf(stderr, "[RCCHK] atest func %X ref %02X | v0 %.3f %.3f %.3f %.3f v1.a %.3f fog %.2f"
            " | t0 %.3f %.3f %.3f %.3f t1 %.3f %.3f %.3f %.3f | out %.3f %.3f %.3f a=%.3f"
            " | aicw %08X %08X aocw %08X %08X ctl %08X\n",
            b->state->alpha_func, b->state->alpha_ref & 0xFF, v0[0], v0[1], v0[2], v0[3],
            v1[3], fog[3], t[0][0], t[0][1], t[0][2], t[0][3], t[1][0], t[1][1], t[1][2],
            t[1][3], out[0], out[1], out[2], out[3], b->rc->alpha_icw[0], b->rc->alpha_icw[1],
            b->rc->alpha_ocw[0], b->rc->alpha_ocw[1], b->rc->control);
}

/* VC3_GL_COMBINERS=1 evaluates the register combiners. Off by default: their
 * alpha comes out 0 for VC3's lightmapped walls (alpha-tested GEQUAL 0x80),
 * so combat lost the whole room, while texture * diffuse draws every scene
 * correctly once render targets read no-alpha formats as alpha 1. */
static int s_use_rc = 0;


/* Combiner state and inputs for a batch that carries them (Nv2aBatch.rc):
 * the four stage textures on units 0-3, each with the scale that turns its
 * coordinates -- normalised for swizzled/DXT, texels for linear -- into
 * GL's [0,1] over the texture actually bound; then the registers. */
static void bind_combiners(const Surf *f, const Nv2aBatch *b)
{
    const Nv2aCombiner *rc = b->rc;
    GLint on[4] = { 0, 0, 0, 0 }, cube[4] = { 0, 0, 0, 0 };
    GLfloat ts[8], f0[32], f1[32], c[4];
    GLuint k;
    int st;

    for (st = 0; st < 4; st++) {
        const Nv2aTexture *t = b->stage[st];
        float sw = 1.0f, sh = 1.0f;

        ts[st * 2] = ts[st * 2 + 1] = 1.0f;
        /* A cube map goes on unit 4 + stage, sampled by direction. */
        if (t && t->cube) {
            glActiveTexture(GL_TEXTURE4 + st);
            if (tex_bind_cube(t)) {
                on[st] = cube[st] = 1;
                glActiveTexture(GL_TEXTURE0 + st);
                glBindTexture(GL_TEXTURE_2D, s_white);
                continue;
            }
        }
        glActiveTexture(GL_TEXTURE0 + st);
        if (t && !t->cube && tex_bind_for(f, t, &sw, &sh)) {
            int norm = d3d8_format_is_swizzled(t->color)
                    || d3d8_format_dxt_block_bytes(t->color);
            on[st] = 1;
            ts[st * 2]     = (norm ? (float)t->width : 1.0f) / sw;
            ts[st * 2 + 1] = (norm ? (float)t->height : 1.0f) / sh;
        } else {
            glBindTexture(GL_TEXTURE_2D, s_white);
        }
    }
    glActiveTexture(GL_TEXTURE0);
    for (k = 0; k < 8; k++) {
        nv2a_rc_unpack(rc->factor0[k], &f0[k * 4]);
        nv2a_rc_unpack(rc->factor1[k], &f1[k * 4]);
    }
    glUniform1i(u_rc, 1);
    glUniform1uiv(u_cicw, 8, rc->color_icw);
    glUniform1uiv(u_aicw, 8, rc->alpha_icw);
    glUniform1uiv(u_cocw, 8, rc->color_ocw);
    glUniform1uiv(u_aocw, 8, rc->alpha_ocw);
    glUniform4fv(u_f0, 8, f0);
    glUniform4fv(u_f1, 8, f1);
    glUniform1ui(u_fin0, rc->final0);
    glUniform1ui(u_fin1, rc->final1);
    glUniform1ui(u_ctl, rc->control);
    glUniform1ui(u_sprog, rc->stage_program);
    nv2a_rc_unpack(rc->final_c0, c);
    glUniform4fv(u_fc0, 1, c);
    nv2a_rc_unpack(rc->final_c1, c);
    glUniform4fv(u_fc1, 1, c);
    /* FOG_COLOR is R in bits 0-7, the reverse of a D3DCOLOR. */
    nv2a_rc_unpack(b->fog_color, c);
    glUniform3f(u_fogc, c[2], c[1], c[0]);
    glUniform1iv(u_on, 4, on);
    glUniform1iv(glGetUniformLocation(s_prog, "u_iscube"), 4, cube);
    glUniform1i(glGetUniformLocation(s_prog, "u_dbgon"),
                !(b->stage[0] && surf_find(b->stage[0]->offset)));
    glUniform2fv(u_ts, 4, ts);
    {
        /* RCDBG 6: a colour per (stage program, final combiner, stage 0-1
         * inputs), logged once each, so a pixel names the setup that drew it. */
        static uint32_t seen[64];
        static int nseen;
        uint32_t h = rc->stage_program * 2654435761u ^ rc->final0 * 40503u
                   ^ rc->final1 ^ rc->color_icw[0] * 7u ^ rc->color_icw[1] * 13u;
        float r = (float)((h >> 16) & 0xFF) / 255.0f, g = (float)((h >> 8) & 0xFF) / 255.0f,
              bl = (float)(h & 0xFF) / 255.0f;
        int i, known = 0;
        for (i = 0; i < nseen; i++)
            if (seen[i] == h) known = 1;
        if (!known && nseen < 64 && genv("VC3_GL_RCDBG")) {
            seen[nseen++] = h;
            fprintf(stderr, "[GL]   rc setup %02X%02X%02X: prog %05X fin %08X %08X "
                    "s0 %08X/%08X s1 %08X/%08X spec %08X tex %08X/%08X\n",
                    (h >> 16) & 0xFF, (h >> 8) & 0xFF, h & 0xFF, rc->stage_program,
                    rc->final0, rc->final1, rc->color_icw[0], rc->color_ocw[0],
                    rc->color_icw[1], rc->color_ocw[1], b->ext[0].specular,
                    b->stage[0] ? b->stage[0]->offset : 0,
                    b->stage[1] ? b->stage[1]->offset : 0);
        }
        glUniform3f(glGetUniformLocation(s_prog, "u_dbgcol"), r, g, bl);
        if (genv("VC3_GL_RCDBG") && atoi(genv("VC3_GL_RCDBG")) >= 6)
            glDisable(GL_BLEND);     /* exact colours, so a pixel names its setup */
    }

    glBindBuffer(GL_ARRAY_BUFFER, s_vbo_ext);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(b->count * sizeof(Nv2aVertexEx)),
                 b->ext, GL_STREAM_DRAW);
    for (k = 3; k <= 8; k++)
        glEnableVertexAttribArray(k);
}

/* The lightmap pattern of VC3's walls: stage 1 samples a 2D texture and the
 * stage's RGB is t1 * r0 (input A texture 1, B r0). */
static int lightmap_batch(const Nv2aBatch *b)
{
    uint32_t icw1;

    if (!b->rc || !b->ext || !b->stage[1] || (b->rc->control & 0xFF) < 2
        || ((b->rc->stage_program >> 5) & 0x1F) != 1)
        return 0;
    icw1 = b->rc->color_icw[1];
    return ((icw1 >> 24) & 0x1F) == 0x09 && ((icw1 >> 16) & 0x1F) == 0x0C;
}

static void gl_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    Surf *f;
    float tw = 1.0f, th = 1.0f;
    int textured = 0;

    if (!b->count || !gl_ready() || !(f = surf_bind(s)))
        return;
    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    apply_state(b->state);
    glUniform2f(u_surf, (float)f->w, (float)f->h);
    glActiveTexture(GL_TEXTURE0);
    if (b->texture)
        textured = tex_bind_for(f, b->texture, &tw, &th);
    if (s_st.on) {
        if (!b->texture && (b->stage[1] || b->stage[2] || b->stage[3]))
            s_st.stage0_empty++;
        if (b->rc && !(b->rc->stage_program & 0x1F) && b->texture)
            s_st.stage0_off++;
        if (b->rc) {
            int st;
            for (st = 0; st < 4; st++)
                s_st.stage_mode[(b->rc->stage_program >> (st * 5)) & 0x1F]++;
        }
    }
    if (s_st.on && textured && surf_find(b->texture->offset) && s_st.rt_logged < 6) {
        float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
        uint32_t k;
        for (k = 0; k < b->count; k++) {
            const Nv2aVertex *q = &b->vertices[k];
            if (q->u < u0) u0 = q->u;
            if (q->u > u1) u1 = q->u;
            if (q->v < v0) v0 = q->v;
            if (q->v > v1) v1 = q->v;
        }
        s_st.rt_logged++;
        fprintf(stderr, "[GL]   rt %08X -> %08X: uv %.1f..%.1f x %.1f..%.1f, tex %ux%u "
                "fmt %02X pitch %u, divided by %.0fx%.0f\n", b->texture->offset, f->va,
                u0, u1, v0, v1, b->texture->width, b->texture->height,
                b->texture->color, b->texture->pitch, tw, th);
    }
    if (s_ftr == 1) {
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, z0 = 1e30f, z1 = -1e30f;
        uint32_t k;
        for (k = 0; k < b->count; k++) {
            const Nv2aVertex *q = &b->vertices[k];
            if (q->x < x0) x0 = q->x;
            if (q->x > x1) x1 = q->x;
            if (q->y < y0) y0 = q->y;
            if (q->y > y1) y1 = q->y;
            if (q->z < z0) z0 = q->z;
            if (q->z > z1) z1 = q->z;
        }
        fprintf(stderr, "[GLTR]   depth func %X z %.6g..%.6g clip %.6g..%.6g | atest %u func %X ref %02X"
                " | aicw0 %08X aicw1 %08X fin1 %08X\n",
                b->state->depth_func, z0, z1, b->state->depth_min, b->state->depth_max,
                b->state->alpha_test_enable, b->state->alpha_func, b->state->alpha_ref & 0xFF,
                b->rc ? b->rc->alpha_icw[0] : 0, b->rc ? b->rc->alpha_icw[1] : 0,
                b->rc ? b->rc->final1 : 0);
        fprintf(stderr, "[GLTR] %s surf %08X n %u box %.0f..%.0f x %.0f..%.0f diff %08X "
                "tex %08X%s s1 %08X blend %u %X/%X z %u/%u sprog %05X fin %08X fog %.2f\n",
                b->rc ? "RC " : "FF ", f->va, b->count, x0, x1, y0, y1,
                b->vertices[0].diffuse, b->texture ? b->texture->offset : 0,
                b->texture && b->texture->offset == f->va ? "(self)" : "",
                b->stage[1] ? b->stage[1]->offset : 0, b->state->blend_enable,
                b->state->blend_src, b->state->blend_dst, b->state->depth_test_enable,
                b->state->depth_write, b->rc ? b->rc->stage_program : 0,
                b->rc ? b->rc->final0 : 0, b->ext && b->rc ? b->ext[0].fog : -1.0f);
        if (b->texture) {
            /* What stage 0 samples: the texture's shape and the coordinate
             * ranges the default path (u,v) and the program (tex[0]) give. */
            float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
            float s0 = 1e30f, s1 = -1e30f, t0 = 1e30f, t1 = -1e30f;
            uint32_t k;
            for (k = 0; k < b->count; k++) {
                const Nv2aVertex *q = &b->vertices[k];
                if (q->u < u0) u0 = q->u;
                if (q->u > u1) u1 = q->u;
                if (q->v < v0) v0 = q->v;
                if (q->v > v1) v1 = q->v;
                if (b->ext) {
                    const float *tc = b->ext[k].tex[0];
                    if (tc[0] < s0) s0 = tc[0];
                    if (tc[0] > s1) s1 = tc[0];
                    if (tc[1] < t0) t0 = tc[1];
                    if (tc[1] > t1) t1 = tc[1];
                }
            }
            fprintf(stderr, "[GLTR]   tex0 %ux%u fmt %02X pitch %u | uv %.3g..%.3g x %.3g..%.3g"
                    " | st %.3g..%.3g x %.3g..%.3g\n",
                    b->texture->width, b->texture->height, b->texture->color & 0xFF,
                    b->texture->pitch, u0, u1, v0, v1, s0, s1, t0, t1);
        }
    }
    s_st.draws++;
    s_st.tris += b->count / 3;
    if (textured)
        s_st.textured++;
    else if (!b->texture)
        s_st.untex++;
    if (!textured)
        glBindTexture(GL_TEXTURE_2D, s_white);
    glUniform1i(u_textured, textured);
    glUniform2f(u_texsize, tw, th);
    if (b->rc && b->ext && s_use_rc && s_st.on && s_st.rc_logged < 4) {
        const Nv2aVertexEx *e = &b->ext[0];
        s_st.rc_logged++;
        fprintf(stderr, "[GL]   rc batch: fog %08X diffuse %08X specular %08X "
                "fogf %.3f tc0 %.3f %.3f %.3f %.3f tc1 %.3f %.3f %.3f %.3f\n",
                b->fog_color, b->vertices[0].diffuse, e->specular, e->fog,
                e->tex[0][0], e->tex[0][1], e->tex[0][2], e->tex[0][3],
                e->tex[1][0], e->tex[1][1], e->tex[1][2], e->tex[1][3]);
        {
            int k;
            for (k = 0; k < 3 && k < (int)b->count; k++)
                fprintf(stderr, "[GL]     v%d xy %.1f %.1f rhw %.4f uv %.2f %.2f | "
                        "ext tc0 %.4f %.4f %.4f %.4f\n", k, b->vertices[k].x,
                        b->vertices[k].y, b->vertices[k].rhw, b->vertices[k].u,
                        b->vertices[k].v, b->ext[k].tex[0][0], b->ext[k].tex[0][1],
                        b->ext[k].tex[0][2], b->ext[k].tex[0][3]);
        }
    }
    if (genv("VC3_GL_RCCHECK"))
        rc_check(b);
    /* A batch with a cube-map stage can only be drawn faithfully by the
     * combiners: the default texture * diffuse path has no direction to look
     * a cube up by. Opt-in (VC3_GL_CUBE_RC=1) while the combiner path is not
     * yet right: routed by default, VC3's monitor-room walls went flat grey. */
    int has_cube = 0;
    if (b->rc && b->ext && genv("VC3_GL_CUBE_RC")) {
        int st;
        for (st = 0; st < 4; st++)
            if (b->stage[st] && b->stage[st]->cube
                && ((b->rc->stage_program >> (st * 5)) & 0x1F) == 3)
                has_cube = 1;
    }
    if (b->rc && b->ext && (s_use_rc || has_cube))
        bind_combiners(f, b);
    else {
        GLuint k;
        int lmap = lightmap_batch(b);
        glUniform1i(u_rc, 0);
        glUniform1i(glGetUniformLocation(s_prog, "u_lmap"), 0);
        for (k = 3; k <= 8; k++)
            glDisableVertexAttribArray(k);
        if (lmap && textured) {
            /* VC3's walls: stage 1 multiplies by a lightmap (t1 * r0). Drawn
             * as texture * diffuse * lightmap, keeping the texture * diffuse
             * alpha its alpha test needs. */
            float sw = 1.0f, sh = 1.0f;
            const Nv2aTexture *t = b->stage[1];
            glActiveTexture(GL_TEXTURE1);
            if (tex_bind_for(f, t, &sw, &sh)) {
                int norm = d3d8_format_is_swizzled(t->color)
                        || d3d8_format_dxt_block_bytes(t->color);
                GLfloat ts[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
                ts[2] = (norm ? (float)t->width : 1.0f) / sw;
                ts[3] = (norm ? (float)t->height : 1.0f) / sh;
                glUniform2fv(u_ts, 4, ts);
                glUniform1ui(u_sprog, b->rc->stage_program);
                glUniform1i(glGetUniformLocation(s_prog, "u_lmap"), 1);
                glBindBuffer(GL_ARRAY_BUFFER, s_vbo_ext);
                glBufferData(GL_ARRAY_BUFFER,
                             (GLsizeiptr)(b->count * sizeof(Nv2aVertexEx)),
                             b->ext, GL_STREAM_DRAW);
                for (k = 3; k <= 8; k++)
                    glEnableVertexAttribArray(k);
            }
            glActiveTexture(GL_TEXTURE0);
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(b->count * sizeof(Nv2aVertex)),
                 b->vertices, GL_STREAM_DRAW);
    if (b->zpass_out) {
        /* A visibility test: count the pixels that pass, for GET_REPORT.
         * Reading the result waits for the GPU, but only test batches pay. */
        static GLuint q;
        GLuint passed = 0;
        if (!q)
            glGenQueries(1, &q);
        glBeginQuery(GL_SAMPLES_PASSED, q);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)b->count);
        glEndQuery(GL_SAMPLES_PASSED);
        glGetQueryObjectuiv(q, GL_QUERY_RESULT, &passed);
        *b->zpass_out += passed;
        s_st.zpass_batches++;
        s_st.zpass_pixels += passed;
    } else {
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)b->count);
    }
    if (genv("VC3_GL_RCCHECK") && b->rc && b->ext && s_use_rc
        && b->state->alpha_test_enable && (b->state->alpha_ref & 0xFF) == 0x80
        && b->rc->alpha_icw[1] == 0x191C0000u) {
        /* The GPU's own values for this batch, read back at the centre of
         * its first triangle: debug view 13 (vertex alpha, stage 0/1 bound)
         * then 12 (combiner alpha, t0.a, t1.a). */
        static int probes;
        float cx = (b->vertices[0].x + b->vertices[1].x + b->vertices[2].x) / 3.0f;
        float cy = (b->vertices[0].y + b->vertices[1].y + b->vertices[2].y) / 3.0f;
        if (probes < 8 && cx >= 0 && cy >= 0 && cx < f->w && cy < f->h) {
            GLint loc = glGetUniformLocation(s_prog, "u_rcdbg");
            unsigned char px13[4], px12[4];
            probes++;
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
            glDisable(GL_STENCIL_TEST);
            glColorMask(1, 1, 1, 1);
            glUniform1i(u_atest, 0);
            glUniform1i(glGetUniformLocation(s_prog, "u_dbgon"), 1);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, f->fbo);
            {
                unsigned char before[4], red[4];
                glReadPixels((GLint)cx, (GLint)cy, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, before);
                glEnable(GL_SCISSOR_TEST);
                glScissor((GLint)cx, (GLint)cy, 1, 1);
                glClearColor(1, 0, 0, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                glDisable(GL_SCISSOR_TEST);
                glReadPixels((GLint)cx, (GLint)cy, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, red);
                fprintf(stderr, "[RCGPU] probe %.0f,%.0f fbo %u surf %08X %dx%d: after draw %02X%02X%02X%02X,"
                        " after red clear %02X%02X%02X%02X; v0 %.1f,%.1f v1 %.1f,%.1f v2 %.1f,%.1f\n",
                        cx, cy, f->fbo, f->va, f->w, f->h, before[0], before[1], before[2], before[3],
                        red[0], red[1], red[2], red[3], b->vertices[0].x, b->vertices[0].y,
                        b->vertices[1].x, b->vertices[1].y, b->vertices[2].x, b->vertices[2].y);
            }
            glUniform1i(loc, 13);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glReadPixels((GLint)cx, (GLint)cy, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px13);
            glUniform1i(loc, 12);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glReadPixels((GLint)cx, (GLint)cy, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px12);
            glUniform1i(loc, genv("VC3_GL_RCDBG") ? atoi(genv("VC3_GL_RCDBG")) : 0);
            fprintf(stderr, "[RCGPU] at %.0f,%.0f: v_col.a %u on0 %u on1 %u | comb.a %u t0.a %u t1.a %u"
                    " | diffuse[0] %08X\n", cx, cy, px13[0], px13[1], px13[2],
                    px12[0], px12[1], px12[2], b->vertices[0].diffuse);
        }
    }
    glActiveTexture(GL_TEXTURE0);
    f->dirty = 1;
    f->memq_valid = 0;
    f->cpu_owned = 0;
}

static void gl_flip(void)
{
    int i;

    if (!gl_ready())
        return;
    if (s_ftr == 1) {
        s_ftr = 2;
        fprintf(stderr, "[GLTR] end of frame\n");
    } else if (s_ftr == 0 && genv("VC3_GL_FTRACE")) {
        FILE *fl = fopen(genv("VC3_GL_FTRACE"), "rb");
        if (fl) {
            fclose(fl);
            s_ftr = 1;
            fprintf(stderr, "[GLTR] frame begins\n");
        }
    }
    /* Only the surface being presented goes back to guest memory: render
     * targets the title samples are read from their framebuffers on the GPU,
     * and reading every dirty surface back each flip, synchronously, was half
     * the GPU thread. VC3_GL_READBACK_ALL=1 restores that, for a title that
     * reads a render target with the CPU. */
    {
        static int all = -1;
        uint32_t shown = nv2a_backend_presented_va();
        if (all < 0)
            all = genv("VC3_GL_READBACK_ALL") != NULL;
        for (i = 0; i < s_nsurfs; i++) {
            if (!s_surfs[i].dirty)
                continue;
            if (shown && s_surfs[i].va == shown && !all)
                surf_readback_async(&s_surfs[i]);
            else if (all || !shown)
                surf_readback(&s_surfs[i]);
        }
    }
    s_frame++;
    if (genv("VC3_FPS_LOG")) {            /* wall-clock flips per second */
        static struct timespec t0;
        static uint32_t f0;
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (!t0.tv_sec) {
            t0 = t;
            f0 = s_frame;
        } else if (t.tv_sec - t0.tv_sec >= 1) {
            double dt = (double)(t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
            fprintf(stderr, "[FPS] %.1f\n", (s_frame - f0) / dt);
            t0 = t;
            f0 = s_frame;
        }
    }
    if (s_st.on && s_frame % 300 == 0) {
        fprintf(stderr, "[GL] %u flips: %u draws (%u tris), %u textured, %u "
                "no texture, %u from surfaces, %u uploads; surfaces:",
                300u, s_st.draws, s_st.tris, s_st.textured, s_st.untex,
                s_st.rt, s_st.uploads);
        for (i = 0; i < s_nsurfs; i++)
            fprintf(stderr, " %08X/%dx%d", s_surfs[i].va, s_surfs[i].w,
                    s_surfs[i].h);
        fprintf(stderr, "\n[GL]   visibility tests: %u batches, %u pixels passed", s_st.zpass_batches, s_st.zpass_pixels);
        fprintf(stderr, "\n[GL]   stage 0 empty but a later stage textured: %u; stage-0 program off: %u",
                s_st.stage0_empty, s_st.stage0_off);
        fprintf(stderr, "\n[GL]   stage modes:");
        for (i = 0; i < 32; i++)
            if (s_st.stage_mode[i])
                fprintf(stderr, " %d:%u", i, s_st.stage_mode[i]);
        fprintf(stderr, "\n[GL]   formats:");
        for (i = 0; i < 256; i++)
            if (s_st.fmt[i])
                fprintf(stderr, " %02X:%u%s", i, s_st.fmt[i],
                        s_st.fail[i] ? "(FAIL)" : "");
        fprintf(stderr, "\n");
        i = s_st.on;
        memset(&s_st, 0, sizeof s_st);
        s_st.on = i;
    }
}

static const Nv2aBackend s_gl_backend = { gl_clear, gl_draw, gl_flip };

/* Register unless VC3_GL=0 (the executor's software rasteriser then draws). */
void vc3_gl_backend_register(void)
{
    const char *e = genv("VC3_GL");

    if (e && e[0] == '0') {
        fprintf(stderr, "[GL] off (VC3_GL=0): software rasteriser\n");
        return;
    }
    s_st.on = genv("VC3_GL_STATS") != NULL;
    e = genv("VC3_GL_COMBINERS");
    s_use_rc = e && e[0] == '1';
    nv2a_backend_register(&s_gl_backend);
    fprintf(stderr, "[GL] OpenGL back end registered\n");
}
