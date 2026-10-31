/*
 * Texture uploads: what they cost (VC3_TEX_STATS), and their decode on the
 * GPU (VC3_TEX_GPU).
 *
 * The CPU path (nv2a_backend_decode_texture) decodes a texture one texel at a
 * time through the executor's sampler and uploads RGBA8. Here the guest's
 * bytes go up as they are -- every level, one copy into a texture buffer
 * (R32UI words) -- and a shader decodes them into the RGBA8 texture: the
 * Morton swizzle, DXT1/3/5 blocks, the 8-, 16- and 32-bit packed formats, P8
 * through its palette (appended to the same buffer), and 4:2:2 YUV.
 *
 *   GLES 3.2 (Quest)  a compute shader, one dispatch per level, into the
 *                     texture as an image (immutable storage, glTexStorage2D).
 *   GL 4.1 (Mac)      no compute: a fragment pass renders each level into the
 *                     texture through a framebuffer. Same decode function.
 *
 * The decode is the CPU's integer arithmetic carried over (d3d8_swizzle.h and
 * sample_tex in nv2a_pb_exec.c), so it is bit-exact; VC3_TEX_COMPARE=1 checks
 * that on every upload. VC3_TEX_S3TC=1, where the driver has S3TC: DXT goes
 * up compressed with no decode at all. Not the default: the Mac's S3TC
 * decode rounds its interpolated colours differently from the CPU decoder,
 * up to 3 apart (over the regression, 1.2 M texels of 422 DXT textures), so
 * the image would no longer be the one the CPU path draws.
 *
 * Mips: a texture the back end mipmaps decodes the guest's own levels where
 * it has them (FORMAT's MIPMAP_LEVELS), else level 0 and glGenerateMipmap.
 *
 * All of this runs in the middle of gl_backend's draw setup, so the passes
 * save and restore every piece of GL state they touch.
 */
#if defined(VC3_GLES)
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#define GL_BGRA                     GL_RGBA
#define GL_UNSIGNED_INT_8_8_8_8_REV GL_UNSIGNED_BYTE
#else
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#endif

#include "gl_texdec.h"
#include "nv2a_backend.h"
#include "../d3d/d3d8_swizzle.h"   /* relative to src/kernel, as the executor */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "chihiro/ch_env.h"

#ifndef GL_COPY_WRITE_BUFFER_BINDING
#define GL_COPY_WRITE_BUFFER_BINDING GL_COPY_WRITE_BUFFER   /* same enum */
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* ── Counters (VC3_TEX_STATS=1) ─────────────────────────────── */

int g_texstat_on;

static struct {
    uint64_t n[3][256], texels[3][256], dec_ns[3][256], gl_ns[3][256], max_ns[3][256];
    uint64_t hash_ns, hashes;
    uint64_t gpu_ns, gpu_passes;          /* GL_TIMESTAMP around the passes (GL) */
} s_ts;

/* VC3_TEX_COMPARE=1: per format, what the comparison found. */
static struct {
    uint64_t tex[256], levels[256], texels[256], bad_tex[256], bad_texels[256];
    uint32_t maxdiff[256], logged;
    uint64_t skipped;                     /* the CPU decoder refused the format */
} s_cmp;

uint64_t texstat_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void texstat_init(void)
{
    const char *e = ch_env("TEX_STATS");
    g_texstat_on = e && *e && *e != '0' ? atoi(e) > 1 ? 2 : 1 : 0;
}

void texstat_add(uint32_t fmt, int path, uint64_t texels, uint64_t dec_ns, uint64_t gl_ns)
{
    fmt &= 0xFF;
    s_ts.n[path][fmt]++;
    s_ts.texels[path][fmt] += texels;
    s_ts.dec_ns[path][fmt] += dec_ns;
    s_ts.gl_ns[path][fmt] += gl_ns;
    if (dec_ns + gl_ns > s_ts.max_ns[path][fmt])
        s_ts.max_ns[path][fmt] = dec_ns + gl_ns;
}

void texstat_hash(uint64_t ns)
{
    s_ts.hash_ns += ns;
    s_ts.hashes++;
}

static int s_compare;

/* Every 300 flips: the run so far, per path and format. */
void texstat_flip(uint32_t frame)
{
    static const char *const PATH[3] = { "cpu", "gpu", "native" };
    uint64_t n = 0, dec = 0, gl = 0, mx = 0;
    int p, f;

    if (frame % 300)
        return;
    if (g_texstat_on) {
        fprintf(stderr, "[TEXSTAT] flip %u: hash %.2f ms over %llu checks; GPU decode passes "
                "%llu, %.2f ms GPU time\n", frame, s_ts.hash_ns / 1e6,
                (unsigned long long)s_ts.hashes, (unsigned long long)s_ts.gpu_passes,
                s_ts.gpu_ns / 1e6);
        for (p = 0; p < 3; p++)
            for (f = 0; f < 256; f++) {
                if (!s_ts.n[p][f])
                    continue;
                fprintf(stderr, "[TEXSTAT]   %-6s fmt %02X: %5llu uploads %8.2f Mtexels"
                        " decode %8.2f ms  gl %7.2f ms  max %6.2f ms\n", PATH[p], f,
                        (unsigned long long)s_ts.n[p][f], s_ts.texels[p][f] / 1e6,
                        s_ts.dec_ns[p][f] / 1e6, s_ts.gl_ns[p][f] / 1e6,
                        s_ts.max_ns[p][f] / 1e6);
                n += s_ts.n[p][f];
                dec += s_ts.dec_ns[p][f];
                gl += s_ts.gl_ns[p][f];
                if (s_ts.max_ns[p][f] > mx)
                    mx = s_ts.max_ns[p][f];
            }
        fprintf(stderr, "[TEXSTAT]   total: %llu uploads, decode %.2f ms, gl %.2f ms, max %.2f ms\n",
                (unsigned long long)n, dec / 1e6, gl / 1e6, mx / 1e6);
    }
    if (s_compare) {
        uint64_t t = 0, l = 0, x = 0, bt = 0, bx = 0;
        fprintf(stderr, "[TEXCMP] flip %u:", frame);
        for (f = 0; f < 256; f++) {
            if (!s_cmp.tex[f])
                continue;
            fprintf(stderr, " %02X: %llu tex/%llu levels, %llu bad (%llu texels, max diff %u);", f,
                    (unsigned long long)s_cmp.tex[f], (unsigned long long)s_cmp.levels[f],
                    (unsigned long long)s_cmp.bad_tex[f],
                    (unsigned long long)s_cmp.bad_texels[f], s_cmp.maxdiff[f]);
            t += s_cmp.tex[f];
            l += s_cmp.levels[f];
            x += s_cmp.texels[f];
            bt += s_cmp.bad_tex[f];
            bx += s_cmp.bad_texels[f];
        }
        fprintf(stderr, "\n[TEXCMP]   total %llu textures, %llu levels, %.2f Mtexels: %llu "
                "mismatched textures, %llu texels; %llu not comparable\n",
                (unsigned long long)t, (unsigned long long)l, x / 1e6,
                (unsigned long long)bt, (unsigned long long)bx,
                (unsigned long long)s_cmp.skipped);
    }
}

