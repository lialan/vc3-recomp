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
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend.h"
#include "../d3d/d3d8_swizzle.h"   /* relative to src/kernel, as the executor */

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* ── Context and shaders ───────────────────────────────────── */

static CGLContextObj s_ctx;
static int s_failed;
static GLuint s_prog, s_vao, s_vbo, s_white;
static GLint u_surf, u_depth, u_texsize, u_textured, u_atest, u_afunc, u_aref;
static uint32_t s_frame = 1;

/* VC3_GL_STATS=1: every 300 flips, what the batches asked for. */
static struct {
    int on;
    uint32_t draws, tris, textured, rt, uploads, untex, rt_logged;
    uint32_t fail[256];        /* failed decodes by format code */
    uint32_t fmt[256];         /* textured batches by format code */
} s_st;

static const char *VS =
    "#version 410\n"
    "layout(location=0) in vec4 a_pos;\n"     /* x, y, z, rhw */
    "layout(location=1) in vec4 a_col;\n"     /* B, G, R, A bytes */
    "layout(location=2) in vec2 a_uv;\n"      /* texels */
    "uniform vec2 u_surf;\n"                  /* framebuffer size */
    "uniform vec2 u_depth;\n"                 /* clip min, max */
    "uniform vec2 u_texsize;\n"
    "out vec4 v_col;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  float w = (a_pos.w > 0.0 && a_pos.w < 1e30) ? 1.0 / a_pos.w : 1.0;\n"
    "  vec2 ndc = a_pos.xy / u_surf * 2.0 - 1.0;\n"
    "  float z = clamp((a_pos.z - u_depth.x) / (u_depth.y - u_depth.x), 0.0, 1.0) * 2.0 - 1.0;\n"
    "  gl_Position = vec4(ndc * w, z * w, w);\n"
    "  v_col = a_col.zyxw;\n"
    "  v_uv = a_uv / u_texsize;\n"
    "}\n";

static const char *FS =
    "#version 410\n"
    "in vec4 v_col;\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_textured;\n"
    "uniform int u_atest;\n"
    "uniform int u_afunc;\n"                  /* GL_NEVER 0x200 .. GL_ALWAYS 0x207 */
    "uniform float u_aref;\n"
    "out vec4 o_col;\n"
    "void main() {\n"
    "  vec4 c = v_col;\n"
    "  if (u_textured != 0) c *= texture(u_tex, v_uv);\n"
    "  if (u_atest != 0) {\n"
    "    float a = c.a; bool pass = true;\n"
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
}

/* ── Textures ──────────────────────────────────────────────── */

#define TEX_SLOTS 2048
typedef struct {
    uint32_t offset, w, h, fmt, pitch;
    uint64_t hash;
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

static GLint gl_wrap(uint32_t mode)
{
    switch (mode) {
    case 2:  return GL_MIRRORED_REPEAT;
    case 3:  return GL_CLAMP_TO_EDGE;
    case 4:  return GL_CLAMP_TO_BORDER;
    default: return GL_REPEAT;
    }
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
    if (rt && rt->fbo) {
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
            e->checked = 0;
        }
        glBindTexture(GL_TEXTURE_2D, e->tex);
        if (e->checked != s_frame) {
            uint64_t h = hash_bytes(mem + t->offset, tex_bytes(t));
            e->checked = s_frame;
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
    return 1;
}

/* ── State ─────────────────────────────────────────────────── */

static void apply_state(const Nv2aRenderState *rs)
{
    static int cull_mode = -1;
    uint32_t m = rs->color_mask;

    if (cull_mode < 0) {
        const char *e = getenv("VC3_GL_CULL");
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
    glUniform1i(u_atest, rs->alpha_test_enable ? 1 : 0);
    glUniform1i(u_afunc, (GLint)rs->alpha_func);
    glUniform1f(u_aref, (float)(rs->alpha_ref & 0xFF) / 255.0f);
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
    if (bits)
        glClear(bits);
    f->dirty = 1;
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
    if (b->texture && b->texture->offset != f->va)
        textured = tex_bind(b->texture, &tw, &th);
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
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(b->count * sizeof(Nv2aVertex)),
                 b->vertices, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)b->count);
    f->dirty = 1;
}

static void gl_flip(void)
{
    int i;

    if (!gl_ready())
        return;
    for (i = 0; i < s_nsurfs; i++)
        if (s_surfs[i].dirty)
            surf_readback(&s_surfs[i]);
    s_frame++;
    if (s_st.on && s_frame % 300 == 0) {
        fprintf(stderr, "[GL] %u flips: %u draws (%u tris), %u textured, %u "
                "no texture, %u from surfaces, %u uploads; surfaces:",
                300u, s_st.draws, s_st.tris, s_st.textured, s_st.untex,
                s_st.rt, s_st.uploads);
        for (i = 0; i < s_nsurfs; i++)
            fprintf(stderr, " %08X/%dx%d", s_surfs[i].va, s_surfs[i].w,
                    s_surfs[i].h);
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
    const char *e = getenv("VC3_GL");

    if (e && e[0] == '0') {
        fprintf(stderr, "[GL] off (VC3_GL=0): software rasteriser\n");
        return;
    }
    s_st.on = getenv("VC3_GL_STATS") != NULL;
    nv2a_backend_register(&s_gl_backend);
    fprintf(stderr, "[GL] OpenGL back end registered\n");
}