/* ── The decoder ───────────────────────────────────────────── */

/* One source for both stages. ARGB comes out of decode() as the CPU's
 * 0xAARRGGBB; ES stores it as B,G,R,A bytes, the way the CPU path's upload
 * lands there (gl_backend's es_bgra swizzles it back when sampled). */
static const char DECODE_GLSL[] =
    "uniform highp usamplerBuffer u_raw;\n"
    "uniform int u_fmt;\n"            /* the linear twin, or the DXT code */
    "uniform uint u_base, u_pitch, u_mx, u_my, u_w, u_pal;\n"
    "uint rd32(uint o) {\n"
    "    uint s = (o & 3u) * 8u, a = texelFetch(u_raw, int(o >> 2)).r;\n"
    "    if (s == 0u) return a;\n"
    "    return (a >> s) | (texelFetch(u_raw, int(o >> 2) + 1).r << (32u - s));\n"
    "}\n"
    "uint rd8(uint o) { return (texelFetch(u_raw, int(o >> 2)).r >> ((o & 3u) * 8u)) & 0xFFu; }\n"
    "uint rd16(uint o) { return rd32(o) & 0xFFFFu; }\n"
    /* swizzle_deposit: the low bits of v into the set bits of mask */
    "uint deposit(uint v, uint mask) {\n"
    "    uint r = 0u, bit = 1u;\n"
    "    while (mask != 0u) {\n"
    "        uint low = mask & (~mask + 1u);\n"
    "        if ((v & bit) != 0u) r |= low;\n"
    "        mask &= mask - 1u;\n"
    "        bit <<= 1u;\n"
    "    }\n"
    "    return r;\n"
    "}\n"
    "uint expand(uint v, uint bits) { return v * 255u / ((1u << bits) - 1u); }\n"
    "uint at(uint x, uint y, uint bpp) {\n"
    "    if (u_pitch == 0u) return u_base + (deposit(x, u_mx) | deposit(y, u_my)) * bpp;\n"
    "    return u_base + y * u_pitch + x * bpp;\n"
    "}\n"
    "uint rgb565(uint c) {\n"
    "    return (expand((c >> 11) & 0x1Fu, 5u) << 16) | (expand((c >> 5) & 0x3Fu, 6u) << 8)\n"
    "         | expand(c & 0x1Fu, 5u);\n"
    "}\n"
    /* d3d8_dxt_decode_texel */
    "uint dxt(uint x, uint y) {\n"
    "    uint bb = u_fmt == 0x0C ? 8u : 16u, bpr = (u_w + 3u) / 4u;\n"
    "    uint b = u_base + ((y >> 2) * bpr + (x >> 2)) * bb, texel = (y & 3u) * 4u + (x & 3u);\n"
    "    uint alpha = 255u;\n"
    "    if (bb == 16u) {\n"
    "        if (u_fmt == 0x0E) {\n"
    "            uint nib = rd8(b + (texel >> 1));\n"
    "            alpha = expand((texel & 1u) != 0u ? (nib >> 4) : (nib & 0x0Fu), 4u);\n"
    "        } else {\n"
    "            uint a0 = rd8(b), a1 = rd8(b + 1u), sel, sh = 3u * texel;\n"
    "            uint lo = rd32(b + 2u), hi = rd16(b + 6u);\n"
    "            if (sh + 3u <= 32u) sel = (lo >> sh) & 7u;\n"
    "            else if (sh >= 32u) sel = (hi >> (sh - 32u)) & 7u;\n"
    "            else sel = ((lo >> sh) | (hi << (32u - sh))) & 7u;\n"
    "            if (sel == 0u) alpha = a0;\n"
    "            else if (sel == 1u) alpha = a1;\n"
    "            else if (a0 > a1) alpha = ((8u - sel) * a0 + (sel - 1u) * a1) / 7u;\n"
    "            else if (sel < 6u) alpha = ((6u - sel) * a0 + (sel - 1u) * a1) / 5u;\n"
    "            else alpha = sel == 6u ? 0u : 255u;\n"
    "        }\n"
    "        b += 8u;\n"
    "    }\n"
    "    uint c0 = rd16(b), c1 = rd16(b + 2u), e0 = rgb565(c0), e1 = rgb565(c1), c = 0u;\n"
    "    uint idx = (rd32(b + 4u) >> (2u * texel)) & 3u;\n"
    "    if (idx == 0u) c = e0;\n"
    "    else if (idx == 1u) c = e1;\n"
    "    else if (c0 > c1 || bb == 16u) {\n"
    "        for (uint i = 0u; i < 3u; i++) {\n"
    "            uint sh = i * 8u, m0 = (e0 >> sh) & 0xFFu, m1 = (e1 >> sh) & 0xFFu;\n"
    "            c |= (idx == 2u ? (2u * m0 + m1) / 3u : (m0 + 2u * m1) / 3u) << sh;\n"
    "        }\n"
    "    } else if (idx == 2u) {\n"
    "        for (uint i = 0u; i < 3u; i++) {\n"
    "            uint sh = i * 8u, m0 = (e0 >> sh) & 0xFFu, m1 = (e1 >> sh) & 0xFFu;\n"
    "            c |= ((m0 + m1) / 2u) << sh;\n"
    "        }\n"
    "    } else {\n"
    "        c = 0u;\n"
    "        alpha = bb == 8u ? 0u : alpha;\n"
    "    }\n"
    "    return (alpha << 24) | (c & 0xFFFFFFu);\n"
    "}\n"
    "int clamp255(int v) { return clamp(v, 0, 255); }\n"
    /* sample_tex's switch, by the linear twin */
    "uint decode(uint x, uint y) {\n"
    "    uint t;\n"
    "    switch (u_fmt) {\n"
    "    case 0x0C: case 0x0E: case 0x0F: return dxt(x, y);\n"
    "    case 0x12: return rd32(at(x, y, 4u));\n"
    "    case 0x1E: return rd32(at(x, y, 4u)) | 0xFF000000u;\n"
    "    case 0x0B: return rd32(u_pal + rd8(at(x, y, 1u)) * 4u);\n"
    "    case 0x3F: t = rd32(at(x, y, 4u));\n"
    "        return (t & 0xFF00FF00u) | ((t & 0xFFu) << 16) | ((t >> 16) & 0xFFu);\n"
    "    case 0x40: t = rd32(at(x, y, 4u));\n"
    "        return ((t & 0xFFu) << 24) | (((t >> 8) & 0xFFu) << 16) | (((t >> 16) & 0xFFu) << 8)\n"
    "             | ((t >> 24) & 0xFFu);\n"
    "    case 0x41: t = rd32(at(x, y, 4u)); return ((t & 0xFFu) << 24) | (t >> 8);\n"
    "    case 0x10: t = rd16(at(x, y, 2u));\n"
    "        return ((t & 0x8000u) != 0u ? 0xFF000000u : 0u) | (expand((t >> 10) & 0x1Fu, 5u) << 16)\n"
    "             | (expand((t >> 5) & 0x1Fu, 5u) << 8) | expand(t & 0x1Fu, 5u);\n"
    "    case 0x1C: t = rd16(at(x, y, 2u));\n"
    "        return 0xFF000000u | (expand((t >> 10) & 0x1Fu, 5u) << 16)\n"
    "             | (expand((t >> 5) & 0x1Fu, 5u) << 8) | expand(t & 0x1Fu, 5u);\n"
    "    case 0x11: t = rd16(at(x, y, 2u)); return 0xFF000000u | rgb565(t);\n"
    "    case 0x1D: t = rd16(at(x, y, 2u));\n"
    "        return (expand((t >> 12) & 0xFu, 4u) << 24) | (expand((t >> 8) & 0xFu, 4u) << 16)\n"
    "             | (expand((t >> 4) & 0xFu, 4u) << 8) | expand(t & 0xFu, 4u);\n"
    "    case 0x13: t = rd8(at(x, y, 1u)); return 0xFF000000u | (t << 16) | (t << 8) | t;\n"
    "    case 0x1F: return (rd8(at(x, y, 1u)) << 24) | 0x00FFFFFFu;\n"
    "    case 0x24: case 0x25: {\n"
    "        uint yoff = u_fmt == 0x24 ? 0u : 1u, g = u_base + y * u_pitch + (x & ~1u) * 2u;\n"
    "        int c = int(rd8(g + ((x & 1u) != 0u ? 2u + yoff : yoff))) - 16;\n"
    "        int cu = int(rd8(g + 1u - yoff)) - 128, cv = int(rd8(g + 3u - yoff)) - 128;\n"
    "        int r = clamp255((298 * c + 409 * cv + 128) >> 8);\n"
    "        int gg = clamp255((298 * c - 100 * cu - 208 * cv + 128) >> 8);\n"
    "        int bl = clamp255((298 * c + 516 * cu + 128) >> 8);\n"
    "        return 0xFF000000u | (uint(r) << 16) | (uint(gg) << 8) | uint(bl);\n"
    "    }\n"
    "    }\n"
    "    return 0xFFFF00FFu;\n"
    "}\n"
    "vec4 texel(uint x, uint y) {\n"
    "    uint c = decode(x, y);\n"
    "    vec4 v = vec4(float((c >> 16) & 0xFFu), float((c >> 8) & 0xFFu), float(c & 0xFFu),\n"
    "                  float(c >> 24)) / 255.0;\n"
#if defined(VC3_GLES)
    "    return v.bgra;\n"
#else
    "    return v;\n"
#endif
    "}\n";

#if defined(VC3_GLES)
#define GLSL_HEAD "#version 320 es\nprecision highp float;\nprecision highp int;\n" \
                  "precision highp usamplerBuffer;\nprecision highp sampler2D;\n"
#else
#define GLSL_HEAD "#version 410 core\n"
#endif

static const char VS_SRC[] = GLSL_HEAD
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID & 2) * 2 - 1));\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";
static const char FS_HEAD[] = GLSL_HEAD "out vec4 o;\n";
static const char FS_MAIN[] =
    "void main() { o = texel(uint(gl_FragCoord.x), uint(gl_FragCoord.y)); }\n";
#if defined(VC3_GLES)
static const char CS_HEAD[] = GLSL_HEAD
    "precision highp image2D;\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(rgba8, binding = 0) writeonly uniform highp image2D u_img;\n"
    "uniform uvec2 u_size;\n";
static const char CS_MAIN[] =
    "void main() {\n"
    "    uvec2 p = gl_GlobalInvocationID.xy;\n"
    "    if (p.x >= u_size.x || p.y >= u_size.y) return;\n"
    "    imageStore(u_img, ivec2(p), texel(p.x, p.y));\n"
    "}\n";
#endif
/* Compare mode, a native compressed texture: texelFetch it into RGBA8. */
static const char FETCH_FS[] = GLSL_HEAD
    "out vec4 o;\nuniform sampler2D u_src;\nuniform int u_lod;\n"
    "void main() {\n"
    "    vec4 v = texelFetch(u_src, ivec2(gl_FragCoord.xy), u_lod);\n"
#if defined(VC3_GLES)
    "    o = v.bgra;\n"
#else
    "    o = v;\n"
#endif
    "}\n";

#define RAW_UNIT 15        /* the raw bytes' buffer texture, bound once */
#define FETCH_UNIT 14      /* compare mode's source texture */

typedef struct {
    GLuint prog;
    GLint fmt, base, pitch, mx, my, w, pal, size;
} DecProg;

static int s_mode = -1;    /* 0 off (CPU), 1 fragment pass, 2 compute */
static int s_s3tc;
static int s_clipext = 1;  /* clip distances exist (core on GL, an extension on ES) */
static GLint s_tbo_max;
static DecProg s_frag, s_comp;
static GLuint s_fetch_prog, s_fetch_lod, s_vao, s_fbo, s_rfbo, s_buf, s_btex, s_nearest;
static GLuint s_scratch;
static uint32_t s_scratch_w, s_scratch_h;
static size_t s_buf_cap;
static uint32_t *s_cpu, *s_gpu;
static size_t s_cmp_cap;
#if !defined(VC3_GLES)
static GLuint s_q[64][1];
static unsigned s_qn;
#endif

static GLuint compile(GLenum type, const char *const *src, int n)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, n, src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[TEXDEC] shader compile failed: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link(GLuint a, GLuint b)
{
    GLuint p = glCreateProgram();
    GLint ok = 0;
    glAttachShader(p, a);
    if (b)
        glAttachShader(p, b);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "[TEXDEC] link failed: %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static void prog_locs(DecProg *d)
{
    GLint cur = 0;
    d->fmt = glGetUniformLocation(d->prog, "u_fmt");
    d->base = glGetUniformLocation(d->prog, "u_base");
    d->pitch = glGetUniformLocation(d->prog, "u_pitch");
    d->mx = glGetUniformLocation(d->prog, "u_mx");
    d->my = glGetUniformLocation(d->prog, "u_my");
    d->w = glGetUniformLocation(d->prog, "u_w");
    d->pal = glGetUniformLocation(d->prog, "u_pal");
    d->size = glGetUniformLocation(d->prog, "u_size");
    glGetIntegerv(GL_CURRENT_PROGRAM, &cur);
    glUseProgram(d->prog);
    glUniform1i(glGetUniformLocation(d->prog, "u_raw"), RAW_UNIT);
    glUseProgram((GLuint)cur);
}

static int has_ext(const char *name)
{
    GLint i, n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (i = 0; i < n; i++) {
        const char *x = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (x && !strcmp(x, name))
            return 1;
    }
    return 0;
}

typedef struct {
    GLint dfbo, prog, vao, vp[4], cwb, unpack;
    GLboolean blend, scissor, cull, cd0, cd1, cd2, dither, cm[4];
} Saved;
static void save_raster(Saved *s);
static void restore_raster(const Saved *s);

/* One throwaway 4x4 decode: drivers compile a program for real at its
 * first draw, and the first texture of a level load took that (12 ms on the
 * Mac) as a hitch. */
static void warm_up(void)
{
    GLint b = 0, prog = 0;
    GLuint t;
    const DecProg *d = s_mode == 2 ? &s_comp : &s_frag;
    Saved sv;

    glGetIntegerv(GL_TEXTURE_BINDING_2D, &b);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
#if defined(VC3_GLES)
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4);
#else
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
#endif
    glUseProgram(d->prog);
    glUniform1i(d->fmt, 0x0C);
    glUniform1ui(d->base, 0);
    glUniform1ui(d->pitch, 0);
    glUniform1ui(d->mx, 0x5);
    glUniform1ui(d->my, 0xA);
    glUniform1ui(d->w, 4);
    if (s_mode == 2) {
#if defined(VC3_GLES)
        glUniform2ui(d->size, 4, 4);
        glBindImageTexture(0, t, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
#endif
    } else {
        save_raster(&sv);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
        glViewport(0, 0, 4, 4);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        restore_raster(&sv);
    }
    glFinish();
    glUseProgram((GLuint)prog);
    glBindTexture(GL_TEXTURE_2D, (GLuint)b);
    glDeleteTextures(1, &t);
}

static void texdec_init(void)
{
    const char *e = ch_env("TEX_GPU"), *c = ch_env("TEX_COMPARE");
    const char *z = ch_env("TEX_S3TC");
    int want = !e ? 1 : !strcmp(e, "frag") ? 2 : !strcmp(e, "compute") ? 3 : *e != '0';
    GLint maj = 0, min = 0, cur = 0, act = 0;
    GLuint vs, fs;

    s_mode = 0;
    s_compare = c && *c && *c != '0';
    if (!want) {
        fprintf(stderr, "[TEXDEC] off (VC3_TEX_GPU=0): textures decode on the CPU\n");
        return;
    }
    glGetIntegerv(GL_MAJOR_VERSION, &maj);
    glGetIntegerv(GL_MINOR_VERSION, &min);
#if defined(VC3_GLES)
    if (maj * 10 + min < 32) {            /* texture buffers are core from ES 3.2 */
        fprintf(stderr, "[TEXDEC] ES %d.%d has no texture buffers: CPU decode\n", maj, min);
        return;
    }
#endif
    glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &s_tbo_max);
    {
        const char *fsrc[3] = { FS_HEAD, DECODE_GLSL, FS_MAIN };
        const char *vsrc[1] = { VS_SRC };
        vs = compile(GL_VERTEX_SHADER, vsrc, 1);
        fs = compile(GL_FRAGMENT_SHADER, fsrc, 3);
        s_frag.prog = vs && fs ? link(vs, fs) : 0;
        if (fs)
            glDeleteShader(fs);
        {
            const char *f2[1] = { FETCH_FS };
            GLuint ff = compile(GL_FRAGMENT_SHADER, f2, 1);
            s_fetch_prog = vs && ff ? link(vs, ff) : 0;
            if (ff)
                glDeleteShader(ff);
        }
        if (vs)
            glDeleteShader(vs);
    }
#if defined(VC3_GLES)
    if (want != 2) {
        const char *csrc[3] = { CS_HEAD, DECODE_GLSL, CS_MAIN };
        GLuint cs = compile(GL_COMPUTE_SHADER, csrc, 3);
        s_comp.prog = cs ? link(cs, 0) : 0;
        if (cs)
            glDeleteShader(cs);
    }
#endif
    if (!s_frag.prog && !s_comp.prog) {
        fprintf(stderr, "[TEXDEC] no decode program: CPU decode\n");
        return;
    }
    if (s_frag.prog)
        prog_locs(&s_frag);
    if (s_comp.prog)
        prog_locs(&s_comp);
    glGetIntegerv(GL_CURRENT_PROGRAM, &cur);
    if (s_fetch_prog) {
        glUseProgram(s_fetch_prog);
        glUniform1i(glGetUniformLocation(s_fetch_prog, "u_src"), FETCH_UNIT);
        s_fetch_lod = (GLuint)glGetUniformLocation(s_fetch_prog, "u_lod");
    }
    glUseProgram((GLuint)cur);
    glGenVertexArrays(1, &s_vao);
    glGenFramebuffers(1, &s_fbo);
    glGenFramebuffers(1, &s_rfbo);
    glGenBuffers(1, &s_buf);
    glGenTextures(1, &s_btex);
    glGenSamplers(1, &s_nearest);
    glSamplerParameteri(s_nearest, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(s_nearest, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    /* The buffer texture stays on its unit for good; glBufferData on the
     * buffer re-specifies what it reads. */
    {
        GLint cwb = 0;
        glGetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, &cwb);
        glBindBuffer(GL_COPY_WRITE_BUFFER, s_buf);
        glBufferData(GL_COPY_WRITE_BUFFER, 4096, NULL, GL_STREAM_DRAW);
        s_buf_cap = 4096;
        glBindBuffer(GL_COPY_WRITE_BUFFER, (GLuint)cwb);
    }
    glGetIntegerv(GL_ACTIVE_TEXTURE, &act);
    glActiveTexture(GL_TEXTURE0 + RAW_UNIT);
    glBindTexture(GL_TEXTURE_BUFFER, s_btex);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, s_buf);
    glActiveTexture((GLenum)act);
#if !defined(VC3_GLES)
    if (g_texstat_on)
        for (cur = 0; cur < 64; cur++)
            glGenQueries(1, s_q[cur]);
#endif
    s_s3tc = z && *z == '1' && has_ext("GL_EXT_texture_compression_s3tc");
#if defined(VC3_GLES)
    s_clipext = has_ext("GL_EXT_clip_cull_distance");
#endif
    s_mode = (s_comp.prog && want != 2) ? 2 : s_frag.prog ? 1 : 0;
    if (want == 3 && !s_comp.prog)
        fprintf(stderr, "[TEXDEC] VC3_TEX_GPU=compute: no compute shader here\n");
    fprintf(stderr, "[TEXDEC] GPU texture decode: %s pass, DXT %s, buffer max %d words%s\n",
            s_mode == 2 ? "compute" : "fragment", s_s3tc ? "uploaded compressed (S3TC)" : "decoded",
            s_tbo_max, s_compare ? "; comparing every upload with the CPU decode" : "");
    if (s_mode)
        warm_up();
    while (glGetError() != GL_NO_ERROR)
        ;
}

int texdec_mode(void)
{
    if (s_mode < 0)
        texdec_init();
    return s_mode;
}

int texdec_immutable(void)
{
    return texdec_mode() == 2;
}

/* The linear twin of a swizzled format (sample_tex's), the DXT codes as they
 * are, or 0 when the shader cannot decode it. *bpp: bytes per texel. */
static uint32_t dec_fmt(uint32_t fmt, uint32_t *bpp)
{
    switch (fmt) {
    case 0x00: case 0x13: *bpp = 1; return 0x13;
    case 0x02: case 0x10: *bpp = 2; return 0x10;
    case 0x03: case 0x1C: *bpp = 2; return 0x1C;
    case 0x04: case 0x1D: *bpp = 2; return 0x1D;
    case 0x05: case 0x11: *bpp = 2; return 0x11;
    case 0x06: case 0x12: *bpp = 4; return 0x12;
    case 0x07: case 0x1E: *bpp = 4; return 0x1E;
    case 0x19: case 0x1F: *bpp = 1; return 0x1F;
    case 0x0B:            *bpp = 1; return 0x0B;
    case 0x3F: case 0x40: case 0x41: *bpp = 4; return fmt;
    case 0x24: case 0x25: *bpp = 2; return fmt;
    case 0x0C: *bpp = 0; return fmt;
    case 0x0E: case 0x0F: *bpp = 0; return fmt;
    default: return 0;
    }
}

/* Bytes of guest memory a w x h level reads, from the level's base. */
static size_t lvl_bytes(const Nv2aTexture *t, uint32_t w, uint32_t h, uint32_t bpp,
                        uint32_t *mx, uint32_t *my)
{
    uint32_t bb = d3d8_format_dxt_block_bytes(t->color);
    if (bb)
        return (size_t)((w + 3) / 4) * ((h + 3) / 4) * bb;
    if (!d3d8_format_is_swizzled(t->color))
        return (size_t)t->pitch * (h - 1) + (size_t)w * bpp;
    xbox_swizzle_masks(w, h, mx, my);
    return ((size_t)(swizzle_deposit(w - 1, *mx) | swizzle_deposit(h - 1, *my)) + 1) * bpp;
}

int texdec_can(const Nv2aTexture *t)
{
    uint32_t bpp;
    if (texdec_mode() <= 0 || !dec_fmt(t->color, &bpp))
        return 0;
    if (t->color == 0x0B && !t->palette)
        return 0;
    if ((t->color == 0x24 || t->color == 0x25) && (t->width & 1))
        return 0;
    return 1;
}

/* What the passes change (Saved), put back after: they run in the middle of
 * gl_backend's draw setup. */

static void save_raster(Saved *s)
{
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->dfbo);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
    glGetIntegerv(GL_VIEWPORT, s->vp);
    glGetBooleanv(GL_COLOR_WRITEMASK, s->cm);
    s->blend = glIsEnabled(GL_BLEND);
    s->scissor = glIsEnabled(GL_SCISSOR_TEST);
    s->cull = glIsEnabled(GL_CULL_FACE);
    s->dither = glIsEnabled(GL_DITHER);
    s->cd0 = s->cd1 = s->cd2 = 0;
    if (s_clipext) {           /* our vertex shader writes no clip distance */
        s->cd0 = glIsEnabled(0x3000 /* CLIP_DISTANCE0 */);
        s->cd1 = glIsEnabled(0x3001);
        s->cd2 = glIsEnabled(0x3002);
        if (s->cd0) glDisable(0x3000);
        if (s->cd1) glDisable(0x3001);
        if (s->cd2) glDisable(0x3002);
    }
    if (s->blend) glDisable(GL_BLEND);
    if (s->scissor) glDisable(GL_SCISSOR_TEST);
    if (s->cull) glDisable(GL_CULL_FACE);
    if (s->dither) glDisable(GL_DITHER);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(s_vao);
}

static void restore_raster(const Saved *s)
{
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)s->dfbo);
    glBindVertexArray((GLuint)s->vao);
    glViewport(s->vp[0], s->vp[1], s->vp[2], s->vp[3]);
    glColorMask(s->cm[0], s->cm[1], s->cm[2], s->cm[3]);
    if (s->blend) glEnable(GL_BLEND);
    if (s->scissor) glEnable(GL_SCISSOR_TEST);
    if (s->cull) glEnable(GL_CULL_FACE);
    if (s->dither) glEnable(GL_DITHER);
    if (s->cd0) glEnable(0x3000);
    if (s->cd1) glEnable(0x3001);
    if (s->cd2) glEnable(0x3002);
}

static void lvl_dims(const Nv2aTexture *t, uint32_t l, uint32_t *w, uint32_t *h)
{
    *w = t->width >> l;
    *h = t->height >> l;
    if (!*w) *w = 1;
    if (!*h) *h = 1;
}

/* Storage for `levels` levels of RGBA8, if `a` says the texture has none
 * of that shape yet. */
static int alloc_levels(TexdecAlloc *a, const Nv2aTexture *t, uint32_t levels, int full)
{
    uint32_t l, w, h, n = levels;
    if (full) {                            /* glGenerateMipmap fills the chain */
        uint32_t m = t->width > t->height ? t->width : t->height;
        for (n = 1; m > 1; m >>= 1)
            n++;
    }
    if (a->w == t->width && a->h == t->height && (a->levels == n || (a->immut && a->levels >= n)))
        return 1;
    if (a->immut)          /* a new shape needs a new name (gl_backend's job) */
        return 0;
#if defined(VC3_GLES)
    if (texdec_immutable()) {
        /* gl_backend gives an immutable texture a new name before any
         * other shape (texdec_upload's precondition), so only the first
         * allocation gets here. */
        glTexStorage2D(GL_TEXTURE_2D, (GLsizei)n, GL_RGBA8, (GLsizei)t->width,
                       (GLsizei)t->height);
        a->immut = 1;
    } else
#endif
    {
        for (l = 0; l < n; l++) {
            lvl_dims(t, l, &w, &h);
            glTexImage2D(GL_TEXTURE_2D, (GLint)l, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        }
    }
    a->w = t->width;
    a->h = t->height;
    a->levels = n;
    return 1;
}

static int compare_level(GLuint tex, const Nv2aTexture *t, uint32_t l, size_t off,
                         int native);

/* Decode t into `tex`, bound to GL_TEXTURE_2D on the active unit. */
int texdec_upload(GLuint tex, TexdecAlloc *a, const Nv2aTexture *t, int mips, int *mipped)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = 0, fmt = dec_fmt(t->color, &bpp), bb = d3d8_format_dxt_block_bytes(t->color);
    uint32_t levels = 1, l, w, h, mx = 0, my = 0, maxl = 0, m;
    size_t off[16], total = 0, pal = 0, n;
    int native = bb && s_s3tc, gen = 0, path;
    uint64_t tb[4] = { 0, 0, 0, 0 };
    GLint unpack = 0;

    *mipped = 0;
    if (!texdec_can(t))
        return 0;
    for (m = t->width > t->height ? t->width : t->height; m > 1; m >>= 1)
        maxl++;
    if (mips) {
        levels = t->levels > 1 ? t->levels : 1;
        if (levels > maxl + 1)
            levels = maxl + 1;
        if (levels > 16)
            levels = 16;
        gen = levels < 2 && !native;
        *mipped = levels > 1 || gen;
    }
    /* Levels follow each other packed, each w*h*bpp (or its blocks), as
     * gl_backend's level_bytes has them; a non-power-of-two swizzled level
     * may read past that, so the copy covers what the last one reads. */
    for (l = 0, n = 0; l < levels; l++) {
        size_t r;
        lvl_dims(t, l, &w, &h);
        off[l] = n;
        r = lvl_bytes(t, w, h, bpp, &mx, &my);
        if (off[l] + r > total)
            total = off[l] + r;
        n += (bb || !d3d8_format_is_swizzled(t->color)) ? r : (size_t)w * h * bpp;
    }
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
    if (unpack)
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    if (native) {
        static const GLenum CF[3] = { GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
                                      GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,
                                      GL_COMPRESSED_RGBA_S3TC_DXT5_EXT };
        GLenum cf = CF[t->color == 0x0C ? 0 : t->color == 0x0E ? 1 : 2];
        if (a->immut)
            return unpack ? (glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)unpack), 0) : 0;
        for (l = 0; l < levels; l++) {
            lvl_dims(t, l, &w, &h);
            n = (size_t)((w + 3) / 4) * ((h + 3) / 4) * bb;
            glCompressedTexImage2D(GL_TEXTURE_2D, (GLint)l, cf, (GLsizei)w, (GLsizei)h, 0,
                                   (GLsizei)n, mem + t->offset + off[l]);
        }
        a->w = t->width;
        a->h = t->height;
        a->levels = levels;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)levels - 1);
#if defined(VC3_GLES)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_BLUE);
#endif
        path = 2;
    } else {
        GLint cwb = 0;
        size_t base = t->offset & 3u, bytes;
        const DecProg *d = s_mode == 2 ? &s_comp : &s_frag;
        if (t->color == 0x0B) {
            pal = (base + total + 3) & ~(size_t)3;
            bytes = pal + 1024;
        } else {
            bytes = (base + total + 3) & ~(size_t)3;
        }
        if (bytes / 4 > (size_t)s_tbo_max || (a->immut && (a->w != t->width
            || a->h != t->height || a->levels < (gen ? 2u : levels)))) {
            if (unpack)
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)unpack);
            return 0;
        }
        if (g_texstat_on > 1)
            tb[0] = texstat_now();
        glGetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, &cwb);
        glBindBuffer(GL_COPY_WRITE_BUFFER, s_buf);
        /* Orphan the last upload's store (the GPU may still be reading it)
         * and copy this one in. */
        if (bytes > s_buf_cap)
            s_buf_cap = bytes + bytes / 4;
        glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)s_buf_cap, NULL, GL_STREAM_DRAW);
        glBufferSubData(GL_COPY_WRITE_BUFFER, 0, (GLsizeiptr)(base + total),
                        mem + t->offset - base);
        if (pal)
            glBufferSubData(GL_COPY_WRITE_BUFFER, (GLintptr)pal, 1024, mem + t->palette);
        glBindBuffer(GL_COPY_WRITE_BUFFER, (GLuint)cwb);

        if (g_texstat_on > 1)
            tb[1] = texstat_now();
        alloc_levels(a, t, levels, gen);   /* cannot fail: checked above */
        if (g_texstat_on > 1)
            tb[2] = texstat_now();
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, gen ? 1000 : (GLint)levels - 1);
#if defined(VC3_GLES)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);   /* es_bgra */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
#endif
        {
            GLint prog = 0;
            Saved sv;
            glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
            glUseProgram(d->prog);
#if !defined(VC3_GLES)
            if (g_texstat_on)      /* not with VC3_GL_GPUTIME's own query open */
                glBeginQuery(GL_TIME_ELAPSED, s_q[s_qn & 63][0]);
#endif
            if (s_mode == 1) {
                save_raster(&sv);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo);
            }
            glUniform1i(d->fmt, (GLint)fmt);
            glUniform1ui(d->pitch, d3d8_format_is_swizzled(t->color) ? 0u : t->pitch);
            glUniform1ui(d->pal, (GLuint)pal);
            for (l = 0; l < levels; l++) {
                lvl_dims(t, l, &w, &h);
                xbox_swizzle_masks(w, h, &mx, &my);
                glUniform1ui(d->base, (GLuint)(base + off[l]));
                glUniform1ui(d->mx, mx);
                glUniform1ui(d->my, my);
                glUniform1ui(d->w, w);
                if (s_mode == 2) {
#if defined(VC3_GLES)
                    glUniform2ui(d->size, w, h);
                    glBindImageTexture(0, tex, (GLint)l, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
                    glDispatchCompute((w + 7) / 8, (h + 7) / 8, 1);
#endif
                } else {
                    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                           GL_TEXTURE_2D, tex, (GLint)l);
                    glViewport(0, 0, (GLsizei)w, (GLsizei)h);
                    glDrawArrays(GL_TRIANGLES, 0, 3);
                }
            }
            if (s_mode == 1) {
                glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, 0, 0);
                restore_raster(&sv);
            }
#if defined(VC3_GLES)
            if (s_mode == 2)
                glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT
                                | GL_FRAMEBUFFER_BARRIER_BIT);
#else
            if (g_texstat_on) {
                GLuint64 ns = 0;
                glEndQuery(GL_TIME_ELAPSED);
                s_qn++;
                /* 63 uploads on, the oldest query is long done */
                if (s_qn > 63) {
                    glGetQueryObjectui64v(s_q[s_qn & 63][0], GL_QUERY_RESULT, &ns);
                    s_ts.gpu_ns += ns;
                    s_ts.gpu_passes++;
                }
            }
#endif
            glUseProgram((GLuint)prog);
        }
        if (g_texstat_on > 1)
            tb[3] = texstat_now();
        if (gen)
            glGenerateMipmap(GL_TEXTURE_2D);
        path = 1;
        if (g_texstat_on > 1 && texstat_now() - tb[0] > 1000000)
            fprintf(stderr, "[TEXSTAT] slow upload %08X %ux%u fmt %02X levels %u: copy %.2f "
                    "alloc %.2f pass %.2f mips %.2f ms\n", t->offset, t->width, t->height,
                    t->color & 0xFF, levels, (tb[1] - tb[0]) / 1e6, (tb[2] - tb[1]) / 1e6,
                    (tb[3] - tb[2]) / 1e6, (texstat_now() - tb[3]) / 1e6);
    }
    if (unpack)
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)unpack);
    if (s_compare) {
        int bad = 0, done = 0, r;
        for (l = 0; l < levels; l++) {
            r = compare_level(tex, t, l, off[l], native);
            done |= r >= 0;
            bad |= r > 0;
        }
        if (done) {
            s_cmp.tex[t->color & 0xFF]++;
            s_cmp.bad_tex[t->color & 0xFF] += bad;
        }
    }
    return path;
}

/* ── Compare mode ──────────────────────────────────────────── */

static void scratch(uint32_t w, uint32_t h)
{
    GLint b = 0;
    if (s_scratch && s_scratch_w >= w && s_scratch_h >= h)
        return;
    if (!s_scratch)
        glGenTextures(1, &s_scratch);
    s_scratch_w = w > s_scratch_w ? w : s_scratch_w;
    s_scratch_h = h > s_scratch_h ? h : s_scratch_h;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &b);
    glBindTexture(GL_TEXTURE_2D, s_scratch);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)s_scratch_w, (GLsizei)s_scratch_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, (GLuint)b);
}

/* Level l of `tex` (as uploaded) against the CPU decoder's level l: the
 * texels that differ, or -1 when it cannot be compared. */
static int compare_level(GLuint tex, const Nv2aTexture *t, uint32_t l, size_t off, int native)
{
    uint32_t w, h, i, f = t->color & 0xFF, worst = 0, bad = 0, first = 0;
    size_t n;
    Nv2aTexture lt = *t;
    GLint rfbo = 0, pack = 0;

    lvl_dims(t, l, &w, &h);
    n = (size_t)w * h;
    if (n > s_cmp_cap) {
        free(s_cpu);
        free(s_gpu);
        s_cpu = (uint32_t *)malloc(n * 4);
        s_gpu = (uint32_t *)malloc(n * 4);
        s_cmp_cap = s_cpu && s_gpu ? n : 0;
        if (!s_cmp_cap)
            return -1;
    }
    lt.offset = t->offset + (uint32_t)off;
    lt.width = w;
    lt.height = h;
    lt.levels = 1;
    if (!nv2a_backend_decode_texture(&lt, s_cpu)) {
        s_cmp.skipped++;
        return -1;
    }
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfbo);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    if (pack)
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_rfbo);
    if (native) {
        /* Not colour-renderable: texelFetch it into the scratch texture. */
        GLint prog = 0, act = 0, b14 = 0;
        Saved sv;
        scratch(w, h);
        glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &act);
        glActiveTexture(GL_TEXTURE0 + FETCH_UNIT);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &b14);
        glBindTexture(GL_TEXTURE_2D, tex);
        glBindSampler(FETCH_UNIT, s_nearest);
        save_raster(&sv);
        glUseProgram(s_fetch_prog);
        glUniform1i((GLint)s_fetch_lod, (GLint)l);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               s_scratch, 0);
        glViewport(0, 0, (GLsizei)w, (GLsizei)h);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        restore_raster(&sv);
        glUseProgram((GLuint)prog);
        glBindSampler(FETCH_UNIT, 0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)b14);
        glActiveTexture((GLenum)act);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               s_scratch, 0);
    } else {
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               tex, (GLint)l);
    }
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, s_gpu);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfbo);
    if (pack)
        glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);

    for (i = 0; i < n; i++) {
        uint32_t a = s_cpu[i], b = s_gpu[i], d = 0, k;
        if (a == b)
            continue;
        for (k = 0; k < 32; k += 8) {
            int x = (int)((a >> k) & 0xFF) - (int)((b >> k) & 0xFF);
            uint32_t ad = (uint32_t)(x < 0 ? -x : x);
            if (ad > d)
                d = ad;
        }
        if (d > worst)
            worst = d;
        /* a DXT texel the driver decoded (S3TC) may round one apart */
        if (native && d <= 1)
            continue;
        if (!bad)
            first = i;
        bad++;
    }
    s_cmp.levels[f]++;
    s_cmp.texels[f] += n;
    if (worst > s_cmp.maxdiff[f])
        s_cmp.maxdiff[f] = worst;
    s_cmp.bad_texels[f] += bad;
    if (bad && s_cmp.logged++ < 40)
        fprintf(stderr, "[TEXCMP] MISMATCH %08X %ux%u fmt %02X level %u%s: %u texels, max "
                "diff %u; first (%u,%u) cpu %08X gpu %08X\n", t->offset, w, h, f, l,
                native ? " (S3TC)" : "", bad, worst, first % w, first / w, s_cpu[first],
                s_gpu[first]);
    return (int)bad;
}
