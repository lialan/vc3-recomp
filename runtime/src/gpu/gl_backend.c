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
 * game. Textures decode on the GPU (gl_texdec.c; VC3_TEX_GPU=0: on the CPU,
 * through nv2a_backend_decode_texture).
 */
#if defined(VC3_GLES)
/* Meta Quest (VC3_GLES): the same back end on OpenGL ES 3.2, in an EGL
 * context the Quest host makes (src/android/quest_host.c), sharing its
 * objects with the host's so a frame reaches the headset as a texture.
 * What desktop GL 4.1 has and ES lacks:
 *
 *   BGRA pixels     ES moves RGBA bytes. Decoded textures are uploaded as
 *                   they are and swizzled back when sampled (es_bgra), and
 *                   reads into guest memory swap R and B on the copy
 *                   (es_to_guest). GL_BGRA / 8_8_8_8_REV below name RGBA /
 *                   unsigned byte, so the calls read the same.
 *   BGRA attributes size GL_BGRA has no ES form: D3DCOLOR attributes go in
 *                   as RGBA and the shader swaps them (u_bgra).
 *   GLSL 4.10       es_source() puts an ES 3.20 prologue in its place; the
 *                   sources are otherwise valid for both.
 *   clip distances, depth clamp
 *                   EXT_clip_cull_distance and EXT_depth_clamp, where the
 *                   driver has them; without, GL's own near-plane clip
 *                   stands in.
 */
#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#define GL_BGRA                     GL_RGBA
#define GL_UNSIGNED_INT_8_8_8_8_REV GL_UNSIGNED_BYTE
#ifndef GL_DEPTH_CLAMP
#define GL_DEPTH_CLAMP              0x864F      /* EXT_depth_clamp */
#endif
#ifndef GL_CLIP_DISTANCE0
#define GL_CLIP_DISTANCE0           0x3000      /* EXT_clip_cull_distance */
#define GL_CLIP_DISTANCE1           0x3001
#endif
#define GL_SAMPLES_PASSED           GL_ANY_SAMPLES_PASSED
#define glClearDepth(d)             glClearDepthf((GLfloat)(d))
extern int vc3_quest_gl_context(void);         /* src/android/quest_host.c */
struct Vc3VrFrame_;
extern void vc3_quest_frame(GLuint fbo, int pw, int ph, int eyes, int lw, int lh,
                            const void *vr_frame);   /* a Vc3VrFrame (vr_view.h) */
#else
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#endif
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nv2a_backend.h"
#include "gl_texdec.h"
#include "nv2a_vsh_glsl.h"
#include "nv2a_vsh_interp.h"
#include "host/display_settings.h"
#include "host/vr_math.h"
#include "host/vr_view.h"

static void es_bgra(GLenum target);
#include "../d3d/d3d8_swizzle.h"   /* relative to src/kernel, as the executor */
#include "chihiro/ch_env.h"
#include "chihiro/chihiro_profile.h"

/* ── Shadow state ───────────────────────────────────────────
 * The fixed-function state, the framebuffer bindings and the viewport as
 * last set on this context: a call that would set them to what they are is
 * not made. Each draw rebound its surface's framebuffer and viewport (one-
 * pass stereo set the viewport twice more), and a change of render state
 * re-sent all of it -- a dozen driver calls validated per draw on the GL
 * thread, now the limit. Everything below this point calls these through
 * the macros; gl_texdec.c restores what it changes, and the host's frame
 * hand-off (vc3_quest_frame) does not, so sh_reset after it. Unknown values
 * (~0) never match. */
enum { SH_BLEND, SH_DEPTH, SH_STENCIL, SH_CULL, SH_SCISSOR, SH_CAPS };
static struct {
    GLuint draw_fb, read_fb;
    GLint vp[4];
    signed char cap[SH_CAPS];              /* -1 unknown */
    GLenum bsrc, bdst, bsrc_a, bdst_a, beq, dfunc, cface, fface;
    GLfloat bcol[4];
    signed char dmask;
    signed char cmask[4];
    GLenum sfunc, sfail, szfail, szpass;
    GLint sref;
    GLuint smask_func, smask_write;
    int valid;
    uint32_t made, skipped;                /* this second (the GPU time log) */
} s_sh;

static void sh_reset(void)
{
    uint32_t made = s_sh.made, skipped = s_sh.skipped;
    memset(&s_sh, 0xFF, sizeof s_sh);
    s_sh.valid = 0;
    s_sh.made = made;
    s_sh.skipped = skipped;
}

static int sh_cap(GLenum cap)
{
    switch (cap) {
    case GL_BLEND: return SH_BLEND;
    case GL_DEPTH_TEST: return SH_DEPTH;
    case GL_STENCIL_TEST: return SH_STENCIL;
    case GL_CULL_FACE: return SH_CULL;
    case GL_SCISSOR_TEST: return SH_SCISSOR;
    default: return -1;
    }
}

static void sh_glEnable(GLenum cap)
{
    int i = sh_cap(cap);
    if (i >= 0) {
        if (s_sh.cap[i] == 1)
            { s_sh.skipped++; return; }
        s_sh.cap[i] = 1;
    }
    s_sh.made++;
    glEnable(cap);
}

static void sh_glDisable(GLenum cap)
{
    int i = sh_cap(cap);
    if (i >= 0) {
        if (s_sh.cap[i] == 0)
            { s_sh.skipped++; return; }
        s_sh.cap[i] = 0;
    }
    s_sh.made++;
    glDisable(cap);
}

static void sh_glBindFramebuffer(GLenum target, GLuint fb)
{
    if (target == GL_FRAMEBUFFER) {
        if (s_sh.draw_fb == fb && s_sh.read_fb == fb)
            { s_sh.skipped++; return; }
        s_sh.draw_fb = s_sh.read_fb = fb;
    } else if (target == GL_DRAW_FRAMEBUFFER) {
        if (s_sh.draw_fb == fb)
            { s_sh.skipped++; return; }
        s_sh.draw_fb = fb;
    } else if (target == GL_READ_FRAMEBUFFER) {
        if (s_sh.read_fb == fb)
            { s_sh.skipped++; return; }
        s_sh.read_fb = fb;
    }
    s_sh.made++;
    glBindFramebuffer(target, fb);
}

static void sh_glDeleteFramebuffers(GLsizei n, const GLuint *fbs)
{
    GLsizei i;
    for (i = 0; i < n; i++) {           /* a bound one unbinds: unknown */
        if (fbs[i] == s_sh.draw_fb)
            s_sh.draw_fb = ~0u;
        if (fbs[i] == s_sh.read_fb)
            s_sh.read_fb = ~0u;
    }
    glDeleteFramebuffers(n, fbs);
}

static void sh_glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (s_sh.vp[0] == x && s_sh.vp[1] == y && s_sh.vp[2] == w && s_sh.vp[3] == h)
        { s_sh.skipped++; return; }
    s_sh.vp[0] = x;
    s_sh.vp[1] = y;
    s_sh.vp[2] = w;
    s_sh.vp[3] = h;
    s_sh.made++;
    glViewport(x, y, w, h);
}

static void sh_glBlendFuncSeparate(GLenum s, GLenum d, GLenum sa, GLenum da)
{
    if (s_sh.bsrc == s && s_sh.bdst == d && s_sh.bsrc_a == sa && s_sh.bdst_a == da)
        { s_sh.skipped++; return; }
    s_sh.bsrc = s;
    s_sh.bdst = d;
    s_sh.bsrc_a = sa;
    s_sh.bdst_a = da;
    s_sh.made++;
    glBlendFuncSeparate(s, d, sa, da);
}

static void sh_glBlendFunc(GLenum s, GLenum d)
{
    sh_glBlendFuncSeparate(s, d, s, d);
}

static void sh_glBlendEquation(GLenum eq)
{
    if (s_sh.beq == eq)
        { s_sh.skipped++; return; }
    s_sh.beq = eq;
    s_sh.made++;
    glBlendEquation(eq);
}

static void sh_glBlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    if (s_sh.bcol[0] == r && s_sh.bcol[1] == g && s_sh.bcol[2] == b && s_sh.bcol[3] == a)
        { s_sh.skipped++; return; }
    s_sh.bcol[0] = r;
    s_sh.bcol[1] = g;
    s_sh.bcol[2] = b;
    s_sh.bcol[3] = a;
    s_sh.made++;
    glBlendColor(r, g, b, a);
}

static void sh_glDepthFunc(GLenum f)
{
    if (s_sh.dfunc == f)
        { s_sh.skipped++; return; }
    s_sh.dfunc = f;
    s_sh.made++;
    glDepthFunc(f);
}

static void sh_glDepthMask(GLboolean m)
{
    if (s_sh.dmask == (signed char)(m != 0))
        { s_sh.skipped++; return; }
    s_sh.dmask = (signed char)(m != 0);
    s_sh.made++;
    glDepthMask(m);
}

static void sh_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    if (s_sh.cmask[0] == (r != 0) && s_sh.cmask[1] == (g != 0) && s_sh.cmask[2] == (b != 0)
        && s_sh.cmask[3] == (a != 0))
        { s_sh.skipped++; return; }
    s_sh.cmask[0] = (signed char)(r != 0);
    s_sh.cmask[1] = (signed char)(g != 0);
    s_sh.cmask[2] = (signed char)(b != 0);
    s_sh.cmask[3] = (signed char)(a != 0);
    s_sh.made++;
    glColorMask(r, g, b, a);
}

static void sh_glStencilFunc(GLenum f, GLint ref, GLuint mask)
{
    if (s_sh.sfunc == f && s_sh.sref == ref && s_sh.smask_func == mask)
        { s_sh.skipped++; return; }
    s_sh.sfunc = f;
    s_sh.sref = ref;
    s_sh.smask_func = mask;
    s_sh.made++;
    glStencilFunc(f, ref, mask);
}

static void sh_glStencilMask(GLuint mask)
{
    if (s_sh.smask_write == mask)
        { s_sh.skipped++; return; }
    s_sh.smask_write = mask;
    s_sh.made++;
    glStencilMask(mask);
}

static void sh_glStencilOp(GLenum fail, GLenum zfail, GLenum zpass)
{
    if (s_sh.sfail == fail && s_sh.szfail == zfail && s_sh.szpass == zpass)
        { s_sh.skipped++; return; }
    s_sh.sfail = fail;
    s_sh.szfail = zfail;
    s_sh.szpass = zpass;
    s_sh.made++;
    glStencilOp(fail, zfail, zpass);
}

static void sh_glCullFace(GLenum f)
{
    if (s_sh.cface == f)
        { s_sh.skipped++; return; }
    s_sh.cface = f;
    s_sh.made++;
    glCullFace(f);
}

static void sh_glFrontFace(GLenum f)
{
    if (s_sh.fface == f)
        { s_sh.skipped++; return; }
    s_sh.fface = f;
    s_sh.made++;
    glFrontFace(f);
}

#define glEnable sh_glEnable
#define glDisable sh_glDisable
#define glBindFramebuffer sh_glBindFramebuffer
#define glDeleteFramebuffers sh_glDeleteFramebuffers
#define glViewport sh_glViewport
#define glBlendFuncSeparate sh_glBlendFuncSeparate
#define glBlendFunc sh_glBlendFunc
#define glBlendEquation sh_glBlendEquation
#define glBlendColor sh_glBlendColor
#define glDepthFunc sh_glDepthFunc
#define glDepthMask sh_glDepthMask
#define glColorMask sh_glColorMask
#define glStencilFunc sh_glStencilFunc
#define glStencilMask sh_glStencilMask
#define glStencilOp sh_glStencilOp
#define glCullFace sh_glCullFace
#define glFrontFace sh_glFrontFace


/* getenv, looked up once per name: several debug switches are tested per
 * draw, and getenv walks the whole environment each time. They do not change
 * while the game runs, and this all runs on the executor thread. */
static const char *genv(const char *name)
{
    /* Called per draw in places: the names are literals, so a pointer
     * compare finds them; past 32 names each call was a getenv scan. */
    static struct { const char *name, *value; } cache[128];
    static int n;
    int i;
    for (i = 0; i < n; i++)
        if (cache[i].name == name)
            return cache[i].value;
    for (i = 0; i < n; i++)
        if (!strcmp(cache[i].name, name))
            return cache[i].value;
    if (n < (int)(sizeof cache / sizeof cache[0])) {
        cache[n].name = name;
        cache[n].value = ch_env(name);
        return cache[n++].value;
    }
    return ch_env(name);
}

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* ── Context and shaders ───────────────────────────────────── */

/* Clip distances and depth clamp: core on desktop GL 4.1, extensions on ES
 * (where enabling an absent one is a GL error per draw). */
static int s_has_clip = 1, s_has_dclamp = 1;

#if defined(VC3_GLES)
static int s_ctx;                  /* the host's EGL context is current */
#else
static CGLContextObj s_ctx;
#endif
static int s_failed;
static GLuint s_vao, s_vao_raw, s_vao_vp, s_white;
static GLuint s_fs_obj, s_fs_rc_obj;     /* linked into each program shader */
static GLuint s_fs_na_obj, s_fs_rc_na_obj;   /* the same without the alpha test */

/* A linked program and its uniform locations, looked up once at link time:
 * glGetUniformLocation per draw was a string search per call. The fragment
 * shader is shared, so every program has the fragment uniforms; vertex
 * uniforms a program lacks read -1, which GL ignores. */
typedef struct {
    GLuint prog;
    GLint surf, depth, texsize, textured, atest, afunc, aref, xs, eye, sbs;
    GLint rc, cicw, aicw, cocw, aocw, f0, f1, fin0, fin1, ctl, sprog, fc0, fc1,
          fogc, on, ts, iscube, dbgon, dbgcol, lmap, rcdbg;
    /* The fixed-function vertex shader (Nv2aFixedState). */
    GLint comp, mv, proj, tm, c59, skin, usecomp, lighting, lkind, light, samb,
          matalpha, colmat, flipn, hasnrm, hascol, texgen, tgon, tmon, ascale,
          packed, aa, vpz, nearz, fogen, fogmode, fogp, uvs0, bgra;
    /* The immersive view (vr_view.h): this eye's matrix, the game's
     * projection, on / 1 / pz. */
    GLint immvp, gp, gw;
    GLint st2, eye2, immvp2;     /* one-pass stereo */
    /* the fixed-function state last sent to this program (raw_uniforms) */
    Nv2aFixedState ff_last;
    int ff_valid;
    /* u_ascale as last sent (16 slots: past the uniform cache's size) */
    GLfloat ascale_last[16][4];
    int ascale_valid;
    /* every uniform's last value sent from the draw path (cu*): a value
     * sent again is not sent -- each one is driver work at the draw */
    struct UCache *uc;
} Prog;
/* Two of each: the plain fragment shader (texture * colour, the lightmap,
 * the alpha test) for nearly every batch, and the one with the register
 * combiners (RC) for the few that need them. One shader with both had the
 * combiners' registers on every pixel: half the GPU time of the mission
 * intro's thousand draws on the Quest. */
static Prog s_pcpu, s_praw, s_pcpu_rc, s_praw_rc, *U = &s_pcpu;
/* And each again without the alpha test (NOAT; prog 0 if it failed), for
 * the batches that have it off -- nearly all: a shader that can discard
 * leaves the depth test until after it has run, so every hidden fragment of
 * every batch was shaded, and a tiler cannot reject them early either. */
static Prog s_pcpu_na, s_praw_na, s_pcpu_rc_na, s_praw_rc_na;

/* ── Uniforms sent only when they change ──────────────────────────────────
   The draw path sets some thirty uniforms a batch, most of them the same as
   the batch before; on the Quest's driver each call is work at the next
   draw (a thousand draws a frame in the mission intro). cu*() are glUniform*
   with the same arguments that keep, per program, the bytes last sent for
   each location and skip a call that would send them again. Anything that
   sets the current program's uniforms another way calls uc_reset. */
#define UC_SLOTS 128
#define UC_BYTES 640                    /* the largest: 40 light vec4s */
struct UCache {
    struct { GLint loc; uint16_t n; uint8_t v[UC_BYTES]; } e[UC_SLOTS];
};

static void uc_reset(Prog *p)
{
    p->ascale_valid = 0;
    if (p->uc)
        memset(p->uc, 0xFF, sizeof *p->uc);    /* loc -1 everywhere: empty */
}

/* 1 if loc already holds these bytes (or is not a uniform); else records them. */
static int uc_same_(GLint loc, const void *d, size_t n)
{
    struct UCache *c;
    unsigned h, k;
    static int off = -1;
    if (loc < 0)
        return 1;
    if (off < 0)
        off = genv("UCACHE") && genv("UCACHE")[0] == '0';
    if (n > UC_BYTES || off)
        return 0;
    if (!U->uc) {
        U->uc = (struct UCache *)malloc(sizeof *U->uc);
        if (!U->uc)
            return 0;
        uc_reset(U);
    }
    c = U->uc;
    h = ((unsigned)loc * 2654435761u) >> 25;  /* 0..127 */
    for (k = 0; k < UC_SLOTS; k++) {
        unsigned i = (h + k) & (UC_SLOTS - 1);
        if (c->e[i].loc == loc) {
            if (c->e[i].n == n && !memcmp(c->e[i].v, d, n))
                return 1;
            c->e[i].n = (uint16_t)n;
            memcpy(c->e[i].v, d, n);
            return 0;
        }
        if (c->e[i].loc == -1) {
            c->e[i].loc = loc;
            c->e[i].n = (uint16_t)n;
            memcpy(c->e[i].v, d, n);
            return 0;
        }
    }
    return 0;                               /* full: send */
}

/* Uniform calls made and not repeated (the GPU time log). */
static uint32_t s_uc_sent, s_uc_kept, s_vc_sent;
static int uc_same(GLint loc, const void *d, size_t n)
{
    int same = uc_same_(loc, d, n);
    if (loc >= 0) {
        if (same)
            s_uc_kept++;
        else
            s_uc_sent++;
    }
    return same;
}

static void cu1i(GLint l, GLint a) { if (!uc_same(l, &a, sizeof a)) glUniform1i(l, a); }
static void cu1ui(GLint l, GLuint a) { if (!uc_same(l, &a, sizeof a)) glUniform1ui(l, a); }
static void cu1f(GLint l, GLfloat a) { if (!uc_same(l, &a, sizeof a)) glUniform1f(l, a); }
static void cu2f(GLint l, GLfloat a, GLfloat b)
{ GLfloat v[2] = { a, b }; if (!uc_same(l, v, sizeof v)) glUniform2f(l, a, b); }
static void cu3f(GLint l, GLfloat a, GLfloat b, GLfloat c)
{ GLfloat v[3] = { a, b, c }; if (!uc_same(l, v, sizeof v)) glUniform3f(l, a, b, c); }
static void cu4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d)
{ GLfloat v[4] = { a, b, c, d }; if (!uc_same(l, v, sizeof v)) glUniform4f(l, a, b, c, d); }
static void cu2i(GLint l, GLint a, GLint b)
{ GLint v[2] = { a, b }; if (!uc_same(l, v, sizeof v)) glUniform2i(l, a, b); }
static void cu1iv(GLint l, GLsizei n, const GLint *p)
{ if (!uc_same(l, p, (size_t)n * 4)) glUniform1iv(l, n, p); }
static void cu1uiv(GLint l, GLsizei n, const GLuint *p)
{ if (!uc_same(l, p, (size_t)n * 4)) glUniform1uiv(l, n, p); }
static void cu2fv(GLint l, GLsizei n, const GLfloat *p)
{ if (!uc_same(l, p, (size_t)n * 8)) glUniform2fv(l, n, p); }
static void cu4fv(GLint l, GLsizei n, const GLfloat *p)
{ if (!uc_same(l, p, (size_t)n * 16)) glUniform4fv(l, n, p); }
static void cu4uiv(GLint l, GLsizei n, const GLuint *p)
{ if (!uc_same(l, p, (size_t)n * 16)) glUniform4uiv(l, n, p); }
static void cuMatrix4fv(GLint l, GLsizei n, GLboolean t, const GLfloat *p)
{ if (!uc_same(l, p, (size_t)n * 64)) glUniformMatrix4fv(l, n, t, p); }
static uint32_t s_frame = 1;
/* What the draw path last bound; anything else that binds a program or a
 * vertex array (the frame interpolator) zeroes these. */
static GLuint s_cur_prog, s_cur_vao;

/* VC3_GL_ERRCHECK=<n>: glGetError at every checkpoint (GLCK) of the draw,
 * clear and flip paths; the first n errors of each site logged with the
 * batch (cpu / raw ff / raw vp), its surface, the bound VAO and program,
 * and once a second the count per site. Apple's GL 4.1 core has neither
 * KHR_debug nor ARB_debug_output, so this is the way to find a call GL
 * refused there; on GLES the KHR_debug callback is installed as well
 * (gl_ready). An error is charged to the first checkpoint after it. */
static int s_ek = -1;
#define EK_SITES 64
static struct { const char *site; uint32_t n, sec, logged; GLenum last; } s_eks[EK_SITES];
static int s_ekn;

static void glck(const char *site, const Nv2aBatch *b, uint32_t va)
{
    GLenum e;
    if (s_ek < 0) {
        s_ek = genv("GL_ERRCHECK") ? atoi(genv("GL_ERRCHECK")) : 0;
        if (s_ek < 0)
            s_ek = 0;
        if (s_ek)
            fprintf(stderr, "[GLERR] checking glGetError at every checkpoint (VC3_GL_ERRCHECK=%d)\n", s_ek);
    }
    if (!s_ek)
        return;
    while ((e = glGetError()) != GL_NO_ERROR) {
        int k;
        for (k = 0; k < s_ekn && s_eks[k].site != site; k++)
            ;
        if (k == s_ekn) {
            if (s_ekn == EK_SITES)
                k = EK_SITES - 1;
            else
                s_eks[s_ekn++].site = site;
        }
        s_eks[k].n++;
        s_eks[k].sec++;
        s_eks[k].last = e;
        if (s_eks[k].logged < (uint32_t)s_ek) {
            GLint vao = 0, prog = 0;
            s_eks[k].logged++;
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
            glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
            fprintf(stderr, "[GLERR] %s: 0x%04X | %s batch, surface %08X, vao %d, program %d,"
                    " frame %u\n", site, e,
                    !b ? "no" : !b->raw ? "cpu" : b->raw->kind == NV2A_RAW_PROGRAM ? "raw vp"
                    : "raw ff", va, vao, prog, s_frame);
        }
    }
}
#define GLCK(site, b, va) do { if (s_ek) glck((site), (b), (va)); } while (0)

static void glck_second(void)
{
    static uint32_t fr;
    int k, any = 0;
    if (s_ek <= 0 || ++fr % 60)
        return;
    for (k = 0; k < s_ekn; k++)
        if (s_eks[k].sec) {
            if (!any++)
                fprintf(stderr, "[GLERR] second %u:", fr / 60);
            fprintf(stderr, " %s %u (0x%04X);", s_eks[k].site, s_eks[k].sec, s_eks[k].last);
            s_eks[k].sec = 0;
        }
    if (any)
        fputc(10, stderr);
}

/* VC3_GL_FTRACE=<flag file>: once the file exists, log every clear and batch
 * of the next frame (0 idle, 1 tracing, 2 done). */
static int s_ftr;

/* VC3_GL_STATS=1: every 300 flips, what the batches asked for. */
static struct {
    int on;
    uint32_t draws, tris, textured, rt, uploads, mipped, qskip, untex, rt_logged, rc_logged;
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
    "uniform vec2 u_xs;\n"                    /* clip x scale, offset (16:9) */
    "uniform vec2 u_eye;\n"                   /* stereo: h, convergence D */
    "uniform mat4 u_immvp;\n"                 /* immersive: this eye's matrix */
    "uniform vec4 u_gp;\n"                    /* the game's projection kx ky cx cy */
    "uniform vec2 u_gw;\n"                    /* immersive on, 1 / pz */
    "uniform int u_st2;\n"                   /* both eyes in one draw: instance = eye */
    "uniform vec2 u_eye2[2];\n"
    "uniform mat4 u_immvp2[2];\n"
    "flat out int v_eye;\n"
    "uniform vec2 u_depth;\n"                 /* clip min, max */
    "uniform vec2 u_texsize;\n"
    "out vec4 v_col;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_spec;\n"
    "out float v_fog;\n"
    "out vec4 v_tc0, v_tc1, v_tc2, v_tc3;\n"
    "void main() {\n"
    "  float w = (a_pos.w > 0.0 && a_pos.w < 1e30) ? 1.0 / a_pos.w : 1.0;\n"
    "  int seye = u_st2 != 0 ? gl_InstanceID : 0;\n"
    "  vec2 ueye = u_st2 != 0 ? u_eye2[seye] : u_eye;\n"
    "  mat4 ivp = u_st2 != 0 ? u_immvp2[seye] : u_immvp;\n"
    "  vec2 ndc = a_pos.xy / u_surf * 2.0 - 1.0;\n"
    /* stereo, 3D only: rhw 1 is 2D, on the screen */
    "  if (ueye.x != 0.0 && a_pos.w != 1.0 && a_pos.w > 0.0)\n"
    "    ndc.x += ueye.x * (min(ueye.y * a_pos.w, 4.0) - 1.0);\n"
    "  ndc.x = ndc.x * u_xs.x + u_xs.y;\n"
    "  float z = clamp((a_pos.z - u_depth.x) / (u_depth.y - u_depth.x), 0.0, 1.0) * 2.0 - 1.0;\n"
    /* immersive, 3D: the game eye position back from the screen and w,
     * then through this eye (vr_math.h, vr_unproject_gl) */
    "  if (u_gw.x != 0.0 && a_pos.w != 1.0 && a_pos.w > 0.0) {\n"
    "    float ez = w * u_gw.y;\n"
    "    gl_Position = ivp * vec4((a_pos.x - u_gp.z) / u_gp.x * ez,\n"
    "                             (a_pos.y - u_gp.w) / u_gp.y * ez, -w * abs(u_gw.y), 1.0);\n"
    /* the surfaces hold the picture top row first (screen y down into GL's
     * y, flipped back when shown): the eye's clip y the same way round */
    "    gl_Position.y = -gl_Position.y;\n"
    "  } else\n"
    "    gl_Position = vec4(ndc * w, z * w, w);\n"
    "  v_eye = seye;\n"
    "  if (u_st2 != 0)\n"
    "    gl_Position.x = gl_Position.x * 0.5 + (seye == 0 ? -0.5 : 0.5) * gl_Position.w;\n"
    "#if !defined(GL_ES) || defined(GL_EXT_clip_cull_distance)\n"
    "  gl_ClipDistance[0] = u_st2 != 0 ? (seye == 0 ? -gl_Position.x : gl_Position.x) : 1.0;\n"
    "#endif\n"
    "  v_col = a_col.zyxw;\n"
    "  v_uv = a_uv / u_texsize;\n"
    "  v_spec = a_spec.zyxw;\n"
    "  v_fog = a_fog;\n"
    "  v_tc0 = a_tc0; v_tc1 = a_tc1; v_tc2 = a_tc2; v_tc3 = a_tc3;\n"
    "}\n";

/* Fixed function on the GPU (Nv2aRawGeometry, NV2A_RAW_FIXED): the
 * executor's ffp_vertex -- ffp_skinned_clip, lit_color, ffp_eye, ffp_texgen,
 * the texture matrices -- transcribed line for line, on the attribute
 * streams as the title laid them out. Every policy choice (which matrix,
 * which stream is the colour, the env switches) arrives decided in the
 * uniforms, so a difference from the CPU path is a bug in one of the two.
 * It feeds the fragment shader above through the same varyings.
 *
 * Inputs are by role, not NV2A slot: the colour and texcoord-0 slots are the
 * executor's picks. A packed 11:11:10 normal arrives as two unsigned shorts
 * (exact in a float) and is unpacked here; signed shorts arrive raw and are
 * scaled by u_ascale, since GL 4.1 normalises them as (2c+1)/65535. */
static const char *VS_FF =
    "#version 410\n"
    "layout(location=0) in vec4 a_pos;\n"
    "layout(location=1) in vec4 a_wt;\n"
    "layout(location=2) in vec4 a_nrm;\n"
    "layout(location=3) in vec4 a_col;\n"
    "layout(location=4) in vec4 a_t0;\n"
    "layout(location=5) in vec4 a_t1;\n"
    "layout(location=6) in vec4 a_t2;\n"
    "layout(location=7) in vec4 a_t3;\n"
    "uniform vec2 u_surf, u_depth, u_texsize;\n"
    "uniform vec2 u_xs;\n"
    "uniform vec2 u_eye;\n"
    "uniform mat4 u_comp, u_mv[4], u_proj, u_tm[4];\n"   /* uploaded transposed */
    "uniform mat4 u_immvp;\n"
    "uniform vec4 u_gp;\n"
    "uniform vec2 u_gw;\n"
    "uniform int u_st2;\n"                   /* both eyes in one draw: instance = eye */
    "uniform vec2 u_eye2[2];\n"
    "uniform mat4 u_immvp2[2];\n"
    "flat out int v_eye;\n"
    "uniform vec4 u_c59;\n"
    "uniform uint u_skin;\n"
    "uniform int u_usecomp, u_lighting, u_flipn, u_hasnrm, u_hascol;\n"
    "uniform ivec2 u_colmat;\n"                            /* ambient, diffuse */
    "uniform int u_lkind[8];\n"
    "uniform vec4 u_light[40];\n"
    "uniform vec4 u_samb;\n"
    "uniform float u_matalpha;\n"
    "uniform uvec4 u_texgen[4];\n"
    "uniform int u_tgon[4], u_tmon[4];\n"
    "uniform vec4 u_ascale[8];\n"
    "uniform int u_packed;\n"
    "uniform int u_bgra;\n"            /* attributes given as RGBA bytes: swap (ES) */
    "uniform vec2 u_aa;\n"
    "uniform float u_vpz;\n"
    "uniform int u_nearz;\n"
    "uniform int u_fogen;\n"
    "uniform uint u_fogmode;\n"
    "uniform vec2 u_fogp;\n"
    "uniform vec2 u_uvs0;\n"
    "out vec4 v_col;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_spec;\n"
    "out float v_fog;\n"
    "out vec4 v_tc0, v_tc1, v_tc2, v_tc3;\n"
    "out vec4 o_screen;\n"                    /* RECOMP_GPU_XF_CHECK: x y z w */
    "vec4 fetch(int r, vec4 a) {\n"
    "  if (((u_packed >> r) & 1) != 0) {\n"
    "    int v = int(uint(a.x) | (uint(a.y) << 16));\n"
    "    return vec4(float(bitfieldExtract(v, 0, 11)) / 1023.0,\n"
    "                float(bitfieldExtract(v, 11, 11)) / 1023.0,\n"
    "                float(bitfieldExtract(v, 22, 10)) / 511.0, 1.0);\n"
    "  }\n"
    "  if (((u_bgra >> r) & 1) != 0) a = a.zyxw;\n"
    "  return a * u_ascale[r];\n"
    "}\n"
    /* executor fog_factor() */
    "float fog_factor(float d) {\n"
    "  if (u_fogen == 0) return 1.0;\n"
    "  float f, px = u_fogp.x, py = u_fogp.y;\n"
    "  if (u_fogmode == 0x800u || u_fogmode == 0x802u) f = px + exp2(d * py * 16.0) - 1.5;\n"
    "  else if (u_fogmode == 0x801u || u_fogmode == 0x803u) f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n"
    "  else f = px + d * py - 1.0;\n"
    "  if (u_fogmode == 0x802u || u_fogmode == 0x803u || u_fogmode == 0x804u) f = abs(f);\n"
    "  if (isnan(f)) f = 1.0;\n"
    "  return clamp(f, 0.0, 1.0);\n"
    "}\n"
    /* lit_color: model-view 0 only, unskinned, position w taken as 1. */
    "vec4 lit_color(vec4 pos, vec4 nrm, vec4 vc) {\n"
    "  vec3 e = (u_mv[0] * vec4(pos.xyz, 1.0)).xyz;\n"
    "  vec3 n = mat3(u_mv[0]) * nrm.xyz;\n"
    "  float len = length(n);\n"
    "  if (len > 1e-8) n /= len;\n"
    "  if (u_flipn != 0 && dot(n, e) > 0.0) n = -n;\n"
    "  bool hv = u_hascol != 0;\n"
    "  vec3 o = u_samb.rgb;\n"
    "  for (int i = 0; i < 8; i++) {\n"
    "    int kind = u_lkind[i];\n"
    "    if (kind == 0) continue;\n"
    "    vec3 l; float att = 1.0;\n"
    "    if (kind == 1) l = u_light[i * 5 + 2].xyz;\n"
    "    else {\n"
    "      l = u_light[i * 5 + 3].xyz - e;\n"
    "      float d = length(l), range = u_light[i * 5].w;\n"
    "      if (d > range && range > 0.0) continue;\n"
    "      if (d > 1e-8) l /= d;\n"
    "      vec3 a = u_light[i * 5 + 4].xyz;\n"
    "      att = a.x + a.y * d + a.z * d * d;\n"
    "      att = att > 1e-8 ? 1.0 / att : 1.0;\n"
    "    }\n"
    "    float ndl = dot(n, l);\n"
    "    if (ndl < 0.0) ndl = 0.0;\n"
    "    vec3 amb = u_light[i * 5].rgb, dif = u_light[i * 5 + 1].rgb;\n"
    "    if (hv && u_colmat.x != 0) amb *= vc.rgb;\n"
    "    if (hv && u_colmat.y != 0) dif *= vc.rgb;\n"
    "    o += att * (amb + dif * ndl);\n"
    "  }\n"
    "  return clamp(vec4(o, (hv && u_colmat.y != 0) ? vc.a : u_matalpha), 0.0, 1.0);\n"
    "}\n"
    /* ffp_texgen, from the skinned eye position and normal (ffp_eye). */
    "vec4 texgen(uvec4 mode, vec3 e, vec3 n, vec4 tc) {\n"
    "  float len = length(e);\n"
    "  vec3 u = len > 1e-8 ? e / len : vec3(0.0);\n"
    "  float d = dot(n, u);\n"
    "  vec3 r = u - 2.0 * d * n;\n"
    "  float m = 2.0 * sqrt(r.x * r.x + r.y * r.y + (r.z + 1.0) * (r.z + 1.0));\n"
    "  for (int c = 0; c < 3; c++) {\n"
    "    if (mode[c] == 0x8511u) tc[c] = n[c];\n"
    "    else if (mode[c] == 0x8512u) tc[c] = r[c];\n"
    "    else if (mode[c] == 0x2402u && c < 2 && m > 1e-8) tc[c] = r[c] / m + 0.5;\n"
    "  }\n"
    "  if (mode.w == 0u) tc.w = 1.0;\n"
    "  return tc;\n"
    "}\n"
    "void main() {\n"
    "  vec4 pos = fetch(0, a_pos), wt = fetch(1, a_wt), nrm = fetch(2, a_nrm);\n"
    "  vec4 col = fetch(3, a_col);\n"
    "  vec4 t[4];\n"
    "  t[0] = fetch(4, a_t0); t[1] = fetch(5, a_t1); t[2] = fetch(6, a_t2); t[3] = fetch(7, a_t3);\n"
    /* Blend weights: ffp_skinned_clip's table, the last weight implied in
     * modes 1, 3 and 5. */
    "  vec4 ww = vec4(1.0, 0.0, 0.0, 0.0);\n"
    "  int nm = 1;\n"
    "  if (u_skin != 0u) {\n"
    "    nm = u_skin <= 2u ? 2 : u_skin <= 4u ? 3 : 4;\n"
    "    for (int k = 0; k < 4; k++) ww[k] = k < nm ? wt[k] : 0.0;\n"
    "    if ((u_skin & 1u) != 0u) {\n"
    "      float last = 1.0;\n"
    "      for (int k = 0; k < nm - 1; k++) last -= wt[k];\n"
    "      ww[nm - 1] = last;\n"
    "    }\n"
    "  }\n"
    "  vec4 eye = vec4(0.0);\n"
    "  vec3 en = vec3(0.0);\n"
    "  for (int k = 0; k < nm; k++) {\n"
    "    eye += ww[k] * (u_mv[k] * pos);\n"
    "    en += ww[k] * (mat3(u_mv[k]) * nrm.xyz);\n"
    "  }\n"
    "  vec4 clip = u_usecomp != 0 ? u_comp * pos : u_proj * eye;\n"
    "  float w = clip.w;\n"
    "  vec3 s = vec3((clip.xy / w + u_c59.xy) * u_aa, clip.z / w + u_c59.z);\n"
    "  vec4 d0 = u_lighting != 0 ? lit_color(pos, nrm, col)\n"
    "          : (u_hascol != 0 ? col : vec4(1.0));\n"
    "  d0 = clamp(d0, 0.0, 1.0);\n"

    "  float elen = length(en);\n"
    "  if (elen > 1e-8) en /= elen;\n"
    "  for (int st = 0; st < 4; st++) {\n"
    "    if (u_tgon[st] != 0) t[st] = texgen(u_texgen[st], eye.xyz, en, t[st]);\n"
    "    if (u_tmon[st] != 0) t[st] = u_tm[st] * t[st];\n"
    "  }\n"
    "  o_screen = vec4(s, w);\n"
    "  v_col = d0;\n"
    "  v_spec = vec4(0.0);\n"
    "  v_fog = fog_factor(0.0);\n"            /* no fog coordinate in fixed function */
    "  v_tc0 = t[0]; v_tc1 = t[1]; v_tc2 = t[2]; v_tc3 = t[3];\n"
    "  v_uv = t[0].xy * u_uvs0 / u_texsize;\n"
    /* Screen space back to GL clip space, as the CPU path's shader does but
     * with z unclamped (GL_DEPTH_CLAMP clamps per fragment). The CPU clips at
     * w = 1e-3 and at the near plane z = c59.z; so do the clip distances. A
     * vertex the CPU could not place (w = 0, or not finite) takes every
     * triangle touching it with it. */
    "  int seye = u_st2 != 0 ? gl_InstanceID : 0;\n"
    "  vec2 ueye = u_st2 != 0 ? u_eye2[seye] : u_eye;\n"
    "  mat4 ivp = u_st2 != 0 ? u_immvp2[seye] : u_immvp;\n"
    "  vec2 ndc = s.xy / u_surf * 2.0 - 1.0;\n"
    "  if (ueye.x != 0.0 && w > 0.0)\n"
    "    ndc.x += ueye.x * (min(ueye.y / w, 4.0) - 1.0);\n"
    "  ndc.x = ndc.x * u_xs.x + u_xs.y;\n"
    "  float zn = (s.z - u_depth.x) / (u_depth.y - u_depth.x) * 2.0 - 1.0;\n"
    "  bool bad = abs(w) <= 1e-6 || any(isnan(o_screen)) || any(isinf(o_screen));\n"
    /* immersive: the eye position back from s and w, through this eye; the
     * headset's frustum clips (the game's planes no longer apply) */
    "  bool imm = u_gw.x != 0.0;\n"
    "  float iez = w * u_gw.y;\n"
    "  vec4 ipos = ivp * vec4((s.x - u_gp.z) / u_gp.x * iez, (s.y - u_gp.w) / u_gp.y * iez,\n"
    "                             -w * abs(u_gw.y), 1.0);\n"
    "  ipos.y = -ipos.y;\n"                  /* top row first, as the surfaces */
    "  gl_Position = bad ? vec4(0.0, 0.0, 0.0, 1.0) : (imm ? ipos : vec4(ndc * w, zn * w, w));\n"
    "#if !defined(GL_ES) || defined(GL_EXT_clip_cull_distance)\n"
    "  gl_ClipDistance[0] = bad ? -1e30 : (imm ? 1.0 : w - 1e-3);\n"
    "  gl_ClipDistance[1] = bad ? -1e30 : (!imm && u_nearz != 0 ? (s.z - u_vpz) * w : 1.0);\n"
    "#endif\n"
    "  v_eye = seye;\n"
    "  if (u_st2 != 0)\n"
    "    gl_Position.x = gl_Position.x * 0.5 + (seye == 0 ? -0.5 : 0.5) * gl_Position.w;\n"
    "#if !defined(GL_ES) || defined(GL_EXT_clip_cull_distance)\n"
    "  gl_ClipDistance[2] = u_st2 != 0 ? (seye == 0 ? -gl_Position.x : gl_Position.x) : 1.0;\n"
    "#endif\n"
    "}\n";


/* Vertex programs on the GPU (NV2A_RAW_PROGRAM): the title's program,
 * translated by nv2a_vsh_glsl.c into nv2a_vsh(), between the inputs (v0..v15
 * as the executor's attributes, fetched as the fixed-function shader fetches
 * them) and the interpreter's epilogue (transform_vertex): the colours
 * clamped, w = +0 read as 1, x and y scaled for anti-aliasing. Then the
 * fixed-function shader's way back to GL clip space, with the CPU path's
 * rule for screen-space vertices (w = 1: no stereo shift, not re-projected
 * immersive). It feeds the same fragment shaders through the same varyings,
 * and the check (RECOMP_GPU_XF_CHECK) reads the same transform feedback. */
/* after "#version 410" and the inputs the program reads (vp_get) */
static const char *VS_VP_HEAD =
    "uniform vec2 u_surf, u_depth, u_texsize;\n"
    "uniform vec2 u_xs;\n"
    "uniform vec2 u_eye;\n"
    "uniform mat4 u_immvp;\n"
    "uniform vec4 u_gp;\n"
    "uniform vec2 u_gw;\n"
    "uniform int u_st2;\n"
    "uniform vec2 u_eye2[2];\n"
    "uniform mat4 u_immvp2[2];\n"
    "flat out int v_eye;\n"
    "uniform vec4 u_ascale[16];\n"
    "uniform int u_packed;\n"
    "uniform int u_bgra;\n"
    "uniform vec2 u_aa;\n"
    "uniform float u_vpz;\n"
    "uniform int u_nearz;\n"
    "uniform int u_fogen;\n"
    "uniform uint u_fogmode;\n"
    "uniform vec2 u_fogp;\n"
    "uniform vec2 u_uvs0;\n"
    "out vec4 v_col;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_spec;\n"
    "out float v_fog;\n"
    "out vec4 v_tc0, v_tc1, v_tc2, v_tc3;\n"
    "out vec4 o_screen;\n"
    "vec4 fetch(int r, vec4 a) {\n"
    "  if (((u_packed >> r) & 1) != 0) {\n"
    "    int v = int(uint(a.x) | (uint(a.y) << 16));\n"
    "    return vec4(float(bitfieldExtract(v, 0, 11)) / 1023.0,\n"
    "                float(bitfieldExtract(v, 11, 11)) / 1023.0,\n"
    "                float(bitfieldExtract(v, 22, 10)) / 511.0, 1.0);\n"
    "  }\n"
    "  if (((u_bgra >> r) & 1) != 0) a = a.zyxw;\n"
    "  return a * u_ascale[r];\n"
    "}\n"
    "float fog_factor(float d) {\n"
    "  if (u_fogen == 0) return 1.0;\n"
    "  float f, px = u_fogp.x, py = u_fogp.y;\n"
    "  if (u_fogmode == 0x800u || u_fogmode == 0x802u) f = px + exp2(d * py * 16.0) - 1.5;\n"
    "  else if (u_fogmode == 0x801u || u_fogmode == 0x803u) f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n"
    "  else f = px + d * py - 1.0;\n"
    "  if (u_fogmode == 0x802u || u_fogmode == 0x803u || u_fogmode == 0x804u) f = abs(f);\n"
    "  if (isnan(f)) f = 1.0;\n"
    "  return clamp(f, 0.0, 1.0);\n"
    "}\n"
    /* the interpreter's colour clamp: above 1 is 1, NaN and below 0 are 0 */
    "vec4 vsh_clamp(vec4 c) {\n"
    "  vec4 r = mix(vec4(0.0), c, greaterThanEqual(c, vec4(0.0)));\n"
    "  return mix(r, vec4(1.0), greaterThan(c, vec4(1.0)));\n"
    "}\n";

/* after `void main() {` and the inputs fetched (vp_get) */
static const char *VS_VP_MAIN =
    "  vec4 oPos, O[13];\n"
    "  nv2a_vsh(v, oPos, O);\n"
    "  if (floatBitsToUint(oPos.w) == 0u) oPos.w = 1.0;\n"
    "  float w = oPos.w;\n"
    "  vec3 s = vec3(oPos.xy * u_aa, oPos.z);\n"
    "  o_screen = vec4(s, w);\n"
    "  v_col = vsh_clamp(O[3]);\n"
    "  v_spec = vsh_clamp(O[4]);\n"
    "  v_fog = fog_factor(O[5].x);\n"
    "  v_tc0 = O[9]; v_tc1 = O[10]; v_tc2 = O[11]; v_tc3 = O[12];\n"
    "  v_uv = O[9].xy * u_uvs0 / u_texsize;\n"
    "  bool flat2d = w == 1.0;\n"             /* the CPU path's rhw 1: screen space */
    "  int seye = u_st2 != 0 ? gl_InstanceID : 0;\n"
    "  vec2 ueye = u_st2 != 0 ? u_eye2[seye] : u_eye;\n"
    "  mat4 ivp = u_st2 != 0 ? u_immvp2[seye] : u_immvp;\n"
    "  vec2 ndc = s.xy / u_surf * 2.0 - 1.0;\n"
    "  if (ueye.x != 0.0 && !flat2d && w > 0.0)\n"
    "    ndc.x += ueye.x * (min(ueye.y / w, 4.0) - 1.0);\n"
    "  ndc.x = ndc.x * u_xs.x + u_xs.y;\n"
    "  float zn = (s.z - u_depth.x) / (u_depth.y - u_depth.x) * 2.0 - 1.0;\n"
    "  bool bad = abs(w) <= 1e-6 || any(isnan(o_screen)) || any(isinf(o_screen));\n"
    "  bool imm = u_gw.x != 0.0 && !flat2d && w > 0.0;\n"
    "  float iez = w * u_gw.y;\n"
    "  vec4 ipos = ivp * vec4((s.x - u_gp.z) / u_gp.x * iez, (s.y - u_gp.w) / u_gp.y * iez,\n"
    "                             -w * abs(u_gw.y), 1.0);\n"
    "  ipos.y = -ipos.y;\n"
    "  gl_Position = bad ? vec4(0.0, 0.0, 0.0, 1.0) : (imm ? ipos : vec4(ndc * w, zn * w, w));\n"
    /* the CPU clips every program batch at w = 1e-3 and the near plane,
     * immersive or not */
    "#if !defined(GL_ES) || defined(GL_EXT_clip_cull_distance)\n"
    "  gl_ClipDistance[0] = bad ? -1e30 : w - 1e-3;\n"
    "  gl_ClipDistance[1] = bad ? -1e30 : (u_nearz != 0 ? (s.z - u_vpz) * w : 1.0);\n"
    "#endif\n"
    "  v_eye = seye;\n"
    "  if (u_st2 != 0)\n"
    "    gl_Position.x = gl_Position.x * 0.5 + (seye == 0 ? -0.5 : 0.5) * gl_Position.w;\n"
    "#if !defined(GL_ES) || defined(GL_EXT_clip_cull_distance)\n"
    "  gl_ClipDistance[2] = u_st2 != 0 ? (seye == 0 ? -gl_Position.x : gl_Position.x) : 1.0;\n"
    "#endif\n"
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
    "uniform vec4 u_sbs[2];\n"                /* per stage: stereo surface?, eye's half */
    "flat in int v_eye;\n"                    /* one-pass stereo: the eye */
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
    "vec2 sbs(int st, vec2 uv) {\n"
    "  vec2 e = st == 0 ? u_sbs[0].xy : st == 1 ? u_sbs[0].zw : st == 2 ? u_sbs[1].xy : u_sbs[1].zw;\n"
    "  return e.x != 0.0 ? vec2(clamp(uv.x, 0.0, 1.0) * 0.5 + e.y + 0.5 * float(v_eye), uv.y) : uv;\n"
    "}\n"
    "vec2 stage_uv(int st, vec4 tc) {\n"
    "  uint mode = (u_sprog >> uint(st * 5)) & 31u;\n"
    "  float q = tc.w != 0.0 ? tc.w : 1.0;\n"
    "  return sbs(st, (mode == 3u ? tc.xy : tc.xy / q) * u_ts[st]);\n"
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

    "#ifdef RC\n"
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
    "    c = v_col * (u_textured != 0 ? texture(u_tex, sbs(0, v_uv)) : vec4(1.0));\n"
    "  else\n"
    "#endif\n"
    "  if (u_textured != 0) {\n"
    "    c *= texture(u_tex, sbs(0, v_uv));\n"
    "    if (u_lmap != 0) c.rgb *= texture(u_t1, stage_uv(1, v_tc1)).rgb;\n"
    "  }\n"
    "#ifndef NOAT\n"
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
    "#endif\n"
    "  o_col = c;\n"
    "}\n";

#if defined(VC3_GLES)
/* The sources start "#version 410\n"; ES gets its own version in its
 * place -- 3.20 on the Quest; 3.10 or 3.00 on lesser contexts such as the
 * Android emulator's, where 3.00 lacks only bitfieldExtract, given here --
 * the clip distance extension where there is one, and default precisions. */
static const char *es_source(const char *src, char **owned)
{
    static const char tail[] =
        "#extension GL_EXT_clip_cull_distance : enable\n"
        "precision highp float;\n"
        "precision highp int;\n"
        "precision highp sampler2D;\n"
        "precision highp samplerCube;\n";
    static const char poly300[] =
        "int bitfieldExtract(int v, int o, int b) { return (v << (32 - o - b)) >> (32 - b); }\n";
    const char *v = (const char *)glGetString(GL_VERSION);
    const char *ver = "#version 320 es\n";
    size_t n, k;

    *owned = NULL;
    if (strncmp(src, "#version 410\n", 13))
        return src;
    if (v && strstr(v, "OpenGL ES 3.1"))
        ver = "#version 310 es\n";
    else if (v && strstr(v, "OpenGL ES 3.0"))
        ver = "#version 300 es\n";
    n = strlen(src + 13);
    *owned = (char *)malloc(16 + sizeof tail + sizeof poly300 + n);
    if (!*owned)
        return src;
    k = strlen(ver);
    memcpy(*owned, ver, k);
    memcpy(*owned + k, tail, sizeof tail - 1);
    k += sizeof tail - 1;
    if (ver[10] == '0') {
        memcpy(*owned + k, poly300, sizeof poly300 - 1);
        k += sizeof poly300 - 1;
    }
    memcpy(*owned + k, src + 13, n + 1);
    return *owned;
}
#endif

static const char *s_compile_defs = "";  /* lines after #version (compile) */

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    char log[1024];
#if defined(VC3_GLES)
    char *owned;
    src = es_source(src, &owned);
#endif

    {   /* s_compile_defs after the #version line (the RC variant) */
        const char *nl = strchr(src, '\n');
        if (*s_compile_defs && nl) {
            static char buf[200000];
            size_t k = (size_t)(nl - src) + 1;
            int n;
            memcpy(buf, src, k);
            n = snprintf(buf + k, sizeof buf - k, "%s", s_compile_defs);
            if (k + (size_t)n + strlen(nl + 1) < sizeof buf) {
                strcpy(buf + k + n, nl + 1);
                src = buf;
            }
        }
    }
    glShaderSource(s, 1, &src, NULL);
#if defined(VC3_GLES)
    free(owned);
#endif
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[GL] shader: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

/* What the fixed-function shader hands transform feedback for
 * RECOMP_GPU_XF_CHECK: NV2A_RAW_CHECK_FLOATS per vertex, in that order. */
static const char *const k_check_varyings[] = {
    "o_screen", "v_col", "v_spec", "v_fog", "v_tc0", "v_tc1", "v_tc2", "v_tc3"
};

/* Link vs with the shared fragment shader and look its uniforms up. */
static int prog_link(Prog *p, GLuint vs, GLuint fs, int feedback)
{
    GLint ok = 0;
    char log[1024];

    memset(p, 0, sizeof *p);
    if (!vs || !fs)
        return 0;
    p->prog = glCreateProgram();
    glAttachShader(p->prog, vs);
    glAttachShader(p->prog, fs);
    if (feedback)
        glTransformFeedbackVaryings(p->prog, 8, k_check_varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(p->prog);
    glGetProgramiv(p->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(p->prog, sizeof log, NULL, log);
        fprintf(stderr, "[GL] program link failed: %s\n", log);
        glDeleteProgram(p->prog);
        p->prog = 0;
        return 0;
    }
#define LOC(field, name) p->field = glGetUniformLocation(p->prog, name)
    LOC(surf, "u_surf"); LOC(depth, "u_depth"); LOC(texsize, "u_texsize");
    LOC(xs, "u_xs"); LOC(eye, "u_eye"); LOC(sbs, "u_sbs");
    LOC(immvp, "u_immvp"); LOC(gp, "u_gp"); LOC(gw, "u_gw");
    LOC(st2, "u_st2"); LOC(eye2, "u_eye2"); LOC(immvp2, "u_immvp2");
    if (genv("IMM_LOG"))
        fprintf(stderr, "[IMM] program %u: immvp %d gp %d gw %d\n", p->prog, p->immvp, p->gp, p->gw);
    LOC(textured, "u_textured"); LOC(atest, "u_atest"); LOC(afunc, "u_afunc");
    LOC(aref, "u_aref"); LOC(rc, "u_rc"); LOC(cicw, "u_cicw"); LOC(aicw, "u_aicw");
    LOC(cocw, "u_cocw"); LOC(aocw, "u_aocw"); LOC(f0, "u_f0"); LOC(f1, "u_f1");
    LOC(fin0, "u_fin0"); LOC(fin1, "u_fin1"); LOC(ctl, "u_ctl"); LOC(sprog, "u_sprog");
    LOC(fc0, "u_fc0"); LOC(fc1, "u_fc1"); LOC(fogc, "u_fogc"); LOC(on, "u_on");
    LOC(ts, "u_ts"); LOC(iscube, "u_iscube"); LOC(dbgon, "u_dbgon");
    LOC(dbgcol, "u_dbgcol"); LOC(lmap, "u_lmap"); LOC(rcdbg, "u_rcdbg");
    LOC(comp, "u_comp"); LOC(mv, "u_mv"); LOC(proj, "u_proj"); LOC(tm, "u_tm");
    LOC(c59, "u_c59"); LOC(skin, "u_skin"); LOC(usecomp, "u_usecomp");
    LOC(lighting, "u_lighting"); LOC(lkind, "u_lkind"); LOC(light, "u_light");
    LOC(samb, "u_samb"); LOC(matalpha, "u_matalpha"); LOC(colmat, "u_colmat");
    LOC(flipn, "u_flipn"); LOC(hasnrm, "u_hasnrm"); LOC(hascol, "u_hascol");
    LOC(texgen, "u_texgen"); LOC(tgon, "u_tgon"); LOC(tmon, "u_tmon");
    LOC(ascale, "u_ascale"); LOC(packed, "u_packed"); LOC(aa, "u_aa");
    LOC(vpz, "u_vpz"); LOC(nearz, "u_nearz"); LOC(fogen, "u_fogen");
    LOC(fogmode, "u_fogmode"); LOC(fogp, "u_fogp"); LOC(uvs0, "u_uvs0");
    LOC(bgra, "u_bgra");
#undef LOC
    glUseProgram(p->prog);
    glUniform1i(glGetUniformLocation(p->prog, "u_tex"), 0);
    glUniform1i(glGetUniformLocation(p->prog, "u_t1"), 1);
    glUniform1i(glGetUniformLocation(p->prog, "u_t2"), 2);
    glUniform1i(glGetUniformLocation(p->prog, "u_t3"), 3);
    glUniform1i(glGetUniformLocation(p->prog, "u_c0"), 4);
    glUniform1i(glGetUniformLocation(p->prog, "u_c1"), 5);
    glUniform1i(glGetUniformLocation(p->prog, "u_c2"), 6);
    glUniform1i(glGetUniformLocation(p->prog, "u_c3"), 7);
    glUniform1i(glGetUniformLocation(p->prog, "u_adbg"),
                genv("GL_ADBG") ? atoi(genv("GL_ADBG")) : 0);
    glUniform1i(glGetUniformLocation(p->prog, "u_nofog"),
                genv("GL_NOFOG") != NULL);
    glUniform1i(p->rcdbg, genv("GL_RCDBG") ? atoi(genv("GL_RCDBG")) : 0);
    return 1;
}


/* ── Vertex programs: translated, linked, cached by program ───────────────
 * One entry per program (its hash), with the plain and the combiner
 * fragment shader linked to it. Built at a program's first batch: VC3 has a
 * handful. A program that will not translate or link is remembered as
 * failed, and its batches stay on the CPU. */
#define VP_SLOTS 64
typedef struct {
    uint64_t hash;
    int state;                            /* 0 empty, 1 ready, -1 failed */
    uint32_t used;
    uint32_t inputs;                      /* the v[] it reads, a bit each */
    Prog p, p_rc;
    Prog p_na, p_rc_na;                   /* without the alpha test (prog 0: none) */
} VpProg;
static VpProg s_vp[VP_SLOTS];
static VpProg *s_vp_cur;                  /* the program gl_draw draws with */
static uint32_t s_vp_built, s_vp_failed;

#define VC_BINDING 1                       /* the VshConst block's binding */

/* A vertex program's constants block at VC_BINDING. */
static void vc_block(const Prog *p)
{
    GLuint i = glGetUniformBlockIndex(p->prog, "VshConst");
    if (i != GL_INVALID_INDEX)
        glUniformBlockBinding(p->prog, i, VC_BINDING);
}

static VpProg *vp_get(const Nv2aRawGeometry *g)
{
    static char body[65536], src[98304], decl[2048], fetches[2048];
    VpProg *e = NULL;
    uint32_t ninsn = 0, wc = 0, inputs = 0, dl = 0, fl = 0, r;
    GLuint vs;
    int i, n;

    for (i = 0; i < VP_SLOTS; i++)
        if (s_vp[i].state && s_vp[i].hash == g->program_hash) {
            s_vp[i].used = s_frame;
            return s_vp[i].state > 0 ? &s_vp[i] : NULL;
        }
    for (i = 0; i < VP_SLOTS; i++) {           /* a free slot, or the oldest */
        if (!s_vp[i].state) { e = &s_vp[i]; break; }
        if (!e || s_vp[i].used < e->used) e = &s_vp[i];
    }
    if (e->state > 0) {
        glDeleteProgram(e->p.prog);
        if (e->p_rc.prog != e->p.prog)
            glDeleteProgram(e->p_rc.prog);
        free(e->p.uc);
        if (e->p_rc.uc != e->p.uc)
            free(e->p_rc.uc);
        if (e->p_na.prog) {
            glDeleteProgram(e->p_na.prog);
            free(e->p_na.uc);
        }
        if (e->p_rc_na.prog) {
            glDeleteProgram(e->p_rc_na.prog);
            free(e->p_rc_na.uc);
        }
    }
    memset(e, 0, sizeof *e);
    e->hash = g->program_hash;
    e->used = s_frame;
    e->state = -1;
    if (!s_fs_obj
        || !nv2a_vsh_to_glsl(g->program, g->program_start, NV2A_VSH_SLOTS, body, sizeof body,
                             &ninsn, &wc, &inputs)) {
        s_vp_failed++;
        fprintf(stderr, "[GL] vertex program %016llx: no translation\n",
                (unsigned long long)g->program_hash);
        return NULL;
    }
    /* only the inputs it reads: Apple's GL has 16 attributes in all, and
     * gl_InstanceID (one-pass stereo) takes one */
    fl += (uint32_t)snprintf(fetches + fl, sizeof fetches - fl,
                             "void main() {\n  vec4 v[16];\n"
                             "  for (int k = 0; k < 16; k++) v[k] = vec4(0.0, 0.0, 0.0, 1.0);\n");
    for (r = 0; r < 16; r++)
        if (inputs & (1u << r)) {
            dl += (uint32_t)snprintf(decl + dl, sizeof decl - dl,
                                     "layout(location=%u) in vec4 a_v%u;\n", r, r);
            fl += (uint32_t)snprintf(fetches + fl, sizeof fetches - fl,
                                     "  v[%u] = fetch(%u, a_v%u);\n", r, r, r);
        }
    e->inputs = inputs;
    n = snprintf(src, sizeof src, "#version 410\n%s%s%s%s%s%s", decl, VS_VP_HEAD,
                 nv2a_vsh_glsl_helpers(), body, fetches, VS_VP_MAIN);
    if (n < 0 || (size_t)n >= sizeof src) {
        s_vp_failed++;
        return NULL;
    }
    if (genv("GL_VP_DUMP"))                /* the GLSL, to compare with the microcode */
        fprintf(stderr, "[GL] vertex program %016llx GLSL:\n%s\n",
                (unsigned long long)g->program_hash, body);
    vs = compile(GL_VERTEX_SHADER, src);
    if (!vs || !prog_link(&e->p, vs, s_fs_obj, 1)) {
        s_vp_failed++;
        fprintf(stderr, "[GL] vertex program %016llx: shader failed; its batches stay on"
                " the CPU\n", (unsigned long long)g->program_hash);
        if (vs)
            glDeleteShader(vs);
        return NULL;
    }
    vc_block(&e->p);
    if (!s_fs_rc_obj || !prog_link(&e->p_rc, vs, s_fs_rc_obj, 1))
        e->p_rc = e->p;
    else
        vc_block(&e->p_rc);
    if (!s_fs_na_obj || !prog_link(&e->p_na, vs, s_fs_na_obj, 1))
        memset(&e->p_na, 0, sizeof e->p_na);
    else
        vc_block(&e->p_na);
    if (!s_fs_rc_na_obj || e->p_rc.prog == e->p.prog || !prog_link(&e->p_rc_na, vs, s_fs_rc_na_obj, 1))
        memset(&e->p_rc_na, 0, sizeof e->p_rc_na);
    else
        vc_block(&e->p_rc_na);
    glDeleteShader(vs);                        /* the programs keep it */
    e->state = 1;
    s_vp_built++;
    fprintf(stderr, "[GL] vertex program %016llx: %u instructions on the GPU%s\n",
            (unsigned long long)g->program_hash, ninsn,
            wc ? " (its writes to c[] ignored: CXT_WRITE off)" : "");
    return e;
}

/* ── Streaming buffers ─────────────────────────────────────── */

/* Vertex and index data stream through two large buffers, appended to and
 * orphaned when full, instead of a glBufferData per draw: a fresh allocation
 * for every one of ~30000 draws a second. Writes are unsynchronised -- a
 * range is never written twice before the orphan that ends its use. GL 4.1
 * on macOS has no persistent mapping (ARB_buffer_storage). */
/*
 * On OpenGL ES with EXT_buffer_storage (the Quest) the ring is instead
 * mapped once, persistent and coherent, and written with memcpy: Adreno's
 * glMapBufferRange blocked on every call, unsynchronised or not -- a
 * second a second in the upload, the game at 34 fps where it could hold 60.
 * The ring is cut into RING_SEGS segments; leaving one puts a fence after
 * its last draw, and coming round to it again waits for that fence (long
 * signalled at 16 MB a lap), so the GPU never reads data being replaced. */
#define RING_SEGS 4
typedef struct {
    GLuint buf;
    GLenum target;
    GLsizeiptr size, at;
    uint8_t *mem;                /* persistent mapping, or NULL */
    int seg;                     /* the segment being written */
    GLsync fence[RING_SEGS];
} Ring;
static Ring s_vring = { 0, GL_ARRAY_BUFFER, 16 << 20, 0, NULL, 0, { 0 } };
static Ring s_iring = { 0, GL_ELEMENT_ARRAY_BUFFER, 4 << 20, 0, NULL, 0, { 0 } };
/* The vertex programs' constants (the VshConst block, 192 vec4): a copy
 * streamed in whenever they change and bound as a range at VC_BINDING for
 * every program at once -- where each program had its 192 uniforms sent per
 * draw that changed them, and again on every switch of program. Ranges
 * start at the driver's uniform-buffer alignment. */
#define VC_BYTES (192 * 16)
static Ring s_cring = { 0, GL_UNIFORM_BUFFER, 16 << 20, 0, NULL, 0, { 0 } };
static GLint s_ubo_align = 256;
static uint32_t s_vc_gen_bound;         /* the constants' change count bound */

#if defined(VC3_GLES)
typedef void (*BufferStorageFn)(GLenum, GLsizeiptr, const void *, GLbitfield);
static BufferStorageFn s_buffer_storage;
static int s_buffer_storage_tried;

/* Persistent storage for r, on first use; 0 if the driver has none. */
static int ring_persistent(Ring *r)
{
    if (!s_buffer_storage_tried) {
        GLint i, n = 0;
        s_buffer_storage_tried = 1;
        glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        for (i = 0; i < n; i++) {
            const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
            if (e && !strcmp(e, "GL_EXT_buffer_storage"))
                s_buffer_storage = (BufferStorageFn)eglGetProcAddress("glBufferStorageEXT");
        }
        fprintf(stderr, "[GL] streaming: %s\n", s_buffer_storage
                ? "persistent mapped ring (EXT_buffer_storage)" : "map per draw");
    }
    if (!s_buffer_storage)
        return 0;
    if (!r->mem) {
        const GLbitfield fl = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT
                            | GL_MAP_COHERENT_BIT_EXT;
        glBindBuffer(r->target, r->buf);
        s_buffer_storage(r->target, r->size, NULL, fl);
        r->mem = (uint8_t *)glMapBufferRange(r->target, 0, r->size, fl);
        if (!r->mem)
            return 0;
        r->seg = 0;
    }
    return 1;
}

/* About to write [from, to): fence the segments left, wait for those
 * entered to be free. */
static void ring_enter(Ring *r, GLsizeiptr from, GLsizeiptr to)
{
    GLsizeiptr seg = r->size / RING_SEGS;
    int sg, s0 = (int)(from / seg), s1 = (int)((to - 1) / seg);

    for (sg = s0; sg <= s1; sg++) {
        if (sg == r->seg)
            continue;
        if (r->fence[r->seg])
            glDeleteSync(r->fence[r->seg]);
        r->fence[r->seg] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        r->seg = sg;
        if (r->fence[sg]) {
            GLenum result;
            do {
                result = glClientWaitSync(r->fence[sg], GL_SYNC_FLUSH_COMMANDS_BIT,
                                          1000000000ull);
            } while (result == GL_TIMEOUT_EXPIRED);
            if (result == GL_WAIT_FAILED) {
                fprintf(stderr, "[GL] ring wait failed (target 0x%X segment %d); finishing\n",
                        (unsigned)r->target, sg);
                glFinish();
            }
            glDeleteSync(r->fence[sg]);
            r->fence[sg] = 0;
        }
    }
}
#endif

/* Map `bytes` at a 16-byte boundary; the buffer stays bound to its target.
 * NULL if bytes cannot fit even an empty buffer. */
static void *ring_map(Ring *r, GLsizeiptr bytes, GLsizeiptr *offset)
{
    GLsizeiptr al = r->target == GL_UNIFORM_BUFFER && s_ubo_align > 16 ? s_ubo_align : 16;
    GLsizeiptr at = (r->at + al - 1) / al * al;

    if (bytes > r->size)
        return NULL;
    if (!r->buf)
        glGenBuffers(1, &r->buf);
    glBindBuffer(r->target, r->buf);
#if defined(VC3_GLES)
    if (r->mem || ring_persistent(r)) {
        if (at + bytes > r->size)
            at = 0;                    /* round again */
        ring_enter(r, at, at + bytes);
        *offset = at;
        r->at = at + bytes;
        return r->mem + at;
    }
#endif
    if (!r->at || at + bytes > r->size) {
        glBufferData(r->target, r->size, NULL, GL_STREAM_DRAW);
        at = 0;
    }
    *offset = at;
    r->at = at + bytes;
    return glMapBufferRange(r->target, at, bytes, GL_MAP_WRITE_BIT
                            | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
}

/* After writing what ring_map handed out (a persistent ring stays mapped). */
static void ring_unmap(Ring *r)
{
    if (!r->mem)
        glUnmapBuffer(r->target);
}

static GLsizeiptr ring_put(Ring *r, const void *data, GLsizeiptr bytes)
{
    GLsizeiptr off = 0;
    void *p = ring_map(r, bytes, &off);

    if (!p)
        return -1;
    memcpy(p, data, (size_t)bytes);
    ring_unmap(r);
    return off;
}

/* Lazily, on the thread the executor calls from; every entry point makes the
 * context current in case that is ever a different thread. */
#if defined(VC3_GLES)
/* KHR_debug (VC3_GL_ERRCHECK): errors and the driver's warnings, the first
 * 200 of them. */
static void gl_debug_cb(GLenum source, GLenum type, GLuint id, GLenum severity,
                        GLsizei len, const char *msg, const void *user)
{
    static int n;
    (void)source; (void)len; (void)user;
    if (type == 0x8251 /* OTHER */ && severity == 0x826B /* NOTIFICATION */)
        return;
    if (n++ < 200)
        fprintf(stderr, "[GLERR] KHR_debug type 0x%04X id %u severity 0x%04X frame %u: %s\n",
                type, id, severity, s_frame, msg);
}
#endif

static int gl_ready(void)
{
    if (s_failed)
        return 0;
    if (s_ctx) {
#if !defined(VC3_GLES)
        if (CGLGetCurrentContext() != s_ctx)
            CGLSetCurrentContext(s_ctx);
#endif
        return 1;
    }
    {
#if defined(VC3_GLES)
        GLuint fs;
        uint32_t white = 0xFFFFFFFFu;

        if (!vc3_quest_gl_context()) {
            fprintf(stderr, "[GL] no OpenGL ES 3.2 context; back end off\n");
            s_failed = 1;
            return 0;
        }
        s_ctx = 1;
        sh_reset();
        fprintf(stderr, "[GL] %s, %s\n", glGetString(GL_RENDERER),
                glGetString(GL_VERSION));
        {
            GLint i, n = 0;
            s_has_clip = s_has_dclamp = 0;
            glGetIntegerv(GL_NUM_EXTENSIONS, &n);
            for (i = 0; i < n; i++) {
                const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
                if (!e)
                    continue;
                if (!strcmp(e, "GL_EXT_clip_cull_distance"))
                    s_has_clip = 1;
                if (!strcmp(e, "GL_EXT_depth_clamp"))
                    s_has_dclamp = 1;
            }
            fprintf(stderr, "[GL] clip distances %s, depth clamp %s\n",
                    s_has_clip ? "yes" : "no (GL's near clip instead)",
                    s_has_dclamp ? "yes" : "no");
        }
        /* VC3_GL_ERRCHECK: the driver's own account of what it refused,
         * besides the checkpoints (GLCK) */
        if (genv("GL_ERRCHECK") && atoi(genv("GL_ERRCHECK")) > 0) {
            typedef void (*DebugCb)(GLenum, GLenum, GLuint, GLenum, GLsizei, const char *,
                                    const void *);
            typedef void (*DebugFn)(DebugCb, const void *);
            DebugFn fn = (DebugFn)eglGetProcAddress("glDebugMessageCallbackKHR");
            if (fn) {
                glEnable(0x92E0);              /* GL_DEBUG_OUTPUT_KHR */
                fn(gl_debug_cb, NULL);
                fprintf(stderr, "[GLERR] KHR_debug callback installed\n");
            } else {
                fprintf(stderr, "[GLERR] no KHR_debug; checkpoints only\n");
            }
        }
#else
        CGLPixelFormatAttribute attrs[] = {
            kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
            kCGLPFAAccelerated, kCGLPFAAllowOfflineRenderers,
            (CGLPixelFormatAttribute)0
        };
        CGLPixelFormatObj pf = NULL;
        GLint npf = 0;
        GLuint fs;
        uint32_t white = 0xFFFFFFFFu;

        if (CGLChoosePixelFormat(attrs, &pf, &npf) != kCGLNoError || !pf
            || CGLCreateContext(pf, NULL, &s_ctx) != kCGLNoError) {
            fprintf(stderr, "[GL] no OpenGL 4.1 context; back end off\n");
            s_failed = 1;
            return 0;
        }
        CGLDestroyPixelFormat(pf);
        CGLSetCurrentContext(s_ctx);
        sh_reset();
        fprintf(stderr, "[GL] %s, %s\n", glGetString(GL_RENDERER),
                glGetString(GL_VERSION));
#endif

        fs = compile(GL_FRAGMENT_SHADER, FS);
        s_fs_obj = fs;
        if (!prog_link(&s_pcpu, compile(GL_VERTEX_SHADER, VS), fs, 0)) {
            fprintf(stderr, "[GL] program link failed; back end off\n");
            s_failed = 1;
            return 0;
        }
        /* Without it every batch is transformed on the CPU, as before. */
        if (!prog_link(&s_praw, compile(GL_VERTEX_SHADER, VS_FF), fs, 1))
            fprintf(stderr, "[GL] fixed-function shader failed; raw draws declined\n");
        /* the same with the register combiners */
        s_compile_defs = "#define RC\n";
        {
            GLuint fs_rc = compile(GL_FRAGMENT_SHADER, FS);
            s_fs_rc_obj = fs_rc;
            if (!prog_link(&s_pcpu_rc, compile(GL_VERTEX_SHADER, VS), fs_rc, 0))
                s_pcpu_rc = s_pcpu;
            if (s_praw.prog && !prog_link(&s_praw_rc, compile(GL_VERTEX_SHADER, VS_FF), fs_rc, 1))
                s_praw_rc = s_praw;
        }
        /* and both without the alpha test */
        s_compile_defs = "#define NOAT\n";
        if ((s_fs_na_obj = compile(GL_FRAGMENT_SHADER, FS))) {
            if (!prog_link(&s_pcpu_na, compile(GL_VERTEX_SHADER, VS), s_fs_na_obj, 0))
                memset(&s_pcpu_na, 0, sizeof s_pcpu_na);
            if (!s_praw.prog || !prog_link(&s_praw_na, compile(GL_VERTEX_SHADER, VS_FF), s_fs_na_obj, 1))
                memset(&s_praw_na, 0, sizeof s_praw_na);
        }
        s_compile_defs = "#define RC\n#define NOAT\n";
        if ((s_fs_rc_na_obj = compile(GL_FRAGMENT_SHADER, FS))) {
            if (s_pcpu_rc.prog == s_pcpu.prog
                || !prog_link(&s_pcpu_rc_na, compile(GL_VERTEX_SHADER, VS), s_fs_rc_na_obj, 0))
                memset(&s_pcpu_rc_na, 0, sizeof s_pcpu_rc_na);
            if (!s_praw.prog || s_praw_rc.prog == s_praw.prog
                || !prog_link(&s_praw_rc_na, compile(GL_VERTEX_SHADER, VS_FF), s_fs_rc_na_obj, 1))
                memset(&s_praw_rc_na, 0, sizeof s_praw_rc_na);
        }
        s_compile_defs = "";

        /* The CPU path's layout; pointers are set per draw, as each batch
         * lands at a different place in the stream. */
        glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &s_ubo_align);
        glGenVertexArrays(1, &s_vao);
        glBindVertexArray(s_vao);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glGenVertexArrays(1, &s_vao_raw);
        glGenVertexArrays(1, &s_vao_vp);

        glGenTextures(1, &s_white);
        glBindTexture(GL_TEXTURE_2D, s_white);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA,
                     GL_UNSIGNED_INT_8_8_8_8_REV, &white);
        es_bgra(GL_TEXTURE_2D);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
    }
    return 1;
}

/* ES: a texture uploaded from BGRA bytes as RGBA reads back R and B
 * swapped; a swizzle on the texture puts them right when sampled. */
static void es_bgra(GLenum target)
{
#if defined(VC3_GLES)
    glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
    glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, GL_RED);
#else
    (void)target;
#endif
}

/* Guest memory from RGBA8 rows (ES reads): BGRA8, or R5G6B5 for 2-byte
 * surfaces, at the guest pitch. */
static void es_to_guest(uint8_t *dst, uint32_t pitch, uint32_t bpp,
                        const uint8_t *src, GLsizei w, GLsizei h)
{
    GLsizei x, y;

    for (y = 0; y < h; y++) {
        const uint8_t *s = src + (size_t)y * w * 4;
        uint8_t *d = dst + (size_t)y * pitch;
        if (bpp == 4) {
            for (x = 0; x < w; x++, s += 4, d += 4) {
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
            }
        } else {
            for (x = 0; x < w; x++, s += 4, d += 2) {
                uint16_t v = (uint16_t)(((s[0] >> 3) << 11) | ((s[1] >> 2) << 5) | (s[2] >> 3));
                d[0] = (uint8_t)v;
                d[1] = (uint8_t)(v >> 8);
            }
        }
    }
}

/* ── Surfaces ──────────────────────────────────────────────── */

/* Internal resolution and aspect (display_settings.c), as this frame draws
 * them: taken at each flip, so a frame never mixes two. A surface drawn at
 * scale sx, sy is that many times its guest size across and down -- whole
 * numbers from the Resolution setting, any from a headset's native size
 * (scale_pick) --; a widened one (16:9: a 4:3 surface, 640x480 in VC3) is
 * also a third wider, with the guest's picture in the middle -- see
 * surf_xform. Guest memory always gets the guest size and the middle part
 * (surf_resolve). */
static float s_sx = 1.0f, s_sy = 1.0f;
static int s_wide;
static float s_hud_sc = 1.0f;           /* the HUD picture's scale (immersive) */

#define MAX_SURFS 32
typedef struct {
    uint32_t va, pitch, bpp;
    GLsizei w, h;              /* guest size: what coordinates are in */
    GLsizei pw, ph;            /* drawn size: w, h times sc, widened */
    float sx, sy;
    int wide;
    GLsizei cw;                /* the guest picture's drawn width (pw unwidened) */
    /* 16:9: the frame a widened view was composited onto this surface, and
     * its 2D overlays (surf_xform). */
    uint32_t hor_frame;
    struct HudLayout *hud;
    /* Stereo: both eyes side by side in one framebuffer twice pw wide, the
     * left eye's picture at x 0 (what guest memory gets), the right's at pw. */
    int st;
    int fov;                   /* foveated (fov_params) */
    GLuint fbo, color, depth;
    int dirty;                 /* drawn since the last readback */
    /* Guest memory as the GPU last left it, to spot the CPU writing the
     * surface: VC3's movie decoder writes frames straight into a surface the
     * menu once cleared, so the GPU copy stays black. */
    int memq_valid, cpu_owned;
    uint64_t memq;
    /* Immersive: the game's projection into this surface (its last
     * perspective fixed-function batch), and 3D batches this frame. */
    VrGameProj gp;
    int gp_ok;
    uint32_t n3d;
    /* Lazy copies (lazy_skip): the frame it was last sampled; anything but a
     * full-screen 2D copy of a render target ever drawn into it; skipping;
     * the render target the skipped copies read. */
    uint32_t last_sampled, first_drawn;
    int not_copy, skipping;
    uint32_t copy_src;
    /* Depth discard (z_*): this frame's first depth/stencil access (0 none,
     * 1 a clear, 2 a draw), left for another surface this frame, came back
     * and drew with depth before clearing it, frames in a row that began
     * with a clear and never came back for the depth, discarded since. */
    int z_state, z_left, z_ret, z_good, z_inv, z_sten, z_logged;   /* z_sten: stencil ever tested */
    /* A clear not done yet (clear_defer): its buffers, colour mask and values. */
    GLbitfield pc_bits;
    GLboolean pc_mask[4];
    GLfloat pc_col[4], pc_depth;
    GLint pc_sten;
} Surf;
static void lazy_catch_up(Surf *rt);
/* The effect target's run (see "The effect target" below): drawing into L
 * for S; this draw goes there (surf_bind swaps S's framebuffer and size for
 * L's while it lasts). */
static struct {
    int on, draw, swapped;
    Surf *s;
    GLuint fbo, color, depth;
    GLsizei w, h;                    /* L, an eye */
    GLuint save_fbo;
    GLsizei save_pw, save_ph, save_cw;
    uint32_t n_this, n_prev, runs;   /* qualifying batches this frame, the last */
    /* the render target the run samples, reduced as L is, once a run */
    GLuint small_fbo, small_tex;
    GLsizei small_w, small_h;
    uint32_t small_va, small_run;
} s_fx;
static GLuint fx_small(const Surf *rt);
static void fx_pre(const Nv2aSurface *s, const Nv2aBatch *b);
static void fx_post(void);
static void fx_resolve(void);
static int fx_scale(void);
static Surf s_surfs[MAX_SURFS];
static int s_nsurfs;
static int lazy_skip(Surf *f, const Nv2aBatch *b);
static void clear_flush(Surf *f, GLbitfield want);
static void clear_leave(Surf *p);

/* ── Depth discard ──────────────────────────────────────────
 *
 * A tiler stores a pass's depth/stencil to memory when the pass ends --
 * here, when the back end binds another surface -- unless told the contents
 * are no longer needed (glInvalidateFramebuffer). Each surface has its own
 * depth (never read back), so it is dead once nothing will test against it
 * before the next clear. That is learned: a surface whose frames, Z_LEARN in
 * a row, began with a depth clear and never came back to draw with its depth
 * after binding another surface has its depth and stencil discarded when the
 * back end leaves it (and at the flip). Coming back for the depth after a
 * discard -- a pattern the learning had not seen -- is logged, and that
 * surface is never discarded again.
 *
 * VC3_GL_ZDISCARD: 1 discard (GLES only), 0 off (the default), "dry" only
 * count what would be discarded and the violations (any build; on the Mac,
 * whose GL 4.1 has no glInvalidateFramebuffer, this is how the learning is
 * checked).
 *
 * Off by default: in a Mac dry run (immersive 2x, attract to combat) VC3's
 * 3D surface 808D0000 showed the pattern for 50 s and then, at the mission
 * start, came back for its depth after another surface -- a frame with that
 * depth discarded would have drawn wrong once. Only the HUD surface kept
 * the pattern throughout. */
#define Z_LEARN 120               /* frames: two seconds of the same pattern */
static uint64_t tm_now(void);
static int s_zd = -1;                  /* 0 off, 1 on, 2 dry */
static uint32_t s_zd_n, s_zd_bad;      /* discards, violations (this second) */
static Surf *s_z_prev;                 /* the surface surf_bind bound last */

static int zd_mode(void)
{
    if (s_zd < 0) {
        const char *e = ch_env("GL_ZDISCARD");
#if defined(VC3_GLES)
        s_zd = !e ? 0 : !strcmp(e, "dry") ? 2 : *e != '0';
#else
        s_zd = e && !strcmp(e, "dry") ? 2 : 0;
#endif
        if (s_zd)
            fprintf(stderr, "[GL] depth discard %s\n", s_zd == 2 ? "dry run" : "on");
    }
    return s_zd;
}

/* f's depth and stencil are not needed again: tell the driver (f bound). */
static void z_discard(Surf *f)
{
    if (f->z_good < Z_LEARN || f->z_state != 1 || f->z_inv)
        return;
    s_zd_n++;
    f->z_inv = 1;
    if (!f->z_logged) {
        f->z_logged = 1;
        fprintf(stderr, "[GL] depth discard: surface %08X from now on\n", f->va);
    }
#if defined(VC3_GLES)
    if (s_zd == 1) {
        static const GLenum att[1] = { GL_DEPTH_STENCIL_ATTACHMENT };
        glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, att);
    }
#endif
}

/* surf_bind is binding f: the surface it leaves ends its pass. */
static void z_switch(Surf *f)
{
    Surf *p = s_z_prev;
    s_z_prev = f;
    if (p && p != f)
        clear_leave(p);
    if (!s_zd || p == f || !p)
        return;
    if (p->z_state)
        p->z_left = 1;
    z_discard(p);
}

/* a draw into f that tests or writes depth or stencil */
static void z_use(Surf *f, const Nv2aRenderState *rs)
{
    if (!s_zd || !rs->zeta_va
        || !(rs->depth_test_enable || rs->depth_write || rs->stencil_enable))
        return;
    if (rs->stencil_enable)
        f->z_sten = 1;
    if (!f->z_state)
        f->z_state = 2;
    if (f->z_left)
        f->z_ret = 1;
    if (f->z_inv) {                    /* the depth it needed was discarded */
        s_zd_bad++;
        if (f->z_good >= 0)
            fprintf(stderr, "[GL] depth discard: surface %08X drew with the depth it had "
                    "discarded; not discarding it again\n", f->va);
        f->z_good = -1000000;
        f->z_inv = 0;
    }
}

/* a clear of f: depth and stencil (or depth, stencil never tested) */
static void z_clear(Surf *f, GLbitfield bits)
{
    if (!s_zd || !(bits & GL_DEPTH_BUFFER_BIT))
        return;
    if (f->z_sten && !(bits & GL_STENCIL_BUFFER_BIT)) {  /* the stencil lives on */
        if (!f->z_state)
            f->z_state = 2;
        return;
    }
    if (!f->z_state)
        f->z_state = 1;
    f->z_left = 0;
    f->z_inv = 0;
}

/* the flip: the bound surface's pass ends too; the frames' verdicts */
static void z_flip(void)
{
    static uint64_t t0;
    int i;
    uint64_t now;
    if (!zd_mode())
        return;
    if (s_z_prev)
        z_discard(s_z_prev);
    for (i = 0; i < s_nsurfs; i++) {
        Surf *f = &s_surfs[i];
        if (f->z_state == 1 && !f->z_ret)
            f->z_good++;
        else if (f->z_state && f->z_good >= 0)
            f->z_good = 0;
        f->z_state = f->z_left = f->z_ret = 0;
    }
    s_z_prev = NULL;
    now = tm_now();
    if (!t0)
        t0 = now;
    if (now - t0 >= 1000000000ull) {
        if (s_zd_n || s_zd_bad)
            fprintf(stderr, "[GL] depth discard: %u a second%s, %u violations\n",
                    s_zd_n, s_zd == 2 ? " (dry)" : "", s_zd_bad);
        s_zd_n = s_zd_bad = 0;
        t0 = now;
    }
}
static int batch_fullscreen(const Nv2aBatch *b, const Surf *f);
static int batch_extent(const Nv2aBatch *b, Nv2aRawExtent *e);
static int batch_d0(const Nv2aBatch *b, uint32_t *d0);
static int raw_2d(const Nv2aRawGeometry *g);
static int raw_vp_3d(const Nv2aRawGeometry *g);

/* Only the screen-shaped surfaces widen: render targets of other shapes
 * (VC3's 128x128 and 256x256 effect maps) are sampled whole. */
static int surf_widens(GLsizei w, GLsizei h)
{
    return s_wide && w * 3 == h * 4;
}

/* ── Stereo (VC3_STEREO, display_settings.c) ──────────────────

   The game's view in 3D on a screen. A screen-shaped surface holds both eyes
   side by side, and each batch drawn into it is drawn twice -- the same
   state, vertices and textures, only the viewport (the eye's half), the
   eye's shift and the texture halves changing -- so the framebuffer never
   changes between the eyes (a change per draw ends a render pass, which
   costs four times the frame on Apple's GL and more on a tile-based GPU).

   The 3D is shifted sideways by depth, off-axis: what lies at the
   convergence depth D stays on the screen, what is farther goes behind it,
   what is nearer comes out of it:

       x_ndc' = x_ndc + h * (D / w - 1),   h = +sep/2 left, -sep/2 right

   w being the vertex's clip w and sep the separation at infinity in NDC
   (the host sets it from the screen's size: infinity one eye distance
   apart on it). 2D -- the HUD, full-screen passes, anything drawn with rhw
   1 -- is not shifted and sits on the screen. A pass that samples a stereo
   surface reads the half of the eye it draws, so composites and effects
   stay per eye. Surfaces of other shapes (VC3's 256x256 comm-window
   portrait) are drawn once, for both eyes. Guest memory and frame dumps get
   the left eye. */
static int s_stereo;                    /* this frame, latched at the flip */
static float s_st_sep = 0.04f;          /* separation at infinity, NDC */
static float s_st_depth = 8.0f;         /* convergence: clip w (VC3: ~metres) */
static GLfloat s_sbs[4];                /* per texture unit 0-3: 1 = a stereo surface */

/* ── Immersive (vr_view.h) ──────────────────────────────────────────────

   With a headset that wants it, a frame with 3D is drawn for the eyes
   themselves: each 3D vertex's game eye position comes back from its screen
   position and w through the game's projection, and goes through the eye's
   matrix (the head's pose and the eye's field of view) -- so the world
   fills the view and stays put as the head turns. The projection is taken
   from each fixed-function batch's matrices (vr_game_proj_from); the
   vertex-program batches (sprites, effects) use the newest one. 2D that is
   not full-screen goes to the HUD picture, shown on a panel the size of the
   game's view; full-screen 2D (fades, effects that redraw the scene) stays
   in the eyes. A frame with no 3D in it is shown on the virtual screen. */
static int s_imm;                       /* this frame is drawn immersive */
static Vc3VrView s_view;                /* with this pose */
static VrGameProj s_gp, s_bgp;          /* the newest projection; this batch's */
static int s_gp_ok, s_bgp_ok;
static uint32_t s_gp_good, s_gp_bad;    /* for the log */
static uint32_t s_3d_this, s_3d_prev;   /* 3D batches into a shown surface */
/* The surfaces shown lately: the game's swap chain (the profile's
 * gpu.buffers, 4 for VC3, which triple buffers), at most SHOWN_MAX. */
#define SHOWN_MAX 8
static uint32_t s_shown_va[SHOWN_MAX];
static int s_nshown = 4;

/* The game's own rules (chihiro_profile.h, gpu), latched when the back end
 * is registered: per-draw paths read these statics. */
static int (*s_hud_place)(float x0, float x1, float w);   /* NULL: kept with the picture */
static int s_pic_min_w = 320;           /* a full-screen texture this wide is a 4:3 picture */
static int s_lazy_rt = 1;               /* the lazy copies allowed */
static float s_zoom_tol = 0.5f;         /* focal lengths this close are one zoom */
/* Clip distances 0-2 and depth clamp as last set (set_cap): a fixed-function
 * draw wants clip 0, 1 (the CPU path's planes), 2 (one-pass stereo) and the
 * clamp; a CPU-path draw only clip 0 (its one-pass half, else 1.0). Set when
 * they change, not on and off around every draw. */
static int s_cap[4];

static void set_cap(int i, int on)
{
    static const GLenum cap[4] = { GL_CLIP_DISTANCE0, GL_CLIP_DISTANCE0 + 1,
                                   GL_CLIP_DISTANCE0 + 2, GL_DEPTH_CLAMP };
    if ((i < 3 && !s_has_clip) || (i == 3 && !s_has_dclamp) || s_cap[i] == on)
        return;
    if (on)
        glEnable(cap[i]);
    else
        glDisable(cap[i]);
    s_cap[i] = on;
}

static void caps_off(void)
{
    int i;
    for (i = 0; i < 4; i++)
        set_cap(i, 0);
}

/* The game's zoom (VC3 narrows its view for events and cutscenes): its
 * usual focal length -- the one most frames of the scene use -- is the
 * baseline, and each batch is re-projected with that baseline rather than
 * its own, so a narrower view comes out as the world magnified about the
 * game camera's axis by kx / baseline, as the game meant. The HUD panel and
 * the aim use the same baseline, so the marks the game draws in its zoomed
 * screen land on the magnified world. VC3_IMM_ZOOM=0: no zoom. */
#define ZOOM_SLOTS 16
static struct { float k; uint32_t n; } s_zoom_hist[ZOOM_SLOTS];
static float s_kbase;                   /* the baseline |kx|; 0 until known */

static void zoom_note(float kx)
{
    float a = fabsf(kx);
    int i, best = -1, empty = -1;
    if (!(a > 1.0f))
        return;
    for (i = 0; i < ZOOM_SLOTS; i++) {
        if (s_zoom_hist[i].n && fabsf(s_zoom_hist[i].k - a) < s_zoom_tol) {
            s_zoom_hist[i].n++;
            break;
        }
        if (!s_zoom_hist[i].n && empty < 0)
            empty = i;
    }
    if (i == ZOOM_SLOTS && empty >= 0) {
        s_zoom_hist[empty].k = a;
        s_zoom_hist[empty].n = 1;
    }
    for (i = 0; i < ZOOM_SLOTS; i++)
        if (s_zoom_hist[i].n && (best < 0 || s_zoom_hist[i].n > s_zoom_hist[best].n))
            best = i;
    if (best >= 0)
        s_kbase = s_zoom_hist[best].k;
}

/* A projection as the immersive view uses it: the baseline focal length,
 * this one's centre and aspect. */
static VrGameProj zoom_proj(const VrGameProj *g)
{
    static int on = -1;
    VrGameProj o = *g;
    if (on < 0)
        on = !(genv("IMM_ZOOM") && genv("IMM_ZOOM")[0] == '0');
    if (on && s_kbase > 0.0f && fabsf(g->kx) > 1.0f) {
        float z = s_kbase / fabsf(g->kx);
        o.kx = g->kx * z;
        o.ky = g->ky * z;
    }
    return o;
}

static GLuint s_hud_fbo, s_hud_tex;
static GLsizei s_hud_w, s_hud_h;
static uint32_t s_hud_frame;            /* the frame the HUD picture holds */

static int surf_stereo(GLsizei w, GLsizei h)
{
    return s_stereo && w * 3 == h * 4;
}

/* The host's: the separation for its screen's physical size (Quest). */
void vc3_gl_stereo_separation(float ndc)
{
    if (ndc > 0.0f && ndc < 0.5f)
        s_st_sep = ndc;
}

/* Fixed foveation (QCOM_texture_foveated) of the stereo surfaces: full
 * detail round each eye's centre of view, coarser towards the edges, one
 * focal point per eye's half. VC3_FOVEATION: unset or "auto", on for the
 * frames drawn immersive at native size (each eye's picture is the whole
 * field of view; the periphery is what the lenses blur anyway) and flat
 * (gain 0: every pixel full detail) for everything else -- the virtual
 * screen shows the whole picture; 1: always; 0: never. The gain and the
 * full-detail area (VC3_FOV_GAIN, default 4; VC3_FOV_AREA, 0.4) are the
 * extension's: pixel density falls off as 1 / (gain * distance^2 - area)
 * from the focal point, across -1..1. Skipped without the extension or two
 * focal points. Set when a surface is made (the texture bound) and at each
 * flip for the next frame (fov_frame). */
#if defined(VC3_GLES)
typedef void (*FovFn)(GLuint, GLuint, GLuint, GLfloat, GLfloat, GLfloat, GLfloat, GLfloat);
static FovFn s_fov_fn;
#endif
static int s_fov_mode = -1;             /* 0 never, 1 always, 2 auto */
static int s_fov_now;                   /* this frame foveated */

static int fov_mode(void)
{
#if defined(VC3_GLES)
    if (s_fov_mode < 0) {
        const char *e = genv("FOVEATION");
        GLint i, k = 0;
        int have = 0;
        s_fov_mode = !e || !strcmp(e, "auto") ? 2 : e[0] == '1' ? 1 : 0;
        if (s_fov_mode) {
            glGetIntegerv(GL_NUM_EXTENSIONS, &k);
            for (i = 0; i < k; i++) {
                const char *x = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
                if (x && !strcmp(x, "GL_QCOM_texture_foveated"))
                    have = 1;
            }
            if (have)
                s_fov_fn = (FovFn)eglGetProcAddress("glTextureFoveationParametersQCOM");
            if (!s_fov_fn)
                s_fov_mode = 0;
            fprintf(stderr, "[GL] foveation: %s\n", !s_fov_mode ? "not available"
                    : s_fov_mode == 1 ? "always" : "immersive frames at native size");
        }
    }
    return s_fov_mode;
#else
    return 0;
#endif
}

/* f's focal points for this frame (f->color bound or not: by name). */
static void fov_params(const Surf *f)
{
#if defined(VC3_GLES)
    float gain = 0.0f, area = 0.0f;
    int e;
    if (!f->fov)
        return;
    if (s_fov_now) {
        gain = genv("FOV_GAIN") ? (float)atof(genv("FOV_GAIN")) : 4.0f;
        area = genv("FOV_AREA") ? (float)atof(genv("FOV_AREA")) : 0.4f;
    }
    for (e = 0; e < 2; e++) {
        /* the eye's straight ahead in its half: its projection's centre
         * (an eye's field of view is wider outwards); rows top first */
        float cx = 0.0f, cy = 0.0f;
        if (s_imm && s_view.on) {
            float l = s_view.tan_fov[e][0], r = s_view.tan_fov[e][1];
            float u = s_view.tan_fov[e][2], d = s_view.tan_fov[e][3];
            if (r - l > 0.0f && u - d > 0.0f) {
                cx = -(r + l) / (r - l);
                cy = (u + d) / (u - d);
            }
        }
        s_fov_fn(f->color, 0, (GLuint)e, (e ? 0.5f : -0.5f) + 0.5f * cx, cy, gain,
                 gain, area);
    }
#else
    (void)f;
#endif
}

static void surf_foveate(Surf *f)
{
#if defined(VC3_GLES)
    GLint n = 0;
    f->fov = 0;
    if (!fov_mode())
        return;
    glTexParameteri(GL_TEXTURE_2D, 0x8BFB /* TEXTURE_FOVEATED_FEATURE_BITS_QCOM */,
                    0x1 /* FOVEATION_ENABLE_BIT */ | 0x2 /* SCALED_BIN_METHOD_BIT */);
    glGetTexParameteriv(GL_TEXTURE_2D, 0x8BFD /* NUM_FOCAL_POINTS_QUERY */, &n);
    if (n < 2)
        return;
    f->fov = 1;
    fov_params(f);
#else
    (void)f;
#endif
}

/* At the flip, for the next frame: foveated or flat (s_fov_now). */
static void fov_frame(void)
{
    int i, now;
    if (!fov_mode())
        return;
    now = s_fov_mode == 1 || (s_imm && s_view.scale == 0 && s_view.eye_w > 0);
    if (now == s_fov_now && !now)
        return;
    s_fov_now = now;
    for (i = 0; i < s_nsurfs; i++)
        fov_params(&s_surfs[i]);
}

/* Multisampling (VC3_GL_MSAA=2/4, GLES, off by default): the 3D surfaces
 * rendered with EXT_multisampled_render_to_texture -- the samples live in
 * the tile memory and are resolved into the ordinary texture when the pass
 * ends, so everything that reads a surface (sampling it, the self-copy, the
 * blits to the headset) sees resolved pixels with no resolve of its own,
 * and the bandwidth is a single-sampled surface's. Depth is multisampled
 * alike. 0 if unavailable or off; the sample count otherwise. */
#if defined(VC3_GLES)
typedef void (*MsTexFn)(GLenum, GLenum, GLenum, GLuint, GLint, GLsizei);
typedef void (*MsRbFn)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static MsTexFn s_ms_tex;
static MsRbFn s_ms_rb;
#endif
static int msaa_samples(void)
{
    static int n = -1;
#if defined(VC3_GLES)
    if (n < 0) {
        const char *e = genv("GL_MSAA");
        GLint i, k = 0, mx = 0, have = 0;
        n = e ? atoi(e) : 0;
        if (n > 1) {
            glGetIntegerv(GL_NUM_EXTENSIONS, &k);
            for (i = 0; i < k; i++) {
                const char *x = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
                if (x && !strcmp(x, "GL_EXT_multisampled_render_to_texture"))
                    have = 1;
            }
            if (have) {
                s_ms_tex = (MsTexFn)eglGetProcAddress("glFramebufferTexture2DMultisampleEXT");
                s_ms_rb = (MsRbFn)eglGetProcAddress("glRenderbufferStorageMultisampleEXT");
                glGetIntegerv(0x8D57 /* MAX_SAMPLES_EXT */, &mx);
            }
            if (!s_ms_tex || !s_ms_rb || mx < 2)
                n = 0;
            else if (n > mx)
                n = mx;
            fprintf(stderr, "[GL] MSAA: %s (%d samples)\n", n > 1 ? "on" : "not available", n);
        }
        if (n < 2)
            n = 0;
    }
#else
    n = 0;
#endif
    return n;
}

static void surf_alloc(Surf *f, GLsizei w, GLsizei h)
{
    GLsizei pw, ph;

    if (f->fbo) {
        glDeleteFramebuffers(1, &f->fbo);
        glDeleteTextures(1, &f->color);
        glDeleteRenderbuffers(1, &f->depth);
    }
    f->w = w;
    f->h = h;
    f->sx = s_sx;
    f->sy = s_sy;
    f->wide = surf_widens(w, h);
    pw = (GLsizei)((float)w * f->sx + 0.5f);
    ph = (GLsizei)((float)h * f->sy + 0.5f);
    f->cw = pw;
    if (f->wide)
        pw = (pw * 4 / 3 + 1) & ~1;
    f->pw = pw;
    f->ph = ph;
    f->st = surf_stereo(w, h);
    glGenTextures(1, &f->color);
    glBindTexture(GL_TEXTURE_2D, f->color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw * (f->st ? 2 : 1), ph, 0, GL_BGRA,
                 GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (f->st)
        surf_foveate(f);
    glGenRenderbuffers(1, &f->depth);
    glBindRenderbuffer(GL_RENDERBUFFER, f->depth);
    glGenFramebuffers(1, &f->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
#if defined(VC3_GLES)
    if (f->st && msaa_samples()) {      /* the 3D surfaces: stereo ones */
        s_ms_rb(GL_RENDERBUFFER, msaa_samples(), GL_DEPTH24_STENCIL8, pw * 2, ph);
        s_ms_tex(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->color, 0,
                 msaa_samples());
    } else
#endif
    {
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, pw * (f->st ? 2 : 1), ph);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, f->color, 0);
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, f->depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "[GL] surface %08X %dx%d incomplete\n", f->va, pw, ph);
}

static Surf *surf_find(uint32_t va)
{
    int i;
    for (i = 0; i < s_nsurfs; i++)
        if (s_surfs[i].va == va)
            return &s_surfs[i];
    return NULL;
}

/* VC3_GPU_TIME=1: the GPU's time per frame, by the surface drawn into --
 * timer queries opened at each change of surface (surf_bind) and read back
 * when done, never waited on. Logged once a second. */
#ifndef GL_TIME_ELAPSED
#define GL_TIME_ELAPSED 0x88BF            /* EXT_disjoint_timer_query on ES */
#endif
#define GT_RING 512
/* not surfaces: the copies, timed apart from the surface they serve */
#define GT_PRESENT 1u                     /* the frame's copy to the headset */
#define GT_SELF 2u                        /* snapshots for batches sampling their target */
#define GT_LAZY 3u                        /* lazy copies brought up to date */
static uint32_t s_gt_nself, s_gt_nlazy;  /* how many (GPU time log) */
static const char *gt_label(uint32_t va)
{
    return va == GT_PRESENT ? "present" : va == GT_SELF ? "self-copies"
         : va == GT_LAZY ? "lazy-copies" : NULL;
}
static struct { uint32_t draws, tris, declined; } s_rst;   /* fixed-function draws */
static uint32_t s_zp_frame;              /* visibility tests (GPU time log) */
static uint32_t s_sw_frame;              /* surface switches (GPU time log) */
static uint32_t s_va_hit, s_va_miss;     /* attribute pointers kept / set (log) */
static int s_gt_on = -1;
static GLuint s_gt_q[GT_RING];
static uint32_t s_gt_va[GT_RING];
static uint8_t s_gt_tr[GT_RING];          /* logged pass by pass ([GTSEG]) */
static uint32_t s_gt_fr[GT_RING];         /* the frame each was issued in */
static unsigned s_gt_head, s_gt_tail;     /* issued [tail, head) */
static int s_gt_open;
static uint32_t s_gt_cur_va;
#define GT_SURFS 16
static struct { uint32_t va; double ms; } s_gt_sum[GT_SURFS];
static double s_gt_total;
static uint32_t s_gt_frames;

static void gt_close(void)
{
    if (s_gt_open) {
        glEndQuery(GL_TIME_ELAPSED);
        s_gt_open = 0;
    }
}

static void gt_poll(void)
{
    while (s_gt_tail != s_gt_head) {
        unsigned k = s_gt_tail % GT_RING;
        GLuint avail = 0, ns = 0;
        int j;
        if (k == (s_gt_head - 1) % GT_RING && s_gt_open)
            break;                          /* still open */
        glGetQueryObjectuiv(s_gt_q[k], GL_QUERY_RESULT_AVAILABLE, &avail);
        if (!avail)
            break;
        glGetQueryObjectuiv(s_gt_q[k], GL_QUERY_RESULT, &ns);
        if (s_gt_tr[k])             /* the traced frame's passes, in order */
            fprintf(stderr, "[GTSEG] frame %u %08X %.3f ms\n", s_gt_fr[k], s_gt_va[k], ns / 1e6);
        s_gt_total += ns / 1e6;
        for (j = 0; j < GT_SURFS; j++)
            if (s_gt_sum[j].va == s_gt_va[k] || !s_gt_sum[j].va) {
                s_gt_sum[j].va = s_gt_va[k];
                s_gt_sum[j].ms += ns / 1e6;
                break;
            }
        s_gt_tail++;
    }
}

static void gt_surface(uint32_t va)
{
    if (s_gt_on < 0) {
        s_gt_on = genv("GPU_TIME") && genv("GPU_TIME")[0] == '1';
        if (s_gt_on)
            glGenQueries(GT_RING, s_gt_q);
    }
    if (!s_gt_on || (s_gt_open && va == s_gt_cur_va))
        return;
    s_sw_frame++;
    gt_close();
    gt_poll();
    if (s_gt_head - s_gt_tail >= GT_RING - 1)
        return;                             /* all in flight: skip */
    s_gt_va[s_gt_head % GT_RING] = va;
    /* VC3_GPU_TIME_SEG=1: every pass of every frame, in order ([GTSEG]) */
    s_gt_tr[s_gt_head % GT_RING] = s_ftr == 1 || genv("GPU_TIME_SEG");
    s_gt_fr[s_gt_head % GT_RING] = s_frame;
    if (s_ftr == 1)
        fprintf(stderr, "[GLTR] -- GPU timer: %08X\n", va);
    glBeginQuery(GL_TIME_ELAPSED, s_gt_q[s_gt_head % GT_RING]);
    s_gt_head++;
    s_gt_open = 1;
    s_gt_cur_va = va;
}

/* At the flip: close the frame's last query; once a second, the log. */
static void gt_flip(void)
{
    static uint64_t t0;
    uint64_t now;
    struct timespec ts;
    int j;
    if (s_gt_on <= 0)
        return;
    gt_close();
    gt_poll();
    s_gt_frames++;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (!t0)
        t0 = now;
    if (now - t0 < 1000000000ull)
        return;
    {
        char line[512];
        int n = snprintf(line, sizeof line, "[GPUTIME] %.1f ms/frame over %u frames"
                         " (fixed-function %u draws, %u triangles, %u visibility tests, %u surface switches,"
                         " %u self-copies, %u lazy copies a frame):",
                         s_gt_frames ? s_gt_total / s_gt_frames : 0.0, s_gt_frames,
                         s_gt_frames ? s_rst.draws / s_gt_frames : 0,
                         s_gt_frames ? s_rst.tris / s_gt_frames : 0,
                         s_gt_frames ? s_zp_frame / s_gt_frames : 0,
                         s_gt_frames ? s_sw_frame / s_gt_frames : 0,
                         s_gt_frames ? s_gt_nself / s_gt_frames : 0,
                         s_gt_frames ? s_gt_nlazy / s_gt_frames : 0);
        s_gt_nself = s_gt_nlazy = 0;
        s_rst.draws = s_rst.tris = 0;
        s_zp_frame = s_sw_frame = 0;
        fprintf(stderr, "[GPUTIME]   attribute pointers kept %u, set %u; state calls made %u,"
                " not repeated %u; uniforms sent %u, not repeated %u; program constants"
                " sent %u\n", s_va_hit, s_va_miss, s_sh.made, s_sh.skipped, s_uc_sent,
                s_uc_kept, s_vc_sent);
        s_sh.made = s_sh.skipped = 0;
        s_uc_sent = s_uc_kept = s_vc_sent = 0;
        s_va_hit = s_va_miss = 0;
        for (j = 0; j < GT_SURFS && s_gt_sum[j].va && n < (int)sizeof line - 40; j++)
            if (s_gt_sum[j].ms / s_gt_frames >= 0.3) {
                const Surf *q = surf_find(s_gt_sum[j].va);
                if (gt_label(s_gt_sum[j].va))
                    n += snprintf(line + n, sizeof line - (size_t)n, " %s %.1f",
                                  gt_label(s_gt_sum[j].va), s_gt_sum[j].ms / s_gt_frames);
                else
                    n += snprintf(line + n, sizeof line - (size_t)n, " %08X(%dx%d%s) %.1f",
                                  s_gt_sum[j].va, q ? q->w : 0, q ? q->h : 0,
                                  q && q->st ? " st" : "", s_gt_sum[j].ms / s_gt_frames);
            }
        fprintf(stderr, "%s\n", line);
    }
    memset(s_gt_sum, 0, sizeof s_gt_sum);
    s_gt_total = 0;
    s_gt_frames = 0;
    t0 = now;
}

static GLuint s_bound_fbo;              /* surf_bind's, for lazy_catch_up */
static uint32_t s_bound_va;

/* The framebuffer for a guest surface, created or grown to fit, and bound.
 * A change of scale or aspect remakes it at the first draw after the flip
 * that took the change; VC3 redraws every surface each frame. */
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
    if (!f->fbo || w > f->w || h > f->h || f->sx != s_sx || f->sy != s_sy
        || f->wide != surf_widens(w > f->w ? w : f->w, h > f->h ? h : f->h)
        || f->st != surf_stereo(w > f->w ? w : f->w, h > f->h ? h : f->h))
        surf_alloc(f, w > f->w ? w : f->w, h > f->h ? h : f->h);
    f->pitch = s->pitch;
    f->bpp = bpp;
    if (s_fx.draw && f == s_fx.s && !s_fx.swapped) {   /* into the effect target */
        int sc = fx_scale();
        s_fx.save_fbo = f->fbo;
        s_fx.save_pw = f->pw;
        s_fx.save_ph = f->ph;
        s_fx.save_cw = f->cw;
        f->fbo = s_fx.fbo;
        f->pw = s_fx.w;
        f->ph = s_fx.h;
        f->cw = (f->cw + sc - 1) / sc;
        s_fx.swapped = 1;
    }
    gt_surface(f->va);
    z_switch(f);
    s_bound_fbo = f->fbo;
    s_bound_va = f->va;
    glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
    glViewport(0, 0, f->pw, f->ph);
    return f;
}

/* 16:9 HUD layout, per surface: the boxes of this frame's 2D overlays, and
 * the widgets the last frame's made -- boxes that overlap, or glyphs of one
 * text row, joined -- each with its placement. */
#define HUD_BOXES 512
#define HUD_WIDGETS 128
typedef struct { float x0, x1, y0, y1; } HudBox;
struct HudLayout {
    uint32_t frame;              /* the frame boxes[] belongs to */
    int nbox, nwidget;
    HudBox box[HUD_BOXES];
    HudBox widget[HUD_WIDGETS];
    signed char place[HUD_WIDGETS];
};

static int hud_joined(const HudBox *a, const HudBox *b)
{
    float ix = (a->x1 < b->x1 ? a->x1 : b->x1) - (a->x0 > b->x0 ? a->x0 : b->x0);
    float iy = (a->y1 < b->y1 ? a->y1 : b->y1) - (a->y0 > b->y0 ? a->y0 : b->y0);

    if (ix > 2.0f && iy > 2.0f)                     /* parts of one widget */
        return 1;
    return fabsf(a->y0 - b->y0) <= 2.0f && fabsf(a->y1 - b->y1) <= 2.0f
        && ix > -8.0f;                              /* glyphs of one row */
}

/* Where a widget goes: -1 left edge, 1 right edge, 0 middle -- the game's
 * rule (profile gpu.hud_place; VC3: a widget against a side, within 5% of
 * it, keeping to that side's 3/8 -- its score and gun panels). With none,
 * everything stays with the 4:3 picture. */
static int hud_place_of(const HudBox *b, float w)
{
    return s_hud_place ? s_hud_place(b->x0, b->x1, w) : 0;
}

/* The finished frame's boxes into widgets (union of joined boxes). */
static void hud_close_frame(struct HudLayout *L, float w)
{
    static int parent[HUD_BOXES];
    int i, j, n = L->nbox;

    for (i = 0; i < n; i++)
        parent[i] = i;
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (hud_joined(&L->box[i], &L->box[j])) {
                int a = i, b = j;
                while (parent[a] != a) a = parent[a];
                while (parent[b] != b) b = parent[b];
                if (a != b)
                    parent[b] = a;
            }
    /* One widget per root, its box the union of its boxes. */
    {
        static int root_of_widget[HUD_WIDGETS];
        L->nwidget = 0;
        for (i = 0; i < n; i++) {
            int r = i, k;
            while (parent[r] != r) r = parent[r];
            for (k = 0; k < L->nwidget; k++)
                if (root_of_widget[k] == r)
                    break;
            if (k == L->nwidget) {
                if (k == HUD_WIDGETS)
                    continue;
                root_of_widget[k] = r;
                L->widget[k] = L->box[i];
                L->nwidget++;
            } else {
                HudBox *u = &L->widget[k];
                if (L->box[i].x0 < u->x0) u->x0 = L->box[i].x0;
                if (L->box[i].x1 > u->x1) u->x1 = L->box[i].x1;
                if (L->box[i].y0 < u->y0) u->y0 = L->box[i].y0;
                if (L->box[i].y1 > u->y1) u->y1 = L->box[i].y1;
            }
        }
        for (i = 0; i < L->nwidget; i++)
            L->place[i] = (signed char)hud_place_of(&L->widget[i], w);
    }
    L->nbox = 0;
}

/* Clip-space x transform for a batch drawn into f: x' = x * scale + offset.
 *
 * On a widened surface the guest's picture is the middle w * sc of pw. What
 * goes where:
 *
 *   3D (fixed-function batches, VC3's world and its lock-on rings): x
 *     shrinks to the middle, and whatever the view has beyond its old edges
 *     lands in the new sides -- the game's own frustum culling decides how
 *     much that is.
 *   Full-screen passes (a screen-space batch spanning the whole guest
 *     width): stretched edge to edge when they carry the widened view (they
 *     sample a widened surface: VC3's composite of its 3D surface onto the
 *     back buffer) or are one flat colour (fades, tints, letterbox bars). A
 *     full-screen picture (a texture 320 wide or more: the attract movie,
 *     which the CPU decodes into a surface of its own, menu backgrounds)
 *     stays 4:3 in the middle, and the frame counts as 4:3 from there on,
 *     so its overlays stay with it. A small texture spread over the screen
 *     (an effect) is stretched after a widened composite, else kept 4:3.
 *   2D overlays: after a widened composite this frame (gameplay, in-game
 *     cutscenes), the HUD goes to the screen's edges: a batch wholly in the
 *     left 3/8 moves to the left edge, one wholly in the right 3/8 to the
 *     right edge, the rest stays in the middle; a batch overlapping the one
 *     before keeps its placement, so a widget's parts stay together.
 *     Otherwise (menus, attract) overlays stay 4:3 in the middle with the
 *     rest of the picture. */
static void surf_xform(Surf *f, const Nv2aBatch *b, float *scale, float *offset)
{
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
    float mid = (float)f->cw / (float)f->pw;
    const Surf *src;
    int place;

    *scale = 1.0f;
    *offset = 0.0f;
    if (!f->wide)
        return;
    *scale = mid;
    {
        Nv2aRawExtent e;
        if (!batch_extent(b, &e))
            return;
        x0 = e.x0; x1 = e.x1; y0 = e.y0; y1 = e.y1;
        u0 = e.u0; u1 = e.u1; v0 = e.v0; v1 = e.v1;
    }
    if (x0 <= 1.0f && x1 >= (float)f->w - 1.0f) {
        src = b->texture ? surf_find(b->texture->offset) : NULL;
        if (!b->texture || (u1 - u0 < 1e-3f && v1 - v0 < 1e-3f)) {
            *scale = 1.0f;                        /* a flat colour */
        } else if (src && src->wide && !src->cpu_owned) {
            *scale = 1.0f;                        /* the widened view */
            if (src != f)
                f->hor_frame = s_frame;
        } else if (b->texture->width >= (uint32_t)s_pic_min_w) {
            f->hor_frame = 0;                     /* a 4:3 picture: movie, menu */
        } else if (f->hor_frame == s_frame) {
            *scale = 1.0f;                        /* a screen effect in play */
        }
        return;
    }
    if (f->hor_frame != s_frame)
        return;
    {
        struct HudLayout *L = f->hud;
        HudBox bx;
        int i;

        if (!L && !(L = f->hud = (struct HudLayout *)calloc(1, sizeof *L)))
            return;
        /* A new frame: the last one drawn makes the widgets. Frames a few
         * flips apart still count (VC3's attract draws every other flip). */
        if (L->frame != s_frame) {
            if (s_frame - L->frame <= 4)
                hud_close_frame(L, (float)f->w);
            else
                L->nwidget = L->nbox = 0;
            L->frame = s_frame;
        }
        bx.x0 = x0; bx.x1 = x1; bx.y0 = y0; bx.y1 = y1;
        if (L->nbox < HUD_BOXES)
            L->box[L->nbox++] = bx;
        /* The last frame's widget this batch falls in, else the batch's
         * own box (a new element, for its first frame). */
        /* The widget it was part of (inside it, give or take a pixel; a
         * mere touch is not enough: the life bar's frame touches the gun
         * panel), else the one it extends -- the next glyph of a text row,
         * VC3 types its briefings out a letter a frame. */
        place = hud_place_of(&bx, (float)f->w);
        {
            int inside = -1, row = -1;
            for (i = 0; i < L->nwidget; i++) {
                const HudBox *u = &L->widget[i];
                if (x0 >= u->x0 - 1.0f && x1 <= u->x1 + 1.0f
                    && y0 >= u->y0 - 1.0f && y1 <= u->y1 + 1.0f) {
                    inside = i;
                    break;
                }
                if (row < 0 && hud_joined(&bx, u))
                    row = i;
            }
            if (inside >= 0)
                place = L->place[inside];
            else if (row >= 0)
                place = L->place[row];
        }
    }
    *offset = (float)place * (1.0f - mid);
}

/* A framebuffer holding f at its guest size, for reading into guest memory:
 * f's own at 1x, else the middle part of it scaled down into a shared one. */
static GLuint surf_resolve(const Surf *f)
{
    static GLuint fbo, tex;
    static GLsizei rw, rh;
    GLint x0;

    clear_flush((Surf *)f, GL_COLOR_BUFFER_BIT);
    if (f->pw == f->w && f->ph == f->h)
        return f->fbo;
    if (!fbo || rw != f->w || rh != f->h) {
        if (!tex)
            glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, f->w, f->h, 0, GL_BGRA,
                     GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
        if (!fbo)
            glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, tex, 0);
        rw = f->w;
        rh = f->h;
    }
    x0 = (f->pw - f->cw) / 2;
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, f->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glBlitFramebuffer(x0, 0, x0 + f->cw, f->ph, 0, 0, f->w, f->h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    return fbo;
}

/* Write a surface back into guest memory, row 0 first. */
static void surf_readback(Surf *f)
{
    uint8_t *dst = (uint8_t *)xbox_GetMemoryOffset() + f->va;
    GLsizei rows = f->h;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, surf_resolve(f));
#if defined(VC3_GLES)
    {
        static uint8_t *tmp;
        static size_t cap;
        size_t need = (size_t)f->w * rows * 4;
        if (cap < need) {
            free(tmp);
            tmp = (uint8_t *)malloc(need);
            cap = tmp ? need : 0;
        }
        if (tmp) {
            glReadPixels(0, 0, f->w, rows, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
            es_to_guest(dst, f->pitch, f->bpp, tmp, f->w, rows);
        }
    }
#else
    glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)(f->pitch / f->bpp));
    if (f->bpp == 2)
        glReadPixels(0, 0, f->w, rows, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, dst);
    else
        glReadPixels(0, 0, f->w, rows, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, dst);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
#endif
    f->dirty = 0;
    f->memq_valid = 0;
}

/* The presented surface, read back without waiting: each flip starts a read
 * into one pixel-pack buffer and copies the other -- started a flip earlier,
 * long finished -- into guest memory. A synchronous glReadPixels waited for
 * everything queued on the GPU, the window's vsync'd present included, so the
 * title waited on the display; this shows frames one flip late instead.
 *
 * When the window should show more than guest memory holds -- a higher
 * internal resolution, 16:9, or in-between frames -- the drawn frame is
 * read too, whole (BGRA, pw x ph), with any frames made between it and the
 * one before, and at the next flip they go to the window as a group
 * (xbox_FramebufferWindowPresentFrames). Otherwise the window copies guest
 * memory as it always has. */
typedef struct {
    GLuint pbo;
    GLsizeiptr cap;
    uint32_t va, bytes;          /* where and how much, 0 when none pending */
    GLsizei gw, gh;              /* ES: read as RGBA8 rows, converted on the copy */
    uint32_t gpitch, gbpp;
    /* The window's group: the drawn frame (show_pbo) and nmid frames made
     * before it, each sw x sh BGRA. */
    int group, nmid;
    GLuint show_pbo, mid_pbo[VC3_FPS_MAX / 60 - 1];
    GLsizeiptr show_cap;
    GLsizei sw, sh;
    int cut;                     /* VC3_FPS_DEBUG: made as a scene cut */
} Readback;

static Readback s_rb[2];
static int s_rb_next;

static int interp_make(const Surf *f, GLuint *pbos, int n);
static void interp_scene_snap(const Surf *f, const Nv2aBatch *b);
static void interp_dump(const void *const *px, int n, uint32_t pitch,
                        uint32_t bpp, GLsizei w, GLsizei h, int cut);
static int s_ip_last_cut;
extern void xbox_FramebufferWindowPresentFrames(const void *const *frames, int n,
                                                uint32_t w, uint32_t h, uint32_t pitch,
                                                uint32_t bpp);
extern void xbox_FramebufferWindowSetPicture(uint32_t lw, uint32_t lh);

/* Start reading f whole for the window, with its in-between frames. */
static void show_read(Readback *r, const Surf *f, int fps)
{
    GLsizei sw = f->pw * (f->st ? 2 : 1);     /* stereo: left | right */
    GLsizeiptr bytes = (GLsizeiptr)sw * f->ph * 4;
    int i;

    if (r->show_cap < bytes) {
        if (!r->show_pbo)
            glGenBuffers(1, &r->show_pbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, r->show_pbo);
        glBufferData(GL_PIXEL_PACK_BUFFER, bytes, NULL, GL_STREAM_READ);
        for (i = 0; i < VC3_FPS_MAX / 60 - 1; i++) {
            if (!r->mid_pbo[i])
                glGenBuffers(1, &r->mid_pbo[i]);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, r->mid_pbo[i]);
            glBufferData(GL_PIXEL_PACK_BUFFER, bytes, NULL, GL_STREAM_READ);
        }
        r->show_cap = bytes;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, r->show_pbo);
    clear_flush((Surf *)f, GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, f->fbo);
    glReadPixels(0, 0, sw, f->ph, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    r->sw = sw;
    r->sh = f->ph;
    s_ip_last_cut = 0;
    /* No in-between frames for a stereo pair. */
    r->nmid = f->st ? 0 : interp_make(f, r->mid_pbo, fps / 60 - 1);
    r->cut = s_ip_last_cut;
    r->group = 1;
}

/* Map last flip's group and hand it to the window. */
static void show_publish(Readback *r)
{
    const void *px[VC3_FPS_MAX / 60];
    GLsizeiptr bytes = (GLsizeiptr)r->sw * r->sh * 4;
    int n = 0, i;

    for (i = 0; i <= r->nmid; i++) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, i < r->nmid ? r->mid_pbo[i] : r->show_pbo);
        px[n] = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT);
        if (!px[n])
            break;
        n++;
    }
    if (n == r->nmid + 1) {
        xbox_FramebufferWindowPresentFrames(px, n, (uint32_t)r->sw, (uint32_t)r->sh,
                                            (uint32_t)r->sw * 4, 4);
        interp_dump(px, n, (uint32_t)r->sw * 4, 4, r->sw, r->sh, r->cut);
    }
    for (i = 0; i < n; i++) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, i < r->nmid ? r->mid_pbo[i] : r->show_pbo);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    r->group = 0;
}

static void surf_readback_async(Surf *f)
{
    Readback *r = &s_rb[s_rb_next], *prev = &s_rb[s_rb_next ^ 1];
#if defined(VC3_GLES)
    GLsizeiptr bytes = (GLsizeiptr)f->w * f->h * 4;
#else
    GLsizeiptr bytes = (GLsizeiptr)f->pitch * f->h;
#endif
    int fps = vc3_fps_get();

    if (!r->pbo)
        glGenBuffers(1, &r->pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, r->pbo);
    if (r->cap < bytes) {
        glBufferData(GL_PIXEL_PACK_BUFFER, bytes, NULL, GL_STREAM_READ);
        r->cap = bytes;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, surf_resolve(f));
#if defined(VC3_GLES)
    glReadPixels(0, 0, f->w, f->h, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
#else
    glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)(f->pitch / f->bpp));
    if (f->bpp == 2)
        glReadPixels(0, 0, f->w, f->h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
    else
        glReadPixels(0, 0, f->w, f->h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
#endif
    r->va = f->va;
    r->bytes = (uint32_t)bytes;
    r->gw = f->w;
    r->gh = f->h;
    r->gpitch = f->pitch;
    r->gbpp = f->bpp;
    r->group = 0;
    f->dirty = 0;

#if !defined(VC3_GLES)
    /* The Quest shows the drawn frame itself (vc3_quest_frame), with the
     * headset making any in-between frames. */
    if (fps > 60 || f->pw != f->w || f->ph != f->h || f->st)
        show_read(r, f, fps);
#else
    (void)fps;
#endif

    if (prev->pbo && prev->bytes) {
        const void *src;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, prev->pbo);
        src = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, prev->bytes, GL_MAP_READ_BIT);
        if (src) {
            Surf *pf = surf_find(prev->va);
#if defined(VC3_GLES)
            es_to_guest((uint8_t *)xbox_GetMemoryOffset() + prev->va, prev->gpitch,
                        prev->gbpp, (const uint8_t *)src, prev->gw, prev->gh);
#else
            memcpy((uint8_t *)xbox_GetMemoryOffset() + prev->va, src, prev->bytes);
#endif
            if (pf)
                pf->memq_valid = 0;
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        prev->bytes = 0;
        /* Read at 1x, 4:3 and 60 fps (no group), the frame is left to the
         * window's plain copy of guest memory. */
        if (prev->group)
            show_publish(prev);
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
    int streamed;              /* its memory has changed since the first upload */
    int mipped;                /* its GL mip chain is built from this upload */
    GLuint tex;
    TexdecAlloc alloc;         /* the storage the GPU decode gave it (gl_texdec.c) */
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

/* Four independent lanes over 32-byte steps, folded at the end: the one
 * chain of multiplies ran at a word per multiply latency, and the full
 * check of every texture a frame was most of tex_bind's time. */
static uint64_t hash_bytes(const uint8_t *p, size_t n)
{
    uint64_t h0 = 0xcbf29ce484222325ull, h1 = 0x9e3779b97f4a7c15ull,
             h2 = 0xc2b2ae3d27d4eb4full, h3 = 0x165667b19e3779f9ull, h;
    size_t i = 0;

    for (; i + 32 <= n; i += 32) {
        uint64_t w[4];
        memcpy(w, p + i, 32);
        h0 = (h0 ^ w[0]) * 0x100000001b3ull; h0 ^= h0 >> 29;
        h1 = (h1 ^ w[1]) * 0x100000001b3ull; h1 ^= h1 >> 29;
        h2 = (h2 ^ w[2]) * 0x100000001b3ull; h2 ^= h2 >> 29;
        h3 = (h3 ^ w[3]) * 0x100000001b3ull; h3 ^= h3 >> 29;
    }
    h = h0 ^ (h1 * 31) ^ (h2 * 1031) ^ (h3 * 65599) ^ (uint64_t)n;
    for (; i + 8 <= n; i += 8) {
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

/* Sampling state lives in sampler objects, one per (filter, wrap) pair the
 * titles use, bound per unit: setting filter and wrap on the texture at
 * every bind -- five glTexParameteri per draw -- made the Metal-backed driver
 * rebuild its sampler and texture state for nearly every draw. The alpha
 * swizzle is a texture parameter and is set only when it changes. */
static int s_unit;
static GLuint s_unit_sampler[8];
static GLint s_swz[16384];     /* tex_sampling's alpha swizzle, by name; 0 unknown */

static void active_tex(int unit)
{
    if (unit != s_unit) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
        s_unit = unit;
    }
}

static GLint gl_wrap(uint32_t mode);

/* Mipmaps. The NV2A's min filter (SET_TEXTURE_FILTER bits 16-23) is
 * 1 point, 2 linear, 3-6 the four mipmapped filters (box/tent within a
 * level x nearest/tent between levels), 7 convolution. The decoder uploads
 * level 0 only, so a texture the guest gave a mip chain (FORMAT's levels)
 * and samples with a mip filter gets its chain from glGenerateMipmap at
 * upload; without one, every minified texel aliased -- the shimmer of
 * distant floors and walls, worst in VR, where the head never keeps still.
 * Not for a texture streamed through its memory (faces, movie frames:
 * re-uploaded every frame) or a render target.
 *
 * VC3_GL_MIPS: "guest" (default) as above; "all" also any texture with a
 * linear min filter and more than one level's worth of size; 0 none.
 * VC3_GL_ANISO: the anisotropy of mipmapped sampling (EXT_texture_filter_
 * anisotropic, capped to the driver's maximum), default 4; 1 off. */
static int s_mips = -1;
static float s_aniso = -1.0f;

static void mips_init(void)
{
    const char *e = ch_env("GL_MIPS");
    const char *a = ch_env("GL_ANISO");
    GLfloat mx = 1.0f;
    s_mips = !e ? 1 : !strcmp(e, "all") ? 2 : *e != '0';
    s_aniso = a ? (float)atof(a) : 4.0f;
    if (s_aniso > 1.0f) {
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        int have = 0;
#if !defined(VC3_GLES)
        have = 1;                       /* core in GL 4.6; on every Mac driver */
#endif
        if (!ext) {                     /* a core context lists them one by one */
            GLint i, n = 0;
            glGetIntegerv(GL_NUM_EXTENSIONS, &n);
            for (i = 0; i < n; i++) {
                const char *x = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
                if (x && strstr(x, "texture_filter_anisotropic"))
                    have = 1;
            }
        } else if (strstr(ext, "texture_filter_anisotropic")) {
            have = 1;
        }
        if (have)
            glGetFloatv(0x84FF /* MAX_TEXTURE_MAX_ANISOTROPY */, &mx);
        if (s_aniso > mx)
            s_aniso = mx;
        if (!have || mx <= 1.0f)
            s_aniso = 1.0f;
        while (glGetError() != GL_NO_ERROR)
            ;
    }
    if (s_aniso < 1.0f)
        s_aniso = 1.0f;
    fprintf(stderr, "[GL] mipmaps %s, anisotropy %.0fx\n",
            s_mips == 2 ? "all" : s_mips ? "where the guest has them" : "off", s_aniso);
}

/* Whether a texture uploaded now should get a mip chain. */
static int mips_wanted(const Nv2aTexture *t)
{
    uint32_t min = (t->filter >> 16) & 0xFF;
    if (s_mips < 0)
        mips_init();
    if (!s_mips || t->cube || t->width < 4 || t->height < 4)
        return 0;
    if (min >= 3 && min <= 6 && t->levels > 1)
        return 1;
    /* "all": a power-of-two texture (not a linear image: movie frames,
     * 640 wide) sampled with a linear min filter */
    return s_mips == 2 && min != 1 && (t->width > 8 || t->height > 8)
        && !(t->width & (t->width - 1)) && !(t->height & (t->height - 1));
}

static void tex_sampling(GLuint tex, const Nv2aTexture *t, int mipped)
{
    static GLuint samplers[2][7][3][3];   /* [mag point][min filter][u][v] */
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    GLint wu = gl_wrap(t->addr_u), wv = gl_wrap(t->addr_v);
    int iu = wu == GL_REPEAT ? 0 : wu == GL_MIRRORED_REPEAT ? 1 : 2;
    int iv = wv == GL_REPEAT ? 0 : wv == GL_MIRRORED_REPEAT ? 1 : 2;
    /* 0 point, 1 linear, 2-5 the guest's mip filters, 6 trilinear ("all") */
    int im = min == 1 ? 0 : !mipped ? 1 : (min >= 3 && min <= 6) ? (int)min - 1 : 6;
    GLuint *sm = &samplers[mag == 1][im][iu][iv];
    GLint a = fmt_has_alpha(t->color) ? GL_ALPHA : GL_ONE;

    if (!*sm) {
        static const GLenum MINF[7] = {
            GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST,
            GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR };
        glGenSamplers(1, sm);
        /* SET_TEXTURE_FILTER: 1 is point sampling, anything else filters. */
        glSamplerParameteri(*sm, GL_TEXTURE_MAG_FILTER, mag == 1 ? GL_NEAREST : GL_LINEAR);
        glSamplerParameteri(*sm, GL_TEXTURE_MIN_FILTER, (GLint)MINF[im]);
        glSamplerParameteri(*sm, GL_TEXTURE_WRAP_S, wu);
        glSamplerParameteri(*sm, GL_TEXTURE_WRAP_T, wv);
        if (im >= 2 && s_aniso > 1.0f)
            glSamplerParameterf(*sm, 0x84FE /* TEXTURE_MAX_ANISOTROPY */, s_aniso);
    }
    if (s_unit_sampler[s_unit] != *sm) {
        glBindSampler((GLuint)s_unit, *sm);
        s_unit_sampler[s_unit] = *sm;
    }
    if (tex >= 16384 || s_swz[tex] != a) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, a);
        if (tex < 16384)
            s_swz[tex] = a;
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
            uint64_t c0 = g_texstat_on ? texstat_now() : 0, cdec = 0;
            for (f = 0; f < 6; f++) {
                Nv2aTexture tf = *t;
                tf.offset = t->offset + (uint32_t)(face * (size_t)f);
                tf.cube = 0;
                tf.levels = 1;
                uint64_t d0 = g_texstat_on ? texstat_now() : 0;
                if (!nv2a_backend_decode_texture(&tf, s_decode)) {
                    s_st.fail[t->color & 0xFF]++;
                    e->hash = 0;
                    return 0;
                }
                if (g_texstat_on)
                    cdec += texstat_now() - d0;
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8,
                             (GLsizei)t->width, (GLsizei)t->height, 0, GL_BGRA,
                             GL_UNSIGNED_INT_8_8_8_8_REV, s_decode);
                es_bgra(GL_TEXTURE_CUBE_MAP);
            }
            s_st.uploads++;
            if (g_texstat_on)
                texstat_add(t->color, 0, (uint64_t)need * 6, cdec,
                            texstat_now() - c0 - cdec);
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

/* A texture whose GPU decode gave it immutable storage (ES compute) cannot
 * take another shape: a new name instead, the old one's cached state gone. */
static void tex_renew(TexEntry *e)
{
    if (!e->alloc.immut)
        return;
    glDeleteTextures(1, &e->tex);
    if (e->tex < 16384)
        s_swz[e->tex] = 0;
    glGenTextures(1, &e->tex);
    if (e->tex < 16384)
        s_swz[e->tex] = 0;
    memset(&e->alloc, 0, sizeof e->alloc);
    glBindTexture(GL_TEXTURE_2D, e->tex);
}

/* Bind the texture a batch samples to unit 0; returns 0 to draw untextured.
 * *sw, *sh receive the size its texel coordinates are relative to. */
static int tex_bind(const Nv2aTexture *t, float *sw, float *sh)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    Surf *rt = surf_find(t->offset);
    size_t slot, i;
    TexEntry *e = NULL;
    GLuint bound;

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
        lazy_catch_up(rt);
        clear_flush(rt, GL_COLOR_BUFFER_BIT);
        if (genv("RT_USE_LOG")) {   /* which render targets are sampled (debug) */
            static uint32_t vas[16], cnt[16], fr;
            int k;
            if (fr != s_frame && s_frame % 120 == 0) {
                for (k = 0; k < 16 && vas[k]; k++)
                    fprintf(stderr, "[RTUSE] %08X sampled %u times\n", vas[k], cnt[k]);
                memset(vas, 0, sizeof vas);
                memset(cnt, 0, sizeof cnt);
            }
            fr = s_frame;
            for (k = 0; k < 16 && vas[k] && vas[k] != rt->va; k++)
                ;
            if (k < 16) {
                vas[k] = rt->va;
                cnt[k]++;
            }
        }
        /* drawing into the effect target: the render target reduced alike */
        bound = s_fx.draw && rt != s_fx.s ? fx_small(rt) : rt->color;
        glBindTexture(GL_TEXTURE_2D, bound);
        if (rt->st && s_unit < 4)
            s_sbs[s_unit] = 1.0f;
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
            else
                tex_renew(e);
            memset(&e->alloc, 0, sizeof e->alloc);
            e->offset = t->offset;
            e->w = t->width;
            e->h = t->height;
            e->fmt = t->color;
            e->pitch = t->pitch;
            e->hash = 0;
            e->quick = 0;
            e->checked = 0;
            e->streamed = 0;
            e->mipped = 0;
        }
        glBindTexture(GL_TEXTURE_2D, e->tex);
        bound = e->tex;
        /* Full check every fourth frame for a texture that has never
         * changed; every frame for one the CPU has rewritten since its first
         * upload (the comm window's faces, movie frames); and at once
         * whenever the sampled hash (every bind) says the memory now holds
         * something else -- how a texture streamed through one address
         * within a frame shows. */
        /* A texture that has never changed and was already checked this
         * frame skips even the sampled hash (64 scattered reads a bind):
         * VC3 binds its level textures hundreds of times a frame. A change
         * to one then shows the next frame; a streamed texture keeps its
         * per-bind check. VC3_GL_TEXQUICK=0: every bind, as before. */
        static int quick_skip = -1;
        if (quick_skip < 0)
            quick_skip = !(genv("GL_TEXQUICK") && *genv("GL_TEXQUICK") == '0');
        if (quick_skip && e->hash && !e->streamed && e->checked == s_frame) {
            s_st.qskip++;
            goto tex_ready;
        }
        uint64_t h0 = g_texstat_on ? texstat_now() : 0;
        uint64_t q = hash_sampled(mem + t->offset, tex_bytes(t));
        if (q != e->quick || !e->checked
            || (e->streamed ? e->checked != s_frame : s_frame - e->checked >= 4)) {
            uint64_t h = hash_bytes(mem + t->offset, tex_bytes(t)), d0 = 0, d1 = 0;
            if (t->palette)                 /* P8: a new palette is a new texture */
                h ^= hash_bytes(mem + t->palette, 1024) * 0x9e3779b97f4a7c15ull;
            if (g_texstat_on)
                texstat_hash(texstat_now() - h0);
            e->checked = s_frame;
            e->quick = q;
            if (h != e->hash || !e->hash) {
                size_t need = (size_t)t->width * t->height;
                if (e->hash)
                    e->streamed = 1;        /* rewritten: checked every frame now */
                /* On the GPU (gl_texdec.c) unless VC3_TEX_GPU=0 or a format
                 * it does not take; the CPU decode below otherwise. */
                if (texdec_can(t)) {
                    int mipped = 0, path;
                    if (g_texstat_on)
                        d0 = texstat_now();
                    path = texdec_upload(e->tex, &e->alloc, t, !e->streamed && mips_wanted(t),
                                         &mipped);
                    if (path) {
                        s_st.uploads++;
                        e->mipped = mipped;
                        s_st.mipped += mipped != 0;
                        if (s_ftr == 1)
                            fprintf(stderr, "[GLTR]   upload %08X %ux%u fmt %02X (%s)\n",
                                    t->offset, t->width, t->height, t->color & 0xFF,
                                    path == 2 ? "compressed" : "GPU decode");
                        if (g_texstat_on)
                            texstat_add(t->color, path, (uint64_t)need, texstat_now() - d0, 0);
                        e->hash = h;
                        goto tex_ready;
                    }
                }
                tex_renew(e);
                if (e->alloc.levels) {      /* the GPU path set a level range */
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000);
                    memset(&e->alloc, 0, sizeof e->alloc);
                }
                if (need > s_decode_cap) {
                    free(s_decode);
                    s_decode = (uint32_t *)malloc(need * 4);
                    s_decode_cap = s_decode ? need : 0;
                }
                if (g_texstat_on)
                    d0 = texstat_now();
                if (!s_decode || !nv2a_backend_decode_texture(t, s_decode)) {
                    s_st.fail[t->color & 0xFF]++;
                    e->hash = 0;
                    return 0;
                }
                if (g_texstat_on)
                    d1 = texstat_now();
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
                if (genv("GL_TEXDUMP") && strstr(genv("GL_TEXDUMP"), want)) {
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
                es_bgra(GL_TEXTURE_2D);
                e->mipped = !e->streamed && mips_wanted(t);
                if (genv("GL_MIPLOG")) {    /* the uploads' filters and levels */
                    static int nlog;
                    if (nlog++ < 20000)
                        fprintf(stderr, "[MIPLOG] %08X %ux%u fmt %02X min %u levels %u pitch %u "
                                "-> %s\n", t->offset, t->width, t->height, t->color & 0xFF,
                                (t->filter >> 16) & 0xFF, t->levels, t->pitch,
                                e->mipped ? "mips" : "none");
                }
                if (e->mipped) {
                    glGenerateMipmap(GL_TEXTURE_2D);
                    s_st.mipped++;
                }
                if (g_texstat_on)
                    texstat_add(t->color, 0, (uint64_t)t->width * t->height, d1 - d0,
                                texstat_now() - d1);
                e->hash = h;
            }
        } else if (g_texstat_on) {
            texstat_hash(texstat_now() - h0);
        }
tex_ready:
        e->used = s_frame;
        *sw = (float)t->width;
        *sh = (float)t->height;
    }
    tex_sampling(bound, t, e && e->mipped);
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
    if (t->offset != f->va)
        return tex_bind(t, sw, sh);
    if (!s_self_tex)
        glGenTextures(1, &s_self_tex);
    glBindTexture(GL_TEXTURE_2D, s_self_tex);
    if (s_self_w != f->pw * (f->st ? 2 : 1) || s_self_h != f->ph) {
        s_self_w = f->pw * (f->st ? 2 : 1);
        s_self_h = f->ph;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s_self_w, s_self_h, 0, GL_BGRA,
                     GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    }
    clear_flush((Surf *)f, GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, f->fbo);
    gt_surface(GT_SELF);
    s_gt_nself++;
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, s_self_w, s_self_h);
    gt_surface(f->va);
    if (f->st && s_unit < 4)
        s_sbs[s_unit] = 1.0f;          /* both eyes: each reads its half */
    tex_sampling(s_self_tex, t, 0);
    *sw = (float)f->w;
    *sh = (float)f->h;
    return 1;
}

/* The fixed-function state last set by apply_state; anything else that
 * changes it (a clear, the debug probes) clears s_rs_valid. Runs of batches
 * share it, and the driver revalidates its pipeline on every call. */
static Nv2aRenderState s_rs_last;
static int s_rs_valid;

static void apply_state(const Nv2aRenderState *rs)
{
    static int cull_mode = -1;
    uint32_t m = rs->color_mask;

    if (cull_mode < 0) {
        const char *e = genv("GL_CULL");
        cull_mode = e ? atoi(e) : 0;
    }
    static const Prog *rs_prog;
    if (rs_prog != U)
        s_rs_valid = 0;                 /* its uniforms are per program */
    rs_prog = U;
    if (s_rs_valid && !memcmp(&s_rs_last, rs, sizeof *rs))
        goto uniforms;
    s_rs_last = *rs;
    s_rs_valid = 1;
    if (rs->blend_enable) {
        glEnable(GL_BLEND);
        if (s_fx.draw)          /* the effect target: alpha keeps the scene's weight */
            glBlendFuncSeparate(rs->blend_src, rs->blend_dst, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
        else
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
uniforms:
    cu1i(U->atest, rs->alpha_test_enable && !genv("GL_NOATEST") ? 1 : 0);
    cu1i(U->afunc, (GLint)rs->alpha_func);
    cu1f(U->aref, (float)(rs->alpha_ref & 0xFF));
    {
        float dmin = rs->depth_min, dmax = rs->depth_max;
        if (!(dmax > dmin)) {
            dmin = 0.0f;
            dmax = 16777215.0f;
        }
        cu2f(U->depth, dmin, dmax);
    }
}

/* ── Back end entry points ─────────────────────────────────── */

/* ── Deferred clears ────────────────────────────────────────
 *
 * A tiler starts a pass by loading the surface into tile memory, unless the
 * pass begins with a clear, and ends it by storing everything back, unless
 * told a buffer is dead. VC3 clears its back buffer, then draws the scene
 * into another surface, then comes back to composite: done as it comes, the
 * clear is a pass of its own that stores the whole cleared surface, and the
 * composite's pass loads it all back -- at native size twice the colour and
 * depth of a 2064x2208 pair of eyes for nothing. So a clear is only noted
 * (clear_defer) and done where the surface is next used: first thing in its
 * next drawing pass (gl_draw), or before its colour is read (sampled, copied,
 * shown, read back: clear_flush with the colour bit). A clear noted on top
 * of one still pending replaces what it covers. When the back end leaves a
 * surface with a clear pending for its depth and stencil (or its whole
 * colour), those are dead until then and the pass discards them instead of
 * storing (clear_leave; GLES). */
/* Do f's pending clear of the buffers in want (all: before drawing into
 * f), with f bound for it; the bindings are left as they were. */
static void clear_flush(Surf *f, GLbitfield want)
{
    GLbitfield bits = f->pc_bits & want;
    GLint rd = 0, dr = 0;
    if (!bits || !f->fbo)
        return;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rd);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dr);
    if ((GLuint)dr != f->fbo)
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f->fbo);
    glDisable(GL_SCISSOR_TEST);
    if (bits & GL_COLOR_BUFFER_BIT) {
        glColorMask(f->pc_mask[0], f->pc_mask[1], f->pc_mask[2], f->pc_mask[3]);
        glClearColor(f->pc_col[0], f->pc_col[1], f->pc_col[2], f->pc_col[3]);
    }
    if (bits & GL_DEPTH_BUFFER_BIT) {
        glDepthMask(GL_TRUE);
        glClearDepth((double)f->pc_depth);
    }
    if (bits & GL_STENCIL_BUFFER_BIT) {
        glStencilMask(0xFF);
        glClearStencil(f->pc_sten);
    }
    glClear(bits);
    f->pc_bits &= ~bits;
    s_rs_valid = 0;                     /* the masks changed */
    if ((GLuint)dr != f->fbo)
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)dr);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rd);
    GLCK("clear_flush", NULL, f->va);
}

static void clear_defer(Surf *f, GLbitfield bits, const GLboolean m[4], const GLfloat c[4],
                        GLfloat depth, GLint sten)
{
    if (bits & GL_COLOR_BUFFER_BIT) {
        /* a partial colour mask over a pending colour clear: that one first */
        if ((f->pc_bits & GL_COLOR_BUFFER_BIT) && !(m[0] && m[1] && m[2] && m[3]))
            clear_flush(f, GL_COLOR_BUFFER_BIT);
        memcpy(f->pc_mask, m, sizeof f->pc_mask);
        memcpy(f->pc_col, c, sizeof f->pc_col);
    }
    if (bits & GL_DEPTH_BUFFER_BIT)
        f->pc_depth = depth;
    if (bits & GL_STENCIL_BUFFER_BIT)
        f->pc_sten = sten;
    f->pc_bits |= bits;
}

/* The back end leaves p (another surface is bound next): what a pending
 * clear will overwrite is not stored. */
static void clear_leave(Surf *p)
{
#if defined(VC3_GLES)
    GLenum att[2];
    GLsizei n = 0;
    if (!p->fbo || !p->pc_bits)
        return;
    if ((p->pc_bits & GL_COLOR_BUFFER_BIT) && p->pc_mask[0] && p->pc_mask[1] && p->pc_mask[2]
        && p->pc_mask[3])
        att[n++] = GL_COLOR_ATTACHMENT0;
    if ((p->pc_bits & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT))
        == (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT))
        att[n++] = GL_DEPTH_STENCIL_ATTACHMENT;
    if (n) {
        glBindFramebuffer(GL_FRAMEBUFFER, p->fbo);
        glInvalidateFramebuffer(GL_FRAMEBUFFER, n, att);
    }
#else
    (void)p;
#endif
}

static void gl_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    Surf *f;
    GLbitfield bits = 0;

    (void)rs;
    if (!gl_ready())
        return;
    if (s_fx.on)
        fx_resolve();
    GLCK("before clear (elsewhere)", NULL, s->color_va);
    if (!(f = surf_bind(s)))
        return;
    if (lazy_skip(f, NULL))
        return;
    {
        const GLboolean m[4] = { (flags & 0x10) != 0, (flags & 0x20) != 0,
                                 (flags & 0x40) != 0, (flags & 0x80) != 0 };
        const GLfloat c[4] = { ((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f,
                               (argb & 0xFF) / 255.0f, (argb >> 24) / 255.0f };
        if (flags & 0xF0)
            bits |= GL_COLOR_BUFFER_BIT;
        if (flags & 0x1)
            bits |= GL_DEPTH_BUFFER_BIT;
        if (flags & 0x2)
            bits |= GL_STENCIL_BUFFER_BIT;
        if (s_ftr == 1)
            fprintf(stderr, "[GLTR] clear surf %08X flags %X colour %08X zstencil %08X "
                    "zeta %08X\n", f->va, flags, argb, zstencil, rs->zeta_va);
        if (bits)
            clear_defer(f, bits, m, c, (GLfloat)((double)(zstencil >> 8) / 16777215.0),
                        (GLint)(zstencil & 0xFF));
    }
    GLCK("clear: deferred", NULL, f->va);
    z_clear(f, bits);
    GLCK("clear: z_clear", NULL, f->va);
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
            active_tex(4 + st);
            if (tex_bind_cube(t)) {
                on[st] = cube[st] = 1;
                active_tex(st);
                glBindTexture(GL_TEXTURE_2D, s_white);
                continue;
            }
        }
        active_tex(st);
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
    active_tex(0);
    for (k = 0; k < 8; k++) {
        nv2a_rc_unpack(rc->factor0[k], &f0[k * 4]);
        nv2a_rc_unpack(rc->factor1[k], &f1[k * 4]);
    }
    cu1i(U->rc, 1);
    cu1uiv(U->cicw, 8, rc->color_icw);
    cu1uiv(U->aicw, 8, rc->alpha_icw);
    cu1uiv(U->cocw, 8, rc->color_ocw);
    cu1uiv(U->aocw, 8, rc->alpha_ocw);
    cu4fv(U->f0, 8, f0);
    cu4fv(U->f1, 8, f1);
    cu1ui(U->fin0, rc->final0);
    cu1ui(U->fin1, rc->final1);
    cu1ui(U->ctl, rc->control);
    cu1ui(U->sprog, rc->stage_program);
    nv2a_rc_unpack(rc->final_c0, c);
    cu4fv(U->fc0, 1, c);
    nv2a_rc_unpack(rc->final_c1, c);
    cu4fv(U->fc1, 1, c);
    /* FOG_COLOR is R in bits 0-7, the reverse of a D3DCOLOR. */
    nv2a_rc_unpack(b->fog_color, c);
    cu3f(U->fogc, c[2], c[1], c[0]);
    cu1iv(U->on, 4, on);
    cu1iv(U->iscube, 4, cube);
    cu1i(U->dbgon,
                !(b->stage[0] && surf_find(b->stage[0]->offset)));
    cu2fv(U->ts, 4, ts);
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
        if (!known && nseen < 64 && genv("GL_RCDBG")) {
            seen[nseen++] = h;
            fprintf(stderr, "[GL]   rc setup %02X%02X%02X: prog %05X fin %08X %08X "
                    "s0 %08X/%08X s1 %08X/%08X spec %08X tex %08X/%08X\n",
                    (h >> 16) & 0xFF, (h >> 8) & 0xFF, h & 0xFF, rc->stage_program,
                    rc->final0, rc->final1, rc->color_icw[0], rc->color_ocw[0],
                    rc->color_icw[1], rc->color_ocw[1], b->ext ? b->ext[0].specular : 0,
                    b->stage[0] ? b->stage[0]->offset : 0,
                    b->stage[1] ? b->stage[1]->offset : 0);
        }
        cu3f(U->dbgcol, r, g, bl);
        if (genv("GL_RCDBG") && atoi(genv("GL_RCDBG")) >= 6)
            glDisable(GL_BLEND);     /* exact colours, so a pixel names its setup */
            s_rs_valid = 0;
    }

}

/* The lightmap pattern of VC3's walls: stage 1 samples a 2D texture and the
 * stage's RGB is t1 * r0 (input A texture 1, B r0). */
static int lightmap_batch(const Nv2aBatch *b)
{
    uint32_t icw1;

    if (!b->rc || (!b->ext && !b->raw) || !b->stage[1] || (b->rc->control & 0xFF) < 2
        || ((b->rc->stage_program >> 5) & 0x1F) != 1)
        return 0;
    icw1 = b->rc->color_icw[1];
    return ((icw1 >> 24) & 0x1F) == 0x09 && ((icw1 >> 16) & 0x1F) == 0x0C;
}

/* The CPU path's combiner inputs (Nv2aVertexEx), on attributes 3-8. */
static void bind_ext(const Nv2aBatch *b)
{
    GLsizeiptr off = ring_put(&s_vring, b->ext, (GLsizeiptr)(b->count * sizeof(Nv2aVertexEx)));
    GLuint k;

    if (off < 0)
        return;
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Nv2aVertexEx),
                          (void *)(off + offsetof(Nv2aVertexEx, specular)));
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(Nv2aVertexEx),
                          (void *)(off + offsetof(Nv2aVertexEx, fog)));
    for (k = 0; k < 4; k++)
        glVertexAttribPointer(5 + k, 4, GL_FLOAT, GL_FALSE, sizeof(Nv2aVertexEx),
                              (void *)(off + offsetof(Nv2aVertexEx, tex)
                                       + k * 4 * sizeof(float)));
    for (k = 3; k <= 8; k++)
        glEnableVertexAttribArray(k);
}

/* ── Raw (GPU-transformed) geometry ────────────────────────── */

/* The fixed-function shader's inputs by role: position, weight, normal,
 * colour, texcoords 0-3 -> NV2A attribute slot. -1: not read, constant. */
static void raw_roles(const Nv2aRawGeometry *g, int slot[8])
{
    const Nv2aFixedState *ff = g->ff;

    slot[0] = 0;
    slot[1] = ff->skin_mode ? 1 : -1;
    slot[2] = ff->has_normal ? 2 : -1;
    slot[3] = ff->has_color ? (int)ff->color_slot : -1;
    slot[4] = (int)ff->tex0_slot;
    slot[5] = 10;
    slot[6] = 11;
    slot[7] = 12;
}

/* The vertex layout the attribute pointers were last set for, when the
 * attributes were one interleaved stream: a batch with the same layout puts
 * its vertices a whole number of strides past that stream's start in the
 * ring and is drawn with that as its base vertex, the pointers left alone --
 * eight glVertexAttribPointer calls a draw less, each one vertex-fetch state
 * the driver rebuilds. */
typedef struct {
    int valid;
    GLsizeiptr base;                    /* the stream's start in the ring */
    uint32_t stride;
    uint32_t sig[16];                   /* per role: type, size, offset; 0 off */
} VaCache;
static VaCache s_va, s_va_vp;           /* fixed function's VAO, programs' */
static GLint s_basevertex;              /* this raw draw's */

/* Upload each attribute stream min..max once, at its own stride, and point
 * the roles at it. Attributes interleaved in one vertex (offsets within a
 * stride of each other) share one copy. Returns 0 if they cannot fit. */
static int raw_attribs(const Nv2aRawGeometry *g, const int *slot, int nslot, VaCache *vac)
{
#define s_va (*vac)
    struct { const uint8_t *base; uint32_t stride, bytes; GLsizeiptr off; } grp[16];
    int ngrp = 0, gi[16], r, k;
    GLfloat scale[16][4];
    GLint packed = 0, bgra = 0;
    GLsizeiptr total = 0, off = 0;
    uint8_t *dst;

    for (r = 0; r < nslot; r++) {
        const Nv2aAttrib *a = slot[r] >= 0 ? &g->attr[slot[r]] : NULL;
        gi[r] = -1;
        if (!a || !a->data)
            continue;
        for (k = 0; k < ngrp; k++) {
            const uint8_t *p = (const uint8_t *)a->data;
            if (grp[k].stride != a->stride)
                continue;
            if (p >= grp[k].base && p < grp[k].base + a->stride) {
                uint32_t end = (uint32_t)(p - grp[k].base) + a->bytes;
                if (end > grp[k].bytes)
                    grp[k].bytes = end;
                break;
            }
            if (p < grp[k].base && grp[k].base < p + a->stride) {
                uint32_t shift = (uint32_t)(grp[k].base - p);
                grp[k].base = p;
                grp[k].bytes = grp[k].bytes + shift > a->bytes ? grp[k].bytes + shift
                                                               : a->bytes;
                break;
            }
        }
        if (k == ngrp) {
            grp[k].base = (const uint8_t *)a->data;
            grp[k].stride = a->stride;
            grp[k].bytes = a->bytes;
            ngrp++;
        }
        gi[r] = k;
    }
    for (k = 0; k < ngrp; k++) {
        grp[k].off = total;
        total += ((GLsizeiptr)grp[k].bytes + 15) & ~(GLsizeiptr)15;
    }
    /* The attribute uniforms (their layout's): set for this program on
     * either path below. They are the program's own -- when the layout
     * was kept but the program changed (the plain and combiner variants,
     * another vertex program), the kept path used to leave the new one's
     * at its defaults: u_ascale 0, every attribute 0, nothing drawn, no GL
     * error. VC3's Mission Select and attract screens went black so. */
    for (r = 0; r < nslot; r++) {
        const Nv2aAttrib *a = slot[r] >= 0 ? &g->attr[slot[r]] : NULL;
        int c;
        for (c = 0; c < 4; c++)
            scale[r][c] = 1.0f;
        if (gi[r] < 0)
            continue;
        switch (a->type) {
#if defined(VC3_GLES)
        case 0: bgra |= 1 << r; break;
#endif
        case 1:
            for (c = 0; c < (int)a->size; c++)
                scale[r][c] = 1.0f / 32767.0f;
            break;
        case 6: packed |= 1 << r; break;
        default: break;
        }
    }
    if (nslot > 8) {                        /* past the uniform cache's size */
        if (!U->ascale_valid || memcmp(U->ascale_last, scale, (size_t)nslot * 16)) {
            glUniform4fv(U->ascale, nslot, &scale[0][0]);
            memcpy(U->ascale_last, scale, (size_t)nslot * 16);
            U->ascale_valid = 1;
        }
    } else {
        cu4fv(U->ascale, nslot, &scale[0][0]);
    }
    cu1i(U->packed, packed);
    cu1i(U->bgra, bgra);
    s_basevertex = 0;
    if (ngrp == 1 && grp[0].stride) {
        /* one stream: the same layout as the pointers? */
        uint32_t sig[16];
        int same;
        static int keep = -1;
        if (keep < 0)
            keep = !(genv("GL_VAO_KEEP") && genv("GL_VAO_KEEP")[0] == '0');
        memset(sig, 0, sizeof sig);
        for (r = 0; r < nslot; r++) {
            const Nv2aAttrib *a = slot[r] >= 0 ? &g->attr[slot[r]] : NULL;
            sig[r] = gi[r] < 0 ? 0u
                   : 0x80000000u | (a->type & 15u) | ((a->size & 15u) << 4)
                     | ((uint32_t)((const uint8_t *)a->data - grp[0].base) << 8);
        }
        same = keep && s_va.valid && s_va.stride == grp[0].stride && !g->check_out
            && !memcmp(sig, s_va.sig, sizeof sig);
        if (same) {
            GLsizeiptr off0, at;
            uint8_t *p = (uint8_t *)ring_map(&s_vring, total + grp[0].stride, &off0);
            if (!p)
                return 0;
            at = off0;
            if (at >= s_va.base) {
                GLsizeiptr d = (at - s_va.base) % (GLsizeiptr)grp[0].stride;
                if (d)
                    at += (GLsizeiptr)grp[0].stride - d;
                memcpy(p + (at - off0), grp[0].base, grp[0].bytes);
                ring_unmap(&s_vring);
                s_basevertex = (GLint)((at - s_va.base) / (GLsizeiptr)grp[0].stride);
                s_va_hit++;
                for (r = 0; r < nslot; r++)
                    if (gi[r] < 0) {
                        const Nv2aAttrib *a = slot[r] >= 0 ? &g->attr[slot[r]] : NULL;
                        static const float none[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                        glVertexAttrib4fv((GLuint)r, a ? a->value : none);
                    }
                return 1;
            }
            ring_unmap(&s_vring);           /* the ring went round: set again */
        }
        s_va_miss++;
        memcpy(s_va.sig, sig, sizeof sig);
        s_va.stride = grp[0].stride;
        s_va.valid = 0;                     /* set below, once the pointers are */
    } else {
        s_va.valid = 0;
    }
    if (total) {
        if (!(dst = (uint8_t *)ring_map(&s_vring, total, &off)))
            return 0;
        for (k = 0; k < ngrp; k++) {
            memcpy(dst + grp[k].off, grp[k].base, grp[k].bytes);
            grp[k].off += off;
        }
        ring_unmap(&s_vring);
    }
    for (r = 0; r < nslot; r++) {
        const Nv2aAttrib *a = slot[r] >= 0 ? &g->attr[slot[r]] : NULL;
        GLuint loc = (GLuint)r;

        if (gi[r] < 0) {
            static const float none[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            glDisableVertexAttribArray(loc);
            glVertexAttrib4fv(loc, a ? a->value : none);
            continue;
        }
        {
            GLsizei stride = (GLsizei)a->stride;
            const void *at = (const void *)(grp[gi[r]].off
                             + ((const uint8_t *)a->data - grp[gi[r]].base));
            GLint n = (GLint)a->size;
            switch (a->type) {
#if defined(VC3_GLES)
            case 0:
                glVertexAttribPointer(loc, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, at);
                break;
#else
            case 0:  glVertexAttribPointer(loc, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, stride, at); break;
#endif
            case 4:  glVertexAttribPointer(loc, n, GL_UNSIGNED_BYTE, GL_TRUE, stride, at); break;
            case 1:
            case 5:  glVertexAttribPointer(loc, n, GL_SHORT, GL_FALSE, stride, at); break;
            case 6:
                glVertexAttribPointer(loc, 2, GL_UNSIGNED_SHORT, GL_FALSE, stride, at);
                break;
            default: glVertexAttribPointer(loc, n, GL_FLOAT, GL_FALSE, stride, at); break;
            }
            glEnableVertexAttribArray(loc);
        }
    }
    if (ngrp == 1 && grp[0].stride && total) {
        s_va.base = grp[0].off;             /* the pointers' stream */
        s_va.valid = 1;
    }
    return 1;
#undef s_va
}

/* The fixed-function state, re-sent only when it changed: consecutive
 * batches of one mesh share nearly all of it. */
/* What every raw draw's shader reads, fixed function or program. */
static void raw_uniforms_common(const Nv2aRawGeometry *g)
{
    cu2f(U->aa, g->aa_sx, g->aa_sy);
    cu1f(U->vpz, g->vp_off_z);
    cu1i(U->nearz, (GLint)g->near_z);
    cu1i(U->fogen, (GLint)g->fog_enable);
    cu1ui(U->fogmode, g->fog_mode);
    cu2f(U->fogp, g->fog_param[0], g->fog_param[1]);
    cu2f(U->uvs0, g->uv_scale0[0], g->uv_scale0[1]);
}

static void raw_uniforms(const Nv2aRawGeometry *g)
{
    Nv2aFixedState *lastp = &U->ff_last;
#define last (*lastp)
#define valid (U->ff_valid)
    const Nv2aFixedState *ff = g->ff;
    GLint lk[8], tg[4], tm[4], i;
    GLuint gen[16];

    raw_uniforms_common(g);
    {   /* the game's projection for this batch, when its matrices changed */
        static float last_aa[2];
        if (!(valid && !memcmp(&last, ff, sizeof last)) || last_aa[0] != g->aa_sx
            || last_aa[1] != g->aa_sy) {
            last_aa[0] = g->aa_sx;
            last_aa[1] = g->aa_sy;
            s_bgp_ok = vr_game_proj_from(ff->composite, ff->mv[0], (int)ff->use_composite,
                                         ff->proj, g->aa_sx, g->aa_sy, ff->c59, &s_bgp);
            if (s_bgp_ok) {
                s_gp_good++;
            } else {
                s_gp_bad++;
            }
        }
    }
    if (valid && !memcmp(&last, ff, sizeof last))
        return;
    last = *ff;
    valid = 1;
    cuMatrix4fv(U->comp, 1, GL_TRUE, ff->composite);
    cuMatrix4fv(U->mv, 4, GL_TRUE, &ff->mv[0][0]);
    cuMatrix4fv(U->proj, 1, GL_TRUE, ff->proj);
    cuMatrix4fv(U->tm, 4, GL_TRUE, &ff->texmat[0][0]);
    cu4fv(U->c59, 1, ff->c59);
    cu1ui(U->skin, ff->skin_mode);
    cu1i(U->usecomp, (GLint)ff->use_composite);
    cu1i(U->lighting, (GLint)ff->lighting);
    cu2i(U->colmat, (GLint)ff->colmat_amb, (GLint)ff->colmat_dif);
    cu1i(U->flipn, (GLint)ff->flip_normals);
    cu1i(U->hasnrm, (GLint)ff->has_normal);
    cu1i(U->hascol, (GLint)ff->has_color);
    cu4fv(U->samb, 1, ff->scene_amb);
    cu1f(U->matalpha, ff->mat_alpha);
    for (i = 0; i < 8; i++)
        lk[i] = (GLint)ff->light_kind[i];
    cu1iv(U->lkind, 8, lk);
    cu4fv(U->light, 40, &ff->light[0][0]);
    for (i = 0; i < 4; i++) {
        tg[i] = (GLint)ff->texgen_on[i];
        tm[i] = (GLint)ff->texmat_on[i];
    }
    memcpy(gen, ff->texgen, sizeof gen);
    cu4uiv(U->texgen, 4, gen);
    cu1iv(U->tgon, 4, tg);
    cu1iv(U->tmon, 4, tm);
#undef last
#undef valid
}

static GLenum raw_mode(uint32_t prim)
{
    return prim == 6 ? GL_TRIANGLE_STRIP : prim == 7 ? GL_TRIANGLE_FAN : GL_TRIANGLES;
}

/* RECOMP_GPU_XF_CHECK: run the vertices min..max through the same shader
 * into a transform-feedback buffer and read them back for the executor. */
static void raw_capture(const Nv2aRawGeometry *g)
{
    static GLuint tfb;
    GLsizei n = (GLsizei)(g->max_index - g->min_index + 1);
    GLsizeiptr bytes = (GLsizeiptr)n * NV2A_RAW_CHECK_FLOATS * sizeof(float);
    void *p;

    if (!tfb)
        glGenBuffers(1, &tfb);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, tfb);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, bytes, NULL, GL_STREAM_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, tfb);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, n);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    p = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, bytes, GL_MAP_READ_BIT);
    if (p) {
        memcpy(g->check_out, p, (size_t)bytes);
        glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
    } else {
        memset(g->check_out, 0, (size_t)bytes);
    }
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
}

/* Geometry for a raw batch, once the shared state is bound. */
static int raw_submit(const Nv2aRawGeometry *g, GLsizeiptr *ioff)
{
    int slot[16], r;

    if (g->kind == NV2A_RAW_PROGRAM) {
        for (r = 0; r < 16; r++)           /* the inputs the program reads */
            slot[r] = (s_vp_cur->inputs >> r) & 1 ? r : -1;
        if (!raw_attribs(g, slot, 16, &s_va_vp))
            return 0;
        raw_uniforms_common(g);
        if (s_vc_gen_bound != g->const_gen || !s_cring.buf) {   /* the constants, when changed */
            GLsizeiptr off = ring_put(&s_cring, g->constants, VC_BYTES);
            if (off < 0)
                return 0;
            glBindBufferRange(GL_UNIFORM_BUFFER, VC_BINDING, s_cring.buf, off, VC_BYTES);
            s_vc_gen_bound = g->const_gen;
            s_vc_sent++;
        }
    } else {
        raw_roles(g, slot);
        if (!raw_attribs(g, slot, 8, &s_va))
            return 0;
        raw_uniforms(g);
    }
    *ioff = ring_put(&s_iring, g->indices, (GLsizeiptr)g->index_count * 2);
    return *ioff >= 0;
}

/* VC3_GL_STATS: raw draws and why some were declined. */

/* VC3_GL_TIMING=1: wall time spent in gl_draw's stages, per second -- to
 * tell CPU work from a driver wait. TM(k) closes stage k-1 at mark k. */
static int s_tm_on = -1;
static uint64_t s_tm_ns[8], s_tm_last, s_tm_draws;
static uint64_t tm_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#define TM(k) do { if (s_tm_on > 0) { uint64_t n_ = tm_now(); \
    if ((k) > 0) s_tm_ns[(k) - 1] += n_ - s_tm_last; s_tm_last = n_; } } while (0)

/* A surface the game has shown lately (a back buffer): where the HUD is. */
static int surf_shown(const Surf *f)
{
    int k;
    for (k = 0; k < s_nshown; k++)
        if (f->va && f->va == s_shown_va[k])
            return 1;
    return 0;
}

/* ── Lazy copies ──────────────────────────────────────────────────────────
   VC3 copies its scene into another full-size stereo target twice every
   frame (80C28000) that nothing samples outside E.S. mode -- 2 ms of the
   Quest's GPU a frame at 1x, four times that at 2x. A target that is never
   shown, gets only full-screen 2D copies of other render targets, and has
   not been sampled for LAZY_FRAMES frames has its copies (and clears)
   skipped; the moment something samples it again it is first brought up
   to date with a blit from the copies' source, and drawn from then on.
   VC3_LAZY_RT=0: copies always drawn. */
#define LAZY_FRAMES 60

static int lazy_on(void)
{
    static int on = -1;
    if (on < 0)     /* the profile allows them (gpu.lazy_rt), VC3_LAZY_RT=0 not */
        on = s_lazy_rt && !(genv("LAZY_RT") && genv("LAZY_RT")[0] == '0');
    return on;
}

/* Called by tex_bind when a render target is about to be sampled. */
static void lazy_catch_up(Surf *rt)
{
    Surf *src;
    rt->last_sampled = s_frame;
    if (!rt->skipping)
        return;
    rt->skipping = 0;
    src = rt->copy_src ? surf_find(rt->copy_src) : NULL;
    if (genv("IMM_LOG"))
        fprintf(stderr, "[GL] lazy: %08X sampled again, brought up to date from %08X\n",
                rt->va, src ? src->va : 0);
    if (src && src->fbo && src != rt) {
        GLboolean mask[4], sc;
        clear_flush(src, GL_COLOR_BUFFER_BIT);
        rt->pc_bits &= ~(GLbitfield)GL_COLOR_BUFFER_BIT;
        sc = glIsEnabled(GL_SCISSOR_TEST);
        glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(1, 1, 1, 1);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, src->fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rt->fbo);
        gt_surface(GT_LAZY);
        s_gt_nlazy++;
        glBlitFramebuffer(0, 0, src->pw * (src->st ? 2 : 1), src->ph,
                          0, 0, rt->pw * (rt->st ? 2 : 1), rt->ph, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        gt_surface(s_bound_va);
        glBindFramebuffer(GL_FRAMEBUFFER, s_bound_fbo);
        glColorMask(mask[0], mask[1], mask[2], mask[3]);
        if (sc)
            glEnable(GL_SCISSOR_TEST);
        GLCK("lazy_catch_up blit", NULL, rt->va);
    }
}

/* A program batch the back end transforms that used to stay on the CPU --
 * screen space (first vertex at w = 1), or a first vertex behind the eye
 * (w <= 0: VC3's E.S. distortion meshes) -- whose vertices the executor can
 * still place for us (Nv2aRawGeometry.extent). The choices below that look
 * at such a batch's vertices treat it as the CPU path's. */
static int raw_2d(const Nv2aRawGeometry *g)
{
    return g && g->kind == NV2A_RAW_PROGRAM && g->extent && !(g->w0 > 0.0f && g->w0 != 1.0f);
}

/* The CPU path's 3D test, rhw of its first vertex != 1 and > 0, for a raw
 * program batch: that vertex is the first one left after clipping at
 * w = 1e-3, so a batch whose first vertex is behind the eye starts with a
 * clip point (rhw 1000) and is 3D as well. */
static int raw_vp_3d(const Nv2aRawGeometry *g)
{
    return g->w0 != 1.0f && g->w0 == g->w0;
}

/* The batch's vertices on the surface and in stage-0 texels; 0 when not
 * known (a 3D raw batch, or one the executor could not place). */
static int batch_extent(const Nv2aBatch *b, Nv2aRawExtent *e)
{
    uint32_t k;
    if (b->raw)
        return raw_2d(b->raw) && b->raw->extent(b->raw, e);
    if (!b->count)
        return 0;
    e->x0 = e->y0 = e->u0 = e->v0 = 1e30f;
    e->x1 = e->y1 = e->u1 = e->v1 = -1e30f;
    for (k = 0; k < b->count; k++) {
        const Nv2aVertex *q = &b->vertices[k];
        if (q->x < e->x0) e->x0 = q->x;
        if (q->x > e->x1) e->x1 = q->x;
        if (q->y < e->y0) e->y0 = q->y;
        if (q->y > e->y1) e->y1 = q->y;
        if (q->u < e->u0) e->u0 = q->u;
        if (q->u > e->u1) e->u1 = q->u;
        if (q->v < e->v0) e->v0 = q->v;
        if (q->v > e->v1) e->v1 = q->v;
    }
    return 1;
}

/* A 2D batch (rhw 1, as the CPU path has it), and its first vertex's diffuse. */
static int batch_flat(const Nv2aBatch *b)
{
    return b->raw ? raw_2d(b->raw) && b->raw->w0 == 1.0f
                  : b->count && b->vertices[0].rhw == 1.0f;
}

/* The first vertex's diffuse, as the CPU path's vertices[0].diffuse: known
 * for every program batch, whatever its first w (the executor transforms
 * that vertex for each one). It decides the combiner routing of "black
 * diffuse over a render target" (rt_pass); asked only of screen-space
 * batches, it left VC3's E.S. distortion meshes -- 3D when their first
 * vertex happens to be in front of the eye -- drawn as texture * black
 * diffuse, forty alpha-blended layers that turned the scene black. */
static int batch_d0(const Nv2aBatch *b, uint32_t *d0)
{
    if (b->raw) {
        *d0 = b->raw->d0;
        return b->raw->kind == NV2A_RAW_PROGRAM;
    }
    *d0 = b->count ? b->vertices[0].diffuse : 0;
    return b->count != 0;
}

/* Whether a draw into f (b: the batch, NULL for a clear) is skipped. */
static int lazy_skip(Surf *f, const Nv2aBatch *b)
{
    int copy;
    if (!f->first_drawn)
        f->first_drawn = s_frame;
    /* (not_copy never clears: once set, what this batch is no longer matters) */
    copy = !b || (!f->not_copy && batch_flat(b) && b->texture
                  && surf_find(b->texture->offset) && surf_find(b->texture->offset)->fbo
                  && surf_find(b->texture->offset) != f && batch_fullscreen(b, f));
    if (!copy)
        f->not_copy = 1;
    if (!lazy_on() || !f->st || f->not_copy || surf_shown(f) || f->cpu_owned
        || s_frame - f->first_drawn < LAZY_FRAMES || s_frame - f->last_sampled < LAZY_FRAMES)
        return 0;
    if (b)
        f->copy_src = b->texture->offset;
    if (!f->skipping) {
        f->skipping = 1;
        if (genv("IMM_LOG"))
            fprintf(stderr, "[GL] lazy: copies into %08X skipped (unsampled %u frames)\n",
                    f->va, s_frame - f->last_sampled);
    }
    return 1;
}

/* 2D covering (nearly) the whole surface: a fade or a pass over the scene. */
static int batch_fullscreen(const Nv2aBatch *b, const Surf *f)
{
    Nv2aRawExtent e;
    if (!batch_extent(b, &e))
        return 0;
    return e.x1 - e.x0 >= 0.9f * (float)f->w && e.y1 - e.y0 >= 0.9f * (float)f->h;
}

/* The HUD picture, bound for a draw: an eye's size, transparent at the
 * frame's first HUD draw. Drawn with alpha kept as coverage (premultiplied
 * over), for the headset's blended layer. */
static void hud_bind(const Surf *f, const Nv2aRenderState *rs)
{
    /* the game's own size (widened) times s_hud_sc: 1 at a fixed detail (a
     * flat panel at 5 m, a quarter of the pixels to fill at 2x), at native
     * as many as the panel covers on the display (scale_pick) */
    GLsizei hw = (GLsizei)((float)(f->wide ? (f->w * 4 / 3 + 1) & ~1 : f->w) * s_hud_sc + 0.5f);
    GLsizei hh = (GLsizei)((float)f->h * s_hud_sc + 0.5f);
    if (!s_hud_fbo || s_hud_w != hw || s_hud_h != hh) {
        GLint was = 0;
        if (!s_hud_fbo) {
            glGenFramebuffers(1, &s_hud_fbo);
            glGenTextures(1, &s_hud_tex);
        }
        s_hud_w = hw;
        s_hud_h = hh;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &was);
        glBindTexture(GL_TEXTURE_2D, s_hud_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s_hud_w, s_hud_h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, (GLuint)was);
        glBindFramebuffer(GL_FRAMEBUFFER, s_hud_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_hud_tex, 0);
        s_hud_frame = 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_hud_fbo);
    glViewport(0, 0, s_hud_w, s_hud_h);
    if (s_hud_frame != s_frame) {
        GLboolean mask[4], sc = glIsEnabled(GL_SCISSOR_TEST);
        glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(1, 1, 1, 1);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glColorMask(mask[0], mask[1], mask[2], mask[3]);
        if (sc)
            glEnable(GL_SCISSOR_TEST);
        s_hud_frame = s_frame;
    }
    if (rs->blend_enable)
        glBlendFuncSeparate(rs->blend_src, rs->blend_dst, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

/* Whether a batch is drawn through the register combiners (below, at
 * has_cube / rt_pass): decided first, for the program. */
static int batch_needs_rc(const Surf *f, const Nv2aBatch *b, int raw)
{
    const int has_ext = raw ? b->rc != NULL : (b->rc && b->ext);
    uint32_t d0;
    int st;
    if (!has_ext)
        return 0;
    if (s_use_rc)
        return 1;
    if (genv("GL_CUBE_RC"))
        for (st = 0; st < 4; st++)
            if (b->stage[st] && b->stage[st]->cube
                && ((b->rc->stage_program >> (st * 5)) & 0x1F) == 3)
                return 1;
    if (b->texture && batch_d0(b, &d0) && (d0 & 0x00FFFFFFu) == 0) {
        const Surf *rt = surf_find(b->texture->offset);
        if (rt && rt->fbo && !rt->cpu_owned)
            return 1;
    }
    if (genv("GL_RC_SURF")
        && f->va == (uint32_t)strtoul(genv("GL_RC_SURF"), NULL, 16))
        return 1;
    return 0;
}

static void gl_draw_inner(const Nv2aSurface *s, const Nv2aBatch *b);

static void gl_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    if (!gl_ready())
        return;
    fx_pre(s, b);
    gl_draw_inner(s, b);
    fx_post();
}

static void gl_draw_inner(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aRawGeometry *raw = b->raw;
    Surf *f;
    float tw = 1.0f, th = 1.0f;
    int textured = 0;
    uint32_t tris = raw ? (raw->prim == 5 ? raw->index_count / 3 : raw->index_count - 2)
                        : b->count / 3;
    GLsizeiptr ioff = 0;

    if ((!raw && !b->count) || !gl_ready())
        return;
    GLCK("before draw (elsewhere)", b, s->color_va);
    if (!(f = surf_bind(s)))
        return;
    GLCK("draw: surf_bind", b, f->va);
    if (lazy_skip(f, b))
        return;
    {
        /* the program: the combiners or not, the alpha test or not */
        const int rc = batch_needs_rc(f, b, raw != NULL);
        const int at = b->state->alpha_test_enable && !genv("GL_NOATEST");
        Prog *a, *na;
        if (raw && raw->kind == NV2A_RAW_PROGRAM) {
            a = rc ? &s_vp_cur->p_rc : &s_vp_cur->p;
            na = rc ? &s_vp_cur->p_rc_na : &s_vp_cur->p_na;
        } else if (raw) {
            a = rc ? &s_praw_rc : &s_praw;
            na = rc ? &s_praw_rc_na : &s_praw_na;
        } else {
            a = rc ? &s_pcpu_rc : &s_pcpu;
            na = rc ? &s_pcpu_rc_na : &s_pcpu_na;
        }
        U = !at && na->prog && !genv("GL_ATEST_ALL") ? na : a;
    }
    memset(s_sbs, 0, sizeof s_sbs);
    {
        /* Consecutive batches mostly share program and layout. */
        GLuint vao = !raw ? s_vao : raw->kind == NV2A_RAW_PROGRAM ? s_vao_vp : s_vao_raw;
        if (s_cur_prog != U->prog) {
            glUseProgram(U->prog);
            s_cur_prog = U->prog;
        }
        if (s_cur_vao != vao) {
            glBindVertexArray(vao);
            s_cur_vao = vao;
        }
    }
    GLCK("draw: program / VAO", b, f->va);
    TM(0);
    apply_state(b->state);
    GLCK("draw: apply_state", b, f->va);
    z_use(f, b->state);
    GLCK("draw: z_use", b, f->va);
    TM(1);
    cu2f(U->surf, (float)f->w, (float)f->h);
    {
        float xs, xo;
        surf_xform(f, b, &xs, &xo);
        cu2f(U->xs, xs, xo);
        if (s_ftr == 1 && f->wide)
            fprintf(stderr, "[GLTR]   16:9 x scale %.3f offset %+.3f\n", xs, xo);
    }
    GLCK("draw: surface uniforms", b, f->va);
    active_tex(0);
    if (b->texture)
        textured = tex_bind_for(f, b->texture, &tw, &th);
    GLCK("draw: stage-0 texture", b, f->va);
    TM(2);
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
    if (!raw && s_st.on && textured && surf_find(b->texture->offset) && s_st.rt_logged < 6) {
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
    if (s_ftr == 1 && raw) {
        fprintf(stderr, "[GLTR] RAW surf %08X idx %u verts %u..%u prim %u tex %08X s1 %08X"
                " blend %u %X/%X z %u/%u sprog %05X fin %08X\n", f->va, raw->index_count,
                raw->min_index, raw->max_index, raw->prim,
                b->texture ? b->texture->offset : 0, b->stage[1] ? b->stage[1]->offset : 0,
                b->state->blend_enable, b->state->blend_src, b->state->blend_dst,
                b->state->depth_test_enable, b->state->depth_write,
                b->rc ? b->rc->stage_program : 0, b->rc ? b->rc->final0 : 0);
    } else if (s_ftr == 1) {
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
    s_st.tris += tris;
    if (textured)
        s_st.textured++;
    else if (!b->texture)
        s_st.untex++;
    if (!textured)
        glBindTexture(GL_TEXTURE_2D, s_white);
    cu1i(U->textured, textured);
    cu2f(U->texsize, tw, th);
    GLCK("draw: texture uniforms", b, f->va);
    if (!raw && b->rc && b->ext && s_use_rc && s_st.on && s_st.rc_logged < 4) {
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
    if (!raw && genv("GL_RCCHECK"))
        rc_check(b);
    /* Combiner inputs come from Nv2aVertexEx on the CPU path, from the
     * shader's own varyings on the raw one. */
    {
        const int has_ext = raw ? b->rc != NULL : (b->rc && b->ext);
        /* A batch with a cube-map stage can only be drawn faithfully by the
         * combiners: the default texture * diffuse path has no direction to
         * look a cube up by. Opt-in (VC3_GL_CUBE_RC=1) while the combiner path
         * is not yet right: routed by default, VC3's monitor-room walls went
         * flat grey. */
        int has_cube = 0, need_ext = 0, rt_pass = 0;
        if (has_ext && genv("GL_CUBE_RC")) {
            int st;
            for (st = 0; st < 4; st++)
                if (b->stage[st] && b->stage[st]->cube
                    && ((b->rc->stage_program >> (st * 5)) & 0x1F) == 3)
                    has_cube = 1;
        }
        /* A screen pass that samples a render target with a black diffuse:
         * texture * diffuse can only draw black, so the combiners must be
         * what makes its colour. VC3's E.S. mode (the pedal's slow motion)
         * redraws the 3D view through ~40 such distortion meshes; drawn the
         * default way, the whole scene went black for as long as the pedal
         * was held. */
        uint32_t d0;
        if (has_ext && b->texture && batch_d0(b, &d0) && (d0 & 0x00FFFFFFu) == 0) {
            const Surf *rt = surf_find(b->texture->offset);
            rt_pass = rt && rt->fbo && !rt->cpu_owned;
        }
        /* VC3_GL_RC_SURF=<hex va>: every batch drawn into that surface
         * through the combiners (comparing one scene both ways). */
        if (has_ext && genv("GL_RC_SURF")
            && f->va == (uint32_t)strtoul(genv("GL_RC_SURF"), NULL, 16))
            rt_pass = 1;
        if (has_ext && (s_use_rc || has_cube || rt_pass)) {
            bind_combiners(f, b);
            GLCK("draw: bind_combiners", b, f->va);
            need_ext = 1;
        } else {
            int lmap = has_ext && lightmap_batch(b);
            cu1i(U->rc, 0);
            cu1i(U->lmap, 0);
            if (lmap && textured) {
                /* VC3's walls: stage 1 multiplies by a lightmap (t1 * r0).
                 * Drawn as texture * diffuse * lightmap, keeping the texture
                 * * diffuse alpha its alpha test needs. */
                float sw = 1.0f, sh = 1.0f;
                const Nv2aTexture *t = b->stage[1];
                active_tex(1);
                if (tex_bind_for(f, t, &sw, &sh)) {
                    int norm = d3d8_format_is_swizzled(t->color)
                            || d3d8_format_dxt_block_bytes(t->color);
                    GLfloat ts[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
                    ts[2] = (norm ? (float)t->width : 1.0f) / sw;
                    ts[3] = (norm ? (float)t->height : 1.0f) / sh;
                    cu2fv(U->ts, 4, ts);
                    cu1ui(U->sprog, b->rc->stage_program);
                    cu1i(U->lmap, 1);
                    need_ext = 1;
                }
                active_tex(0);
            }
        }
        GLCK("draw: lightmap / combiners off", b, f->va);
        if (raw) {
            TM(3);
            if (!raw_submit(raw, &ioff)) {
                s_rst.declined++;
                return;
            }
            GLCK(raw->kind == NV2A_RAW_PROGRAM ? "draw: raw vp attributes + uniforms"
                 : "draw: raw ff attributes + uniforms", b, f->va);
        } else {
            GLuint k;
            GLsizeiptr off;
            if (need_ext)
                bind_ext(b);
            else
                for (k = 3; k <= 8; k++)
                    glDisableVertexAttribArray(k);
            TM(3);
            off = ring_put(&s_vring, b->vertices, (GLsizeiptr)(b->count * sizeof(Nv2aVertex)));
            if (off < 0)
                return;
            glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Nv2aVertex),
                                  (void *)(off + offsetof(Nv2aVertex, x)));
            glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Nv2aVertex),
                                  (void *)(off + offsetof(Nv2aVertex, diffuse)));
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Nv2aVertex),
                                  (void *)(off + offsetof(Nv2aVertex, u)));
            GLCK("draw: cpu vertices + attributes", b, f->va);
        }
    }
    if (raw) {
        if (raw->check_out)
            raw_capture(raw);
        set_cap(0, 1);
        set_cap(1, 1);
        set_cap(2, 1);
        set_cap(3, 1);
        s_rst.draws++;
        s_rst.tris += tris;
    }
    if (!raw) {
        set_cap(0, 1);
        set_cap(1, 0);
        set_cap(2, 0);
        set_cap(3, 0);
    }
    if (b->zpass_out && f->pc_bits) {
        clear_flush(f, ~(GLbitfield)0);
        apply_state(b->state);
    }
    if (b->zpass_out) {
        GLfloat sb0[8] = { 0 };
        cu1i(U->st2, 0);
        cu4fv(U->sbs, 2, sb0);
        cu2f(U->eye, 0.0f, s_st_depth);
        /* A visibility test: count the pixels that pass, for GET_REPORT.
         * Reading the result waits for the GPU, but only test batches pay. */
        static GLuint q;
        GLuint passed = 0;
        if (!q)
            glGenQueries(1, &q);
        glBeginQuery(GL_SAMPLES_PASSED, q);
        if (raw)
            glDrawElementsBaseVertex(raw_mode(raw->prim), (GLsizei)raw->index_count,
                                     GL_UNSIGNED_SHORT, (void *)ioff, s_basevertex);
        else
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)b->count);
        glEndQuery(GL_SAMPLES_PASSED);
        GLCK("draw: zpass draw", b, f->va);
        glGetQueryObjectuiv(q, GL_QUERY_RESULT, &passed);
        *b->zpass_out += passed;
        s_st.zpass_batches++;
        s_zp_frame++;
        s_st.zpass_pixels += passed;
    } else {
        TM(4);
        /* Immersive: 3D through each eye's matrix; 2D off the full screen to
         * the HUD picture. */
        int is3d, imm3d = 0, hud = 0;
        const int rawff = raw && raw->kind != NV2A_RAW_PROGRAM;
        if (rawff && s_bgp_ok) {
            f->gp = s_bgp;
            f->gp_ok = 1;
        }
        /* a program batch: what the CPU path decides from its first vertex */
        is3d = rawff ? s_bgp_ok
             : raw ? (raw_vp_3d(raw) && (f->gp_ok || s_gp_ok))
                   : (b->vertices[0].rhw != 1.0f && b->vertices[0].rhw > 0.0f
                      && (f->gp_ok || s_gp_ok));
        if (is3d && f->st) {
            s_3d_this++;
            f->n3d++;
        }
        if (is3d && genv("IMM_LOG")) {   /* where the 3D goes (debug) */
            static uint32_t vas[8], cnt[8], fr;
            int k;
            if (fr != s_frame) {
                if (s_frame % 120 == 0)
                    for (k = 0; k < 8 && vas[k]; k++)
                        fprintf(stderr, "[IMM]   3D into %08X %dx%d st %d: %u batches (shown %08X %08X)\n",
                                vas[k], surf_find(vas[k]) ? surf_find(vas[k])->w : 0,
                                surf_find(vas[k]) ? surf_find(vas[k])->h : 0,
                                surf_find(vas[k]) ? surf_find(vas[k])->st : 0, cnt[k],
                                s_shown_va[0], s_shown_va[1]);
                memset(vas, 0, sizeof vas);
                memset(cnt, 0, sizeof cnt);
                fr = s_frame;
            }
            for (k = 0; k < 8 && vas[k] && vas[k] != f->va; k++)
                ;
            if (k < 8) {
                vas[k] = f->va;
                cnt[k]++;
            }
        }
        if (s_imm && f->st) {
            if (is3d)
                imm3d = 1;
            else if ((!raw || raw_2d(raw)) && surf_shown(f) && !batch_fullscreen(b, f))
                hud = 1;
        }
        if (s_imm && !is3d && genv("IMM_LOG") && s_frame % 120 == 0) {
            static uint32_t lf, nl;
            if (lf != s_frame) { lf = s_frame; nl = 0; }
            if (nl++ < 40)
                fprintf(stderr, "[IMM]   2D %s into %08X %dx%d st %d shown %d full %d count %u rhw %.3f tex %08X\n",
                        raw ? "raw" : "cpu", f->va, f->w, f->h, f->st, surf_shown(f),
                        batch_fullscreen(b, f), raw ? raw->index_count : b->count,
                        raw ? (raw->w0 != 0.0f ? 1.0f / raw->w0 : 0.0f) : b->vertices[0].rhw,
                        b->texture ? b->texture->offset : 0);
        }
        if (imm3d) {
            const VrGameProj *g = rawff ? &s_bgp : f->gp_ok ? &f->gp : &s_gp;
            VrGameProj zg = zoom_proj(g);
            cu4f(U->gp, zg.kx, zg.ky, zg.cx, zg.cy);
            cu2f(U->gw, 1.0f, g->inv_pz);
        } else {
            cu2f(U->gw, 0.0f, 0.0f);
        }
        GLCK("draw: eye / immersive uniforms", b, f->va);
        if (hud) {
            hud_bind(f, b->state);
            s_rs_valid = 0;                 /* the blend's alpha was changed */
            GLCK("draw: hud_bind", b, f->va);
        } else if (f->pc_bits) {            /* the pass begins with its clear */
            clear_flush(f, ~(GLbitfield)0);
            apply_state(b->state);
        }
        /* Stereo in one pass: one instanced draw, instance = eye, each in its
         * half of a double-wide viewport, the other half clipped -- one draw
         * call and one set of uniforms for both eyes. VC3_STEREO_1PASS=0
         * draws each eye on its own (as where clip distances are missing). */
        static int one_pass = -1;
        if (one_pass < 0)
            one_pass = !(genv("STEREO_1PASS") && genv("STEREO_1PASS")[0] == '0');
        if (f->st && !hud && one_pass && s_has_clip) {
            GLfloat sb[8], e2[4];
            GLenum cd = raw ? GL_CLIP_DISTANCE0 + 2 : GL_CLIP_DISTANCE0;
            int k;
            for (k = 0; k < 4; k++) {
                sb[2 * k] = s_sbs[k];
                sb[2 * k + 1] = 0.0f;
            }
            cu4fv(U->sbs, 2, sb);
            e2[0] = s_imm ? 0.0f : 0.5f * s_st_sep;
            e2[1] = s_st_depth;
            e2[2] = s_imm ? 0.0f : -0.5f * s_st_sep;
            e2[3] = s_st_depth;
            cu2fv(U->eye2, 2, e2);
            if (imm3d)
                cuMatrix4fv(U->immvp2, 2, GL_FALSE, &s_view.vp[0][0]);
            cu1i(U->st2, 1);
            glViewport(0, 0, 2 * f->pw, f->ph);
            (void)cd;
            if (raw)
                glDrawElementsInstancedBaseVertex(raw_mode(raw->prim),
                                                  (GLsizei)raw->index_count, GL_UNSIGNED_SHORT,
                                                  (void *)ioff, 2, s_basevertex);
            else
                glDrawArraysInstanced(GL_TRIANGLES, 0, (GLsizei)b->count, 2);
            GLCK(raw ? "draw: one-pass stereo (raw)" : "draw: one-pass stereo (cpu)", b, f->va);
            glViewport(0, 0, f->pw, f->ph);
        }
        /* Stereo: the same draw for each eye, in its half. */
        int eye, eyes = f->st && !hud ? 2 : 1;
        if (f->st && !hud && one_pass && s_has_clip)
            eyes = 0;                       /* drawn above */
        else
            cu1i(U->st2, 0);
        for (eye = 0; eye < eyes; eye++) {
            GLfloat sb[8];
            int k;
            for (k = 0; k < 4; k++) {
                sb[2 * k] = s_sbs[k];
                sb[2 * k + 1] = 0.5f * (GLfloat)eye;
            }
            cu4fv(U->sbs, 2, sb);
            if (hud) {
                cu2f(U->eye, 0.0f, s_st_depth);
            } else if (f->st) {
                glViewport(eye * f->pw, 0, f->pw, f->ph);
                if (imm3d)
                    cuMatrix4fv(U->immvp, 1, GL_FALSE, s_view.vp[eye]);
                cu2f(U->eye, s_imm ? 0.0f : eye ? -0.5f * s_st_sep : 0.5f * s_st_sep,
                            s_st_depth);
            } else {
                cu2f(U->eye, 0.0f, s_st_depth);
            }
            if (raw)
                glDrawElementsBaseVertex(raw_mode(raw->prim), (GLsizei)raw->index_count,
                                         GL_UNSIGNED_SHORT, (void *)ioff, s_basevertex);
            else
                glDrawArrays(GL_TRIANGLES, 0, (GLsizei)b->count);
            GLCK(hud ? (raw ? "draw: HUD picture (raw)" : "draw: HUD picture (cpu)")
                 : raw ? "draw: draw (raw)" : "draw: draw (cpu)", b, f->va);
        }
        if (hud)
            glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
        if (f->st)
            glViewport(0, 0, f->pw, f->ph);
        TM(5);
    }
    if (!raw && genv("GL_RCCHECK") && b->rc && b->ext && s_use_rc
        && b->state->alpha_test_enable && (b->state->alpha_ref & 0xFF) == 0x80
        && b->rc->alpha_icw[1] == 0x191C0000u) {
        /* The GPU's own values for this batch, read back at the centre of
         * its first triangle: debug view 13 (vertex alpha, stage 0/1 bound)
         * then 12 (combiner alpha, t0.a, t1.a). */
        static int probes;
        float cx = (b->vertices[0].x + b->vertices[1].x + b->vertices[2].x) / 3.0f;
        float cy = (b->vertices[0].y + b->vertices[1].y + b->vertices[2].y) / 3.0f;
        if (probes < 8 && cx >= 0 && cy >= 0 && cx < f->w && cy < f->h) {
            GLint loc = U->rcdbg;
            unsigned char px13[4], px12[4];
            probes++;
            s_rs_valid = 0;
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
            glDisable(GL_STENCIL_TEST);
            glColorMask(1, 1, 1, 1);
            glUniform1i(U->atest, 0);
            glUniform1i(U->dbgon, 1);
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
            glUniform1i(loc, genv("GL_RCDBG") ? atoi(genv("GL_RCDBG")) : 0);
            uc_reset(U);
            fprintf(stderr, "[RCGPU] at %.0f,%.0f: v_col.a %u on0 %u on1 %u | comb.a %u t0.a %u t1.a %u"
                    " | diffuse[0] %08X\n", cx, cy, px13[0], px13[1], px13[2],
                    px12[0], px12[1], px12[2], b->vertices[0].diffuse);
        }
    }
    active_tex(0);
    f->dirty = 1;
    f->memq_valid = 0;
    f->cpu_owned = 0;
    GLCK("draw: end", b, f->va);
    if (vc3_fps_get() > 60)
        interp_scene_snap(f, b);
    GLCK("draw: interp_scene_snap", b, f->va);
}

/* Nv2aBackend.draw_raw: fixed-function batches, when the shader linked and
 * the batch fits the streams. Anything else goes back to the CPU path. */
static int gl_draw_raw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aRawGeometry *g = b->raw;
    uint32_t r;

    if (!g || g->index_count < 3 || !gl_ready())
        return 0;
    if (g->kind == NV2A_RAW_PROGRAM) {
        /* A screen-space batch (the CPU's first vertex at w = 1: HUD,
         * sprites, the full-screen copies) and one whose first vertex is
         * behind the eye (w <= 0: the E.S. distortion meshes) are drawn here
         * too. The lazy copies, the immersive HUD panel, the combiner routing
         * and the 16:9 HUD placement look at where their vertices land: the
         * executor gives that (g->d0, g->extent), the positions transformed
         * on the CPU only for a batch one of them asks about.
         * VC3_GL_VP_2D=0 keeps both kinds on the CPU, as before; =1 sends a
         * first vertex at NaN too. Immersive, a 3D one needs the game's
         * projection the CPU path would use. */
        static int vp2d = -1;
        const Surf *sf = surf_find(s->color_va);
        const int is3d = raw_vp_3d(g);
        if (vp2d < 0) {
            vp2d = !genv("GL_VP_2D") ? 2 : genv("GL_VP_2D")[0] == '1' ? 1
                 : genv("GL_VP_2D")[0] == '0' ? 0 : 2;
            fprintf(stderr, "[GL] screen-space program batches: %s (VC3_GL_VP_2D=%s)\n",
                    vp2d == 0 ? "on the CPU" : vp2d == 1 ? "on the GPU, every one"
                    : "on the GPU", genv("GL_VP_2D") ? genv("GL_VP_2D") : "");
        }
        if (vp2d == 0 && !(g->w0 > 0.0f && g->w0 != 1.0f))
            return 0;                       /* as before: on the CPU */
        if (vp2d == 2 && g->w0 != g->w0)
            return 0;
        if (is3d && s_imm && !(sf && (sf->gp_ok || s_gp_ok)))
            return 0;
        if (!(s_vp_cur = vp_get(g)))
            return 0;
    } else if (g->kind != NV2A_RAW_FIXED || !g->ff || !s_praw.prog) {
        return 0;
    }
    for (r = 0; r < 16; r++)
        if (g->attr[r].data && g->attr[r].bytes > (uint32_t)(s_vring.size / 8))
            return 0;
    if ((GLsizeiptr)g->index_count * 2 > s_iring.size)
        return 0;
    gl_draw(s, b);
    return 1;
}

/* ── Display frame interpolation (VC3_FPS > 60) ────────────── */

/* At 120 fps the window shows a made frame between each pair of real ones;
 * the game still renders and flips at 60 (display_settings.c). It is made here,
 * on the GPU, from the presented surface of this flip and the last:
 *
 *   0. The 3D scene and the 2D overlays are told apart: VC3 draws its 3D
 *      view into a surface of its own and composites it onto the back buffer
 *      with one full-screen quad, then draws the HUD over it. A copy taken
 *      right after that quad (interp_scene_snap) is the frame without HUD.
 *      Steps 1-4 interpolate that scene layer; every pixel where the real
 *      frame differs from its scene layer -- HUD, text, the lock-on rings,
 *      which scale and turn in ways no block vector follows -- comes from
 *      the nearer real frame as it is. Without such a copy (menus, movies)
 *      the whole frame is interpolated.
 *   1. Each flip copies the presented surface, as drawn, into a history
 *      texture, and makes a luma texture at the guest's size (640x480, or
 *      854x480 at 16:9) with mipmaps (1, 1/2, 1/4, 1/8). Steps 2 and 3 run
 *      at that size whatever the internal resolution, so their cost does
 *      not grow with it.
 *   2. A motion field, one vector per 8x8 block, is found by symmetric block
 *      matching: v such that prev(p - v/2) matches cur(p + v/2), which puts
 *      the field on the in-between frame's own grid -- no splatting, no
 *      holes. Coarse to fine: a wide search at 1/8 size (+-64 px), then
 *      +-1 step refinements at 1/4, 1/2 and full size around the block's
 *      own vector and its four neighbours'. Every level adds a cost per
 *      pixel of motion and gives the zero vector a head start, so a block
 *      whose match is about as good standing still stays still.
 *   3. Each pixel takes, of its block's and the eight neighbours' vectors
 *      and zero, the one with the least colour error over a 3x3 patch
 *      (sharp motion edges), and a weight that falls from 1 to 0 as that
 *      error grows (occlusion, transparency, a flash).
 *   4. Each output pixel, at the drawn size, samples prev at p - t v and
 *      cur at p + (1 - t) v and blends them, faded by the weight towards
 *      the nearer real frame, unwarped. A pixel equal in both frames is
 *      copied exactly, so the HUD and anything else static stays
 *      bit-identical; and when the mean block error says the two frames
 *      are unrelated (a cut), the whole frame is the nearer real one, i.e.
 *      a repeat.
 *
 * The made frames are read back like the real one, without waiting, and at
 * the next flip go to the window as a group with it: made frames first,
 * then the real frame, which the window spreads over the flip interval. A
 * real frame thus reaches the screen one display frame (8.3 ms at 120)
 * later than at 60 fps; nothing else waits. All of it is queued GL on this
 * thread: a few draws and a read per flip, no CPU-side wait. */

#define INTERP_MAX  (VC3_FPS_MAX / 60 - 1)    /* in-between frames per flip */
#define INTERP_BLK  8

extern uint64_t xbox_FramebufferWindowPresentCount(void);
extern int  xbox_FramebufferWindowOpen(void);

static const char *VS_QUAD =
    "#version 410\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

/* The immersive HUD picture over one eye of the desktop test (gl_flip):
 * premultiplied, as the headset's quad layer takes it. */
static const char *FS_HUD =
    "#version 410\n"
    "uniform sampler2D u_img;\n"
    "uniform vec2 u_org, u_size;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  o = texture(u_img, (gl_FragCoord.xy - u_org) / u_size);\n"
    "}\n";

/* At the guest size, from the frame as drawn (filtered down when larger). */
static const char *FS_LUMA =
    "#version 410\n"
    "uniform sampler2D u_img;\n"
    "uniform vec2 u_size;\n"
    "out float o;\n"
    "void main() {\n"
    "  vec3 c = textureLod(u_img, gl_FragCoord.xy / u_size, 0.0).rgb;\n"
    "  o = dot(c, vec3(0.299, 0.587, 0.114));\n"
    "}\n";

/* One fragment per 8x8 block. Output: vector (full-size pixels), the luma
 * error of the chosen vector (mean absolute difference), 0. */
static const char *FS_SEARCH =
    "#version 410\n"
    "uniform sampler2D u_la, u_lb, u_prev;\n"
    "uniform vec2 u_size;\n"
    "uniform float u_lod, u_lambda, u_zbias;\n"
    "uniform int u_win, u_range, u_coarse;\n"
    "uniform ivec2 u_fsize;\n"
    "out vec4 o;\n"
    "float cost(vec2 c, vec2 v, float s) {\n"
    "  vec2 h = 0.5 * v;\n"
    "  float sum = 0.0;\n"
    "  for (int j = -u_win; j < u_win; j++)\n"
    "    for (int i = -u_win; i < u_win; i++) {\n"
    "      vec2 q = c + (vec2(i, j) + 0.5) * s;\n"
    "      sum += abs(textureLod(u_la, (q - h) / u_size, u_lod).r\n"
    "               - textureLod(u_lb, (q + h) / u_size, u_lod).r);\n"
    "    }\n"
    "  return sum / float(4 * u_win * u_win);\n"
    "}\n"
    "void main() {\n"
    "  ivec2 bc = ivec2(gl_FragCoord.xy);\n"
    "  vec2 c = (vec2(bc) + 0.5) * 8.0;\n"
    "  float s = exp2(u_lod);\n"
    "  float raw0 = cost(c, vec2(0.0), s);\n"
    "  float best = raw0 - u_zbias, braw = raw0;\n"
    "  vec2 bv = vec2(0.0);\n"
    "  vec2 cand[5];\n"
    "  int n = 1;\n"
    "  cand[0] = vec2(0.0);\n"
    "  if (u_coarse == 0) {\n"
    "    ivec2 hi = u_fsize - 1;\n"
    "    cand[0] = texelFetch(u_prev, bc, 0).xy;\n"
    "    cand[1] = texelFetch(u_prev, clamp(bc + ivec2(-1, 0), ivec2(0), hi), 0).xy;\n"
    "    cand[2] = texelFetch(u_prev, clamp(bc + ivec2( 1, 0), ivec2(0), hi), 0).xy;\n"
    "    cand[3] = texelFetch(u_prev, clamp(bc + ivec2(0, -1), ivec2(0), hi), 0).xy;\n"
    "    cand[4] = texelFetch(u_prev, clamp(bc + ivec2(0,  1), ivec2(0), hi), 0).xy;\n"
    "    n = 5;\n"
    "  }\n"
    "  for (int k = 0; k < n; k++) {\n"
    "    if (k > 0 && cand[k] == cand[0]) continue;\n"
    "    for (int dy = -u_range; dy <= u_range; dy++)\n"
    "      for (int dx = -u_range; dx <= u_range; dx++) {\n"
    "        vec2 v = clamp(cand[k] + vec2(dx, dy) * s, vec2(-128.0), vec2(128.0));\n"
    "        if (v == vec2(0.0)) continue;\n"
    "        float r = cost(c, v, s);\n"
    "        float e = r + u_lambda * length(v);\n"
    "        if (e < best) { best = e; braw = r; bv = v; }\n"
    "      }\n"
    "  }\n"
    "  o = vec4(bv, braw, 0.0);\n"
    "}\n";

/* One fragment per guest-size pixel, at time u_t between prev (A) and cur
 * (B), sampled from the frames as drawn: the vector to use (guest pixels)
 * and how far to trust it. */
static const char *FS_SELECT =
    "#version 410\n"
    "uniform sampler2D u_a, u_b, u_field;\n"
    "uniform vec2 u_size;\n"
    "uniform ivec2 u_fsize;\n"
    "uniform float u_t, u_lo, u_hi, u_zbias;\n"
    "out vec4 o;\n"
    "vec3 A(vec2 p) { return textureLod(u_a, p / u_size, 0.0).rgb; }\n"
    "vec3 B(vec2 p) { return textureLod(u_b, p / u_size, 0.0).rgb; }\n"
    "float perr(vec2 p, vec2 v) {\n"
    "  vec2 da = -u_t * v, db = (1.0 - u_t) * v;\n"
    "  vec3 e = vec3(0.0);\n"
    "  for (int j = -1; j <= 1; j++)\n"
    "    for (int i = -1; i <= 1; i++) {\n"
    "      vec2 q = p + vec2(i, j);\n"
    "      e += abs(A(q + da) - B(q + db));\n"
    "    }\n"
    "  return (e.r + e.g + e.b) / 9.0;\n"
    "}\n"
    "void main() {\n"
    "  vec2 p = gl_FragCoord.xy;\n"
    "  ivec2 bc = clamp(ivec2(p) / 8, ivec2(0), u_fsize - 1);\n"
    "  float best = perr(p, vec2(0.0)) - u_zbias;\n"
    "  vec2 bv = vec2(0.0);\n"
    "  for (int j = -1; j <= 1; j++)\n"
    "    for (int i = -1; i <= 1; i++) {\n"
    "      vec2 v = texelFetch(u_field, clamp(bc + ivec2(i, j), ivec2(0), u_fsize - 1), 0).xy;\n"
    "      if (v == vec2(0.0)) continue;\n"
    "      float e = perr(p, v);\n"
    "      if (e < best) { best = e; bv = v; }\n"
    "    }\n"
    /* And one pixel either way: an overlay that creeps (VC3's lock-on
     * reticle) moves with none of the blocks behind it, and a thin line is
     * otherwise matched away to the background on both sides. */
    "  for (int k = 0; k < 4; k++) {\n"
    "    vec2 v = vec2(k == 0 ? 1.0 : k == 1 ? -1.0 : 0.0, k == 2 ? 1.0 : k == 3 ? -1.0 : 0.0);\n"
    "    float e = perr(p, v);\n"
    "    if (e < best) { best = e; bv = v; }\n"
    "  }\n"
    "  if (bv == vec2(0.0)) best += u_zbias;\n"
    "  o = vec4(bv, 1.0 - smoothstep(u_lo, u_hi, best), 0.0);\n"
    "}\n";

/* One fragment per output pixel, at the drawn size. */
static const char *FS_MIX =
    "#version 410\n"
    "uniform sampler2D u_a, u_b, u_field, u_sel, u_fa, u_fb;\n"
    "uniform vec2 u_size, u_k;\n"          /* drawn size; drawn px per guest px */
    "uniform float u_t, u_flod, u_cut;\n"
    "uniform int u_layers;\n"              /* a, b are scene layers of fa, fb */
    "out vec4 o;\n"
    "void main() {\n"
    "  vec2 p = gl_FragCoord.xy;\n"
    "  ivec2 pi = ivec2(p);\n"
    "  vec4 a0 = texelFetch(u_a, pi, 0), b0 = texelFetch(u_b, pi, 0);\n"
    "  vec4 near0 = u_t < 0.5 ? a0 : b0;\n"
    "  vec4 fnear = u_t < 0.5 ? texelFetch(u_fa, pi, 0) : texelFetch(u_fb, pi, 0);\n"
    /* Overlay drawn here in the nearer frame: that frame's pixel. */
    "  if (u_layers != 0 && any(greaterThan(abs(fnear.rgb - near0.rgb), vec3(0.5 / 255.0)))) { o = fnear; return; }\n"
    /* A cut: the frames have nothing to interpolate. */
    "  if (textureLod(u_field, vec2(0.5), u_flod).z > u_cut) { o = fnear; return; }\n"
    /* Unchanged here: exactly as it is (HUD, text, still background). */
    "  if (all(lessThan(abs(a0.rgb - b0.rgb), vec3(1.5 / 255.0)))) { o = fnear; return; }\n"
    "  vec4 sel = texelFetch(u_sel, ivec2(p / u_k), 0);\n"
    "  vec2 v = sel.xy * u_k;\n"
    "  vec3 a = v == vec2(0.0) ? a0.rgb : textureLod(u_a, (p - u_t * v) / u_size, 0.0).rgb;\n"
    "  vec3 b = v == vec2(0.0) ? b0.rgb : textureLod(u_b, (p + (1.0 - u_t) * v) / u_size, 0.0).rgb;\n"
    "  o = vec4(mix(near0.rgb, mix(a, b, u_t), sel.z), 1.0);\n"
    "}\n";

typedef struct {
    GLuint prog;
    GLint la, lb, prev, size, lod, lambda, zbias, win, range, coarse, fsize;
} SearchProg;

static struct {
    int state;                       /* 0 not set up, 1 ready, -1 failed */
    GLuint vao;
    GLuint p_luma, p_sel, p_mix;
    GLint luma_img, luma_size;
    GLint s_a, s_b, s_field, s_size, s_fsize, s_t, s_lo, s_hi, s_zbias;
    GLint m_a, m_b, m_field, m_sel, m_size, m_k, m_t, m_flod, m_cut, m_fa, m_fb, m_layers;
    SearchProg s;
    GLsizei w, h;                    /* the frame as drawn */
    GLsizei gw, gh, fw, fh;          /* guest size, field size */
    GLuint sel, sel_fbo;             /* per guest pixel: vector, weight */
    GLuint hist[2], hist_fbo[2];     /* RGBA8 copies of the last two frames */
    GLuint shist[2], shist_fbo[2];   /* and of their scene layers */
    int has_scene[2];
    GLuint scene, scene_fbo;         /* this frame's scene layer, as drawn */
    GLsizei scene_w, scene_h;
    uint32_t scene_va, scene_frame;
    GLuint luma[2], luma_fbo[2];     /* their luma, with mipmaps */
    uint32_t hist_frame[2];          /* s_frame each was taken at */
    int cur;                         /* the newest of the two */
    GLuint field[2], field_fbo[2];   /* RGBA16F motion fields, ping-pong */
    GLint field_levels;
    GLuint mid, mid_fbo;             /* the frame being made */
    uint64_t made;                   /* in-between frames made, for the log */
    float cut, lo, hi;               /* VC3_FPS_TUNE overrides */
} s_ip;

static GLuint link_quad(const char *fs_src)
{
    GLuint vs = compile(GL_VERTEX_SHADER, VS_QUAD), fs = compile(GL_FRAGMENT_SHADER, fs_src);
    GLuint p;
    GLint ok = 0;
    char log[1024];

    if (!vs || !fs)
        return 0;
    p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "[GL] interpolator link failed: %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static int interp_setup(void)
{
    GLuint p;

    if (s_ip.state)
        return s_ip.state > 0;
    s_ip.state = -1;
    s_ip.p_luma = link_quad(FS_LUMA);
    s_ip.p_sel = link_quad(FS_SELECT);
    s_ip.p_mix = link_quad(FS_MIX);
    s_ip.s.prog = p = link_quad(FS_SEARCH);
    if (!s_ip.p_luma || !s_ip.p_sel || !s_ip.p_mix || !p) {
        fprintf(stderr, "[GL] frame interpolation unavailable; showing real frames\n");
        return 0;
    }
    s_ip.luma_img = glGetUniformLocation(s_ip.p_luma, "u_img");
    s_ip.luma_size = glGetUniformLocation(s_ip.p_luma, "u_size");
#define SL(f, n) s_ip.s.f = glGetUniformLocation(p, n)
    SL(la, "u_la"); SL(lb, "u_lb"); SL(prev, "u_prev"); SL(size, "u_size");
    SL(lod, "u_lod"); SL(lambda, "u_lambda"); SL(zbias, "u_zbias"); SL(win, "u_win");
    SL(range, "u_range"); SL(coarse, "u_coarse"); SL(fsize, "u_fsize");
#undef SL
#define ML(f, n) s_ip.f = glGetUniformLocation(p, n)
    p = s_ip.p_sel;
    ML(s_a, "u_a"); ML(s_b, "u_b"); ML(s_field, "u_field"); ML(s_size, "u_size");
    ML(s_fsize, "u_fsize"); ML(s_t, "u_t"); ML(s_lo, "u_lo"); ML(s_hi, "u_hi");
    ML(s_zbias, "u_zbias");
    p = s_ip.p_mix;
    ML(m_a, "u_a"); ML(m_b, "u_b"); ML(m_field, "u_field"); ML(m_sel, "u_sel");
    ML(m_size, "u_size"); ML(m_k, "u_k"); ML(m_t, "u_t"); ML(m_flod, "u_flod");
    ML(m_cut, "u_cut"); ML(m_fa, "u_fa"); ML(m_fb, "u_fb"); ML(m_layers, "u_layers");
#undef ML
    glGenVertexArrays(1, &s_ip.vao);
    /* Thresholds, tuned on combat footage: mean block luma error above
     * `cut` is a scene cut; per-pixel colour error (sum of channels, 3x3
     * mean) from `lo` to `hi` fades from interpolated to the real frame.
     * VC3_FPS_TUNE=cut,lo,hi overrides them. */
    s_ip.cut = 0.10f;
    s_ip.lo = 0.06f;
    s_ip.hi = 0.20f;
    if (genv("FPS_TUNE"))
        sscanf(genv("FPS_TUNE"), "%f,%f,%f", &s_ip.cut, &s_ip.lo, &s_ip.hi);
    s_ip.state = 1;
    return 1;
}

static void tex_image(GLuint *tex, GLenum ifmt, GLenum fmt, GLenum type,
                      GLsizei w, GLsizei h, int mips, GLenum filter)
{
    if (!*tex)
        glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)ifmt, w, h, 0, fmt, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    mips ? (filter == GL_LINEAR ? GL_LINEAR_MIPMAP_NEAREST
                                                : GL_NEAREST_MIPMAP_NEAREST)
                         : (GLint)filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (mips)
        glGenerateMipmap(GL_TEXTURE_2D);
}

static void fbo_for(GLuint *fbo, GLuint tex)
{
    if (!*fbo)
        glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
}

/* (Re)size every target for a frame drawn w x h, guest size gw x gh. */
static void interp_alloc(GLsizei w, GLsizei h, GLsizei gw, GLsizei gh)
{
    int i;
    GLsizei m;

    s_ip.w = w;
    s_ip.h = h;
    s_ip.gw = gw;
    s_ip.gh = gh;
    s_ip.fw = (gw + INTERP_BLK - 1) / INTERP_BLK;
    s_ip.fh = (gh + INTERP_BLK - 1) / INTERP_BLK;
    for (i = 0; i < 2; i++) {
        tex_image(&s_ip.hist[i], GL_RGBA8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                  w, h, 0, GL_LINEAR);
        fbo_for(&s_ip.hist_fbo[i], s_ip.hist[i]);
        tex_image(&s_ip.shist[i], GL_RGBA8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                  w, h, 0, GL_LINEAR);
        fbo_for(&s_ip.shist_fbo[i], s_ip.shist[i]);
        s_ip.has_scene[i] = 0;
        tex_image(&s_ip.luma[i], GL_R16F, GL_RED, GL_FLOAT, gw, gh, 1, GL_LINEAR);
        fbo_for(&s_ip.luma_fbo[i], s_ip.luma[i]);
        tex_image(&s_ip.field[i], GL_RGBA16F, GL_RGBA, GL_FLOAT, s_ip.fw, s_ip.fh,
                  1, GL_NEAREST);
        fbo_for(&s_ip.field_fbo[i], s_ip.field[i]);
        s_ip.hist_frame[i] = 0;
    }
    /* The field's 1x1 level holds the mean block error. */
    for (s_ip.field_levels = 0, m = s_ip.fw > s_ip.fh ? s_ip.fw : s_ip.fh; m > 1; m >>= 1)
        s_ip.field_levels++;
    tex_image(&s_ip.sel, GL_RGBA16F, GL_RGBA, GL_FLOAT, gw, gh, 0, GL_NEAREST);
    fbo_for(&s_ip.sel_fbo, s_ip.sel);
    tex_image(&s_ip.mid, GL_RGBA8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, w, h, 0,
              GL_NEAREST);
    fbo_for(&s_ip.mid_fbo, s_ip.mid);
}

static void bind_tex(int unit, GLuint tex)
{
    active_tex(unit);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (s_unit_sampler[unit]) {        /* the texture's own parameters */
        glBindSampler((GLuint)unit, 0);
        s_unit_sampler[unit] = 0;
    }
}

static void quad_into(GLuint fbo, GLsizei w, GLsizei h)
{
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* ── The effect target (reduced-resolution blended stacks) ─────────────
 *
 * VC3's E.S. mode redraws the scene through ~63 distortion meshes, each
 * blended (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) over the whole view through the
 * combiners, sampling a copy of the scene at distorted coordinates, most of
 * each one then discarded by its alpha test. At native size that is 63
 * layers over two 2064x2208 eyes -- ~570 Mpx of combiner shading a frame,
 * 6-9 fps even on the Mac -- for an effect that is a blur and a ripple.
 *
 * So a run of such batches (blended that way, sampling another render
 * target, no depth or stencil writes, 3D, into a large stereo surface S,
 * after a frame that had at least FX_MIN of them) is drawn into
 * an effect target L of 1/scale the size each way instead: its depth is S's
 * scaled down (the meshes are depth-tested against the scene); its colour
 * starts as (0, 0, 0, 1) and each batch blends colour as it would over S
 * and alpha as ZERO, ONE_MINUS_SRC_ALPHA -- so L holds the stack's own
 * contribution C and the scene's remaining weight T. When the run ends (a
 * draw or clear of anything else, the flip) L goes onto S once, filtered up:
 * S = C + S * T -- exactly what the layers blended one by one over S would
 * give, but with the contribution at the effect's resolution and the scene
 * itself kept native wherever the effect leaves it.
 *
 * VC3_GL_EFFECT_SCALE: 2 (default: half each way, a quarter of the pixels),
 * 3, 4, or 1 (off: every layer at full size). Only surfaces of FX_PX
 * pixels an eye and up (native immersive), never at 2x. */
#define FX_MIN 8                   /* qualifying batches in the frame before */
#define FX_PX 2000000L             /* an eye's pixels for the effect target */

static int fx_scale(void)
{
    static int sc = -1;
    if (sc < 0) {
        const char *e = genv("GL_EFFECT_SCALE");
        sc = e ? atoi(e) : 2;
        if (sc < 1 || sc > 8)
            sc = 1;
    }
    return sc;
}

static int fx_qualifies(const Surf *f, const Nv2aBatch *b)
{
    const Nv2aRenderState *rs = b->state;
    const Surf *rt;
    if (!f || !f->st || !f->fbo || !rs || !b->texture || b->zpass_out)
        return 0;
    if (!rs->blend_enable || rs->blend_src != GL_SRC_ALPHA
        || rs->blend_dst != GL_ONE_MINUS_SRC_ALPHA
        || (rs->blend_eq && rs->blend_eq != GL_FUNC_ADD))
        return 0;
    if ((rs->depth_write && rs->zeta_va) || (rs->stencil_enable && rs->zeta_va)
        || rs->color_mask != 0x01010101u)
        return 0;
    rt = surf_find(b->texture->offset);
    if (!rt || rt == f || !rt->fbo || rt->cpu_owned)
        return 0;
    /* 3D (drawn into the eyes): never a 2D batch, which may go to the HUD */
    if (b->raw)
        return b->raw->kind != NV2A_RAW_PROGRAM || raw_vp_3d(b->raw);
    return b->count && b->vertices[0].rhw != 1.0f && b->vertices[0].rhw > 0.0f;
}

static void fx_begin(Surf *f)
{
    int sc = fx_scale();
    GLsizei w = (f->pw + sc - 1) / sc, h = (f->ph + sc - 1) / sc;
    static int logged;

    clear_flush(f, ~(GLbitfield)0);
    if (!s_fx.fbo || s_fx.w != w || s_fx.h != h) {
        if (!s_fx.fbo) {
            glGenFramebuffers(1, &s_fx.fbo);
            glGenTextures(1, &s_fx.color);
            glGenRenderbuffers(1, &s_fx.depth);
        }
        glBindTexture(GL_TEXTURE_2D, s_fx.color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2 * w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindRenderbuffer(GL_RENDERBUFFER, s_fx.depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 2 * w, h);
        glBindFramebuffer(GL_FRAMEBUFFER, s_fx.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_fx.color, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                  s_fx.depth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "[GL] effect target %dx%d incomplete\n", 2 * w, h);
        s_fx.w = w;
        s_fx.h = h;
    }
    if (!logged) {
        logged = 1;
        fprintf(stderr, "[GL] effect target: a blended stack into %08X drawn at 1/%d"
                " (%dx%d an eye; VC3_GL_EFFECT_SCALE)\n", f->va, sc, w, h);
    }
    /* the scene's depth, scaled down; the stack's contribution none yet */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, f->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fx.fbo);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, 2 * f->pw, f->ph, 0, 0, 2 * w, h,
                      GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fx.fbo);
    glColorMask(1, 1, 1, 1);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    s_rs_valid = 0;
    s_fx.on = 1;
    s_fx.s = f;
    s_fx.runs++;
    GLCK("effect target: begin", NULL, f->va);
}

/* rt reduced to the effect target's scale, for the batches of this run to
 * sample: each pixel of L read the full-size copy of the scene at distorted
 * coordinates, a scattered walk over 4128x2208 texels per layer -- half of
 * what the stack cost. Made once per run (the copy changes every frame). */
static GLuint fx_small(const Surf *rt)
{
    int sc = fx_scale();
    GLsizei w = (rt->pw * (rt->st ? 2 : 1) + sc - 1) / sc, h = (rt->ph + sc - 1) / sc;
    GLint rd = 0, dr = 0, tex = 0;
    if (s_fx.small_run == s_fx.runs && s_fx.small_va == rt->va && s_fx.small_tex)
        return s_fx.small_tex;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    if (!s_fx.small_tex || s_fx.small_w != w || s_fx.small_h != h) {
        if (!s_fx.small_tex) {
            glGenTextures(1, &s_fx.small_tex);
            glGenFramebuffers(1, &s_fx.small_fbo);
        }
        glBindTexture(GL_TEXTURE_2D, s_fx.small_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, s_fx.small_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               s_fx.small_tex, 0);
        s_fx.small_w = w;
        s_fx.small_h = h;
    }
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rd);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dr);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rt->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fx.small_fbo);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, rt->pw * (rt->st ? 2 : 1), rt->ph, 0, 0, w, h, GL_COLOR_BUFFER_BIT,
                      GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rd);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)dr);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    s_fx.small_run = s_fx.runs;
    s_fx.small_va = rt->va;
    return s_fx.small_tex;
}

/* L onto S: S = C + S * T (see above). */
static void fx_resolve(void)
{
    static GLuint prog, vao;
    static GLint u_img, u_org, u_size;
    Surf *f = s_fx.s;

    s_fx.on = 0;
    if (!f || !f->fbo)
        return;
    if (!prog && (prog = link_quad(FS_HUD))) {
        u_img = glGetUniformLocation(prog, "u_img");
        u_org = glGetUniformLocation(prog, "u_org");
        u_size = glGetUniformLocation(prog, "u_size");
    }
    if (!prog)
        return;
    if (!vao)
        glGenVertexArrays(1, &vao);
    glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
    s_bound_fbo = f->fbo;
    glViewport(0, 0, 2 * f->pw, f->ph);
    glUseProgram(prog);
    s_cur_prog = prog;
    glBindVertexArray(vao);
    s_cur_vao = vao;
    bind_tex(0, s_fx.color);
    glUniform1i(u_img, 0);
    glUniform2f(u_org, 0.0f, 0.0f);
    glUniform2f(u_size, (GLfloat)(2 * f->pw), (GLfloat)f->ph);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    caps_off();
    glColorMask(1, 1, 1, 1);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_SRC_ALPHA, GL_ZERO, GL_ONE);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glViewport(0, 0, f->pw, f->ph);
    s_rs_valid = 0;
    f->dirty = 1;
    GLCK("effect target: resolve", NULL, f->va);
}

/* Before a draw (gl_draw): end a run this batch does not continue; begin
 * one when it should; mark the draw as going into L (surf_bind swaps it). */
static void fx_pre(const Nv2aSurface *s, const Nv2aBatch *b)
{
    Surf *f = surf_find(s->color_va);
    int q;
    s_fx.draw = 0;
    q = f ? fx_qualifies(f, b) : 0;
    if (s_fx.on && (f != s_fx.s || !q))
        fx_resolve();
    if (!q)
        return;
    s_fx.n_this++;
    if (!s_fx.on && s_fx.n_prev >= FX_MIN && fx_scale() > 1 && (long)f->pw * f->ph >= FX_PX)
        fx_begin(f);
    if (s_fx.on) {
        s_fx.draw = 1;
        s_rs_valid = 0;
    }
}

/* After it: the surface as it was. */
static void fx_post(void)
{
    if (s_fx.swapped) {
        Surf *f = s_fx.s;
        f->fbo = s_fx.save_fbo;
        f->pw = s_fx.save_pw;
        f->ph = s_fx.save_ph;
        f->cw = s_fx.save_cw;
        s_fx.swapped = 0;
    }
    if (s_fx.draw)
        s_rs_valid = 0;
    s_fx.draw = 0;
}


/* One matching level, from field[dst ^ 1] (the level before) into
 * field[dst]. */
static void search_pass(int coarse, float lod, int win, int range, int dst)
{
    const SearchProg *sp = &s_ip.s;
    int a = s_ip.cur ^ 1, b = s_ip.cur;

    bind_tex(0, s_ip.luma[a]);
    bind_tex(1, s_ip.luma[b]);
    bind_tex(2, s_ip.field[dst ^ 1]);
    glUniform1i(sp->la, 0);
    glUniform1i(sp->lb, 1);
    glUniform1i(sp->prev, 2);
    glUniform2f(sp->size, (float)s_ip.gw, (float)s_ip.gh);
    glUniform2i(sp->fsize, s_ip.fw, s_ip.fh);
    glUniform1f(sp->lod, lod);
    /* A pixel of motion costs 1/2500 of luma range; zero starts 1/250
     * ahead. Small enough not to hold back real motion, large enough that
     * flat or static areas keep zero. */
    glUniform1f(sp->lambda, 0.0004f);
    glUniform1f(sp->zbias, 0.004f);
    glUniform1i(sp->win, win);
    glUniform1i(sp->range, range);
    glUniform1i(sp->coarse, coarse);
    quad_into(s_ip.field_fbo[dst], s_ip.fw, s_ip.fh);
}

/* After each draw, above 60 fps: a batch that covers a screen-shaped
 * surface edge to edge with another surface's picture is VC3's composite
 * of its 3D view onto the back buffer; what that surface holds right after
 * it is the frame without HUD. The first such composite per frame and
 * surface is kept (a later full-screen pass would already have HUD under
 * it); which surface is presented is known only at the flip. */
static void interp_scene_snap(const Surf *f, const Nv2aBatch *b)
{
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    const Surf *src;

    if ((b->raw && !raw_2d(b->raw)) || !b->texture || f->w * 3 != f->h * 4
        || (s_ip.scene_frame == s_frame && s_ip.scene_va == f->va))
        return;
    src = surf_find(b->texture->offset);
    if (!src || src == f || src->cpu_owned)
        return;
    {
        Nv2aRawExtent e;
        if (!batch_extent(b, &e))
            return;
        x0 = e.x0; x1 = e.x1; y0 = e.y0; y1 = e.y1;
    }
    if (x0 > 1.0f || y0 > 1.0f || x1 < (float)f->w - 1.0f || y1 < (float)f->h - 1.0f)
        return;
    if (!s_ip.scene || s_ip.scene_w != f->pw || s_ip.scene_h != f->ph) {
        tex_image(&s_ip.scene, GL_RGBA8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                  f->pw, f->ph, 0, GL_NEAREST);
        fbo_for(&s_ip.scene_fbo, s_ip.scene);
        s_ip.scene_w = f->pw;
        s_ip.scene_h = f->ph;
    }
    clear_flush((Surf *)f, GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, f->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_ip.scene_fbo);
    glBlitFramebuffer(0, 0, f->pw, f->ph, 0, 0, f->pw, f->ph,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
    s_ip.scene_va = f->va;
    s_ip.scene_frame = s_frame;
}

/* Copy the presented surface (as drawn: pw x ph) into the history and,
 * when the frame before it is there too, make the in-between frames into
 * the readback's buffers (BGRA, pw x ph). Returns how many were made. */
static int interp_make(const Surf *f, GLuint *pbos, int n)
{
    int prev, i, layers;
    GLsizei w = f->pw, h = f->ph;
    GLsizei gw = (GLsizei)((float)f->pw / f->sx + 0.5f), gh = f->h;
    GLuint ta, tb;                   /* what is interpolated: scene or frame */

    if (n < 1 || !interp_setup())
        return 0;
    clear_flush((Surf *)f, GL_COLOR_BUFFER_BIT);
    if (w != s_ip.w || h != s_ip.h || gw != s_ip.gw || gh != s_ip.gh)
        interp_alloc(w, h, gw, gh);

    s_ip.cur ^= 1;
    prev = s_ip.cur ^ 1;
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(1, 1, 1, 1);
    s_rs_valid = 0;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, f->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_ip.hist_fbo[s_ip.cur]);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    s_ip.hist_frame[s_ip.cur] = s_frame;
    s_ip.has_scene[s_ip.cur] = s_ip.scene && s_ip.scene_frame == s_frame
        && s_ip.scene_va == f->va && s_ip.scene_w == w && s_ip.scene_h == h;
    if (s_ip.has_scene[s_ip.cur]) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_ip.scene_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_ip.shist_fbo[s_ip.cur]);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    layers = s_ip.has_scene[0] && s_ip.has_scene[1];
    ta = layers ? s_ip.shist[prev] : s_ip.hist[prev];
    tb = layers ? s_ip.shist[s_ip.cur] : s_ip.hist[s_ip.cur];

    glBindVertexArray(s_ip.vao);
    s_cur_vao = 0;
    glUseProgram(s_ip.p_luma);
    s_cur_prog = 0;
    bind_tex(0, tb);
    glUniform1i(s_ip.luma_img, 0);
    glUniform2f(s_ip.luma_size, (float)gw, (float)gh);
    quad_into(s_ip.luma_fbo[s_ip.cur], gw, gh);
    glBindTexture(GL_TEXTURE_2D, s_ip.luma[s_ip.cur]);
    glGenerateMipmap(GL_TEXTURE_2D);

    /* Only between consecutive flips: after a gap (no readback, a mode
     * switch) the older copy is not the frame before. */
    if (s_ip.hist_frame[prev] + 1 != s_frame) {
        active_tex(0);
        return 0;
    }

    glUseProgram(s_ip.s.prog);
    search_pass(1, 3.0f, 2, 8, 0);   /* 1/8: 32x32 window, +-64 px */
    search_pass(0, 2.0f, 3, 1, 1);   /* 1/4: 24x24, +-4 px around */
    search_pass(0, 1.0f, 4, 1, 0);   /* 1/2: 16x16, +-2 px */
    search_pass(0, 0.0f, 6, 1, 1);   /* 1:   12x12, +-1 px */
    glBindTexture(GL_TEXTURE_2D, s_ip.field[1]);
    glGenerateMipmap(GL_TEXTURE_2D);

    bind_tex(0, ta);
    bind_tex(1, tb);
    bind_tex(2, s_ip.field[1]);
    bind_tex(3, s_ip.sel);
    bind_tex(4, s_ip.hist[prev]);
    bind_tex(5, s_ip.hist[s_ip.cur]);
    for (i = 0; i < n; i++) {
        float t = (float)(i + 1) / (float)(n + 1);

        glUseProgram(s_ip.p_sel);
        glUniform1i(s_ip.s_a, 0);
        glUniform1i(s_ip.s_b, 1);
        glUniform1i(s_ip.s_field, 2);
        glUniform2f(s_ip.s_size, (float)gw, (float)gh);
        glUniform2i(s_ip.s_fsize, s_ip.fw, s_ip.fh);
        glUniform1f(s_ip.s_t, t);
        glUniform1f(s_ip.s_lo, s_ip.lo);
        glUniform1f(s_ip.s_hi, s_ip.hi);
        glUniform1f(s_ip.s_zbias, 0.01f);
        quad_into(s_ip.sel_fbo, gw, gh);

        glUseProgram(s_ip.p_mix);
        glUniform1i(s_ip.m_a, 0);
        glUniform1i(s_ip.m_b, 1);
        glUniform1i(s_ip.m_field, 2);
        glUniform1i(s_ip.m_sel, 3);
        glUniform2f(s_ip.m_size, (float)w, (float)h);
        glUniform2f(s_ip.m_k, (float)w / (float)gw, (float)h / (float)gh);
        glUniform1f(s_ip.m_t, t);
        glUniform1f(s_ip.m_flod, (float)s_ip.field_levels);
        glUniform1f(s_ip.m_cut, s_ip.cut);
        glUniform1i(s_ip.m_fa, 4);
        glUniform1i(s_ip.m_fb, 5);
        glUniform1i(s_ip.m_layers, layers);
        quad_into(s_ip.mid_fbo, w, h);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_ip.mid_fbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[i]);
        glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    active_tex(0);
    if (genv("FPS_DEBUG")) {
        /* The scene-cut measure, read back now (a stall; debug only). */
        {
            float mean[4] = { 0 };
            GLuint tmp = 0;
            glGenFramebuffers(1, &tmp);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, tmp);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, s_ip.field[1], s_ip.field_levels);
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, mean);
            glDeleteFramebuffers(1, &tmp);
            fprintf(stderr, "[INTERP] flip %u: mean block error %.4f%s\n", s_frame,
                    mean[2], mean[2] > s_ip.cut ? " (cut)" : "");
            s_ip_last_cut = mean[2] > s_ip.cut;
        }
    }
    s_ip.made += (uint64_t)n;
    return n;
}

/* VC3_FPS_DUMP=<prefix>: write each group the window gets as BMPs,
 * <prefix><flip>_<k>.bmp with k the frame's place in the group (the last is
 * the real one), while VC3_FPS_DUMP_FLIPS names a file that exists (or is
 * "1"; "cuts", with VC3_FPS_DEBUG, writes only groups made as scene cuts).
 * For checking made frames headless, where nothing is shown. */
static void interp_dump(const void *const *px, int n, uint32_t pitch,
                        uint32_t bpp, GLsizei w, GLsizei h, int cut)
{
    const char *prefix = genv("FPS_DUMP"), *flag = genv("FPS_DUMP_FLIPS");
    char path[600];
    uint8_t hdr[54];
    uint32_t row = (uint32_t)w * 3, pad = (4 - (row & 3)) & 3;
    uint32_t filesz = 54 + (row + pad) * (uint32_t)h, uw = (uint32_t)w, uh = (uint32_t)h;
    uint8_t *line;
    int k;
    GLsizei x, y;

    if (!prefix)
        return;
    if (flag && !strcmp(flag, "cuts")) {
        if (!cut)
            return;
    } else if (flag && strcmp(flag, "1")) {
        FILE *fl = fopen(flag, "rb");
        if (!fl)
            return;
        fclose(fl);
    }
    line = (uint8_t *)malloc(row + pad);
    if (!line)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &uw, 4);
    memcpy(hdr + 22, &uh, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    for (k = 0; k < n; k++) {
        FILE *fo;
        snprintf(path, sizeof path, "%s%05u_%d.bmp", prefix, s_frame, k);
        fo = fopen(path, "wb");
        if (!fo)
            continue;
        fwrite(hdr, 1, sizeof hdr, fo);
        memset(line, 0, row + pad);
        for (y = h; y-- > 0; ) {
            const uint8_t *src = (const uint8_t *)px[k] + (size_t)y * pitch;
            for (x = 0; x < w; x++) {
                uint8_t *d = line + x * 3;
                if (bpp == 4) {
                    d[0] = src[x * 4];
                    d[1] = src[x * 4 + 1];
                    d[2] = src[x * 4 + 2];
                } else {
                    uint16_t v = (uint16_t)(src[x * 2] | src[x * 2 + 1] << 8);
                    d[0] = (uint8_t)((v & 0x1F) << 3);
                    d[1] = (uint8_t)(((v >> 5) & 0x3F) << 2);
                    d[2] = (uint8_t)(((v >> 11) & 0x1F) << 3);
                }
            }
            fwrite(line, 1, row + pad, fo);
        }
        fclose(fo);
    }
    free(line);
}

#if defined(VC3_GLES)
/* The Quest: the presented surface goes to the headset as it is drawn, a
 * texture handed over in GL (vc3_quest_frame), never read back. A surface
 * the CPU wrote (VC3's movie decoder) goes up from guest memory first. */
static void quest_present(void)
{
    static GLuint tex, fbo;
    static uint8_t *rgba;
    static size_t cap;
    Surf *f = surf_find(nv2a_backend_presented_va());
    int lw, lh, x, y;
    const uint8_t *mem;

    if (!f || !f->fbo)
        return;
    clear_flush(f, GL_COLOR_BUFFER_BIT);
    /* The host blits: the scissor and colour mask apply to blits too. */
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    s_rs_valid = 0;
    lw = f->wide ? ((f->w * 4 / 3 + 1) & ~1) : f->w;
    lh = f->h;
    if (!f->cpu_owned) {
        Vc3VrFrame vf;
        memset(&vf, 0, sizeof vf);
        vf.imm = s_imm && f->st;
        vf.view = s_view;
        if (vf.imm && s_hud_fbo && s_hud_frame == s_frame) {
            vf.hud_fbo = s_hud_fbo;
            vf.hud_w = s_hud_w;
            vf.hud_h = s_hud_h;
        }
        {   /* the HUD panel and the aim: the zoom's baseline as well */
            VrGameProj zg = zoom_proj(&s_gp);
            vf.gp[0] = zg.kx;
            vf.gp[1] = zg.ky;
        }
        vf.gp[2] = s_gp.cx;
        vf.gp[3] = s_gp.cy;
        vf.gp[4] = s_gp.inv_pz;
        vf.gp_ok = s_gp_ok;
        vf.gw = f->w;
        vf.gh = f->h;
        vc3_quest_frame(f->fbo, f->pw, f->ph, f->st ? 2 : 1, lw, lh, &vf);
        sh_reset();                     /* the host's calls, on this context */
        return;
    }
    /* One texture, reallocated only when the size or format changes, and
     * filled with glTexSubImage2D: a 16-bit surface goes up as it is (an
     * RGB565 texture, the row pitch given to GL), a 32-bit one has red and
     * blue swapped a word at a time -- the host blits from this texture, and
     * a blit ignores texture swizzles. */
    {
        static GLsizei tw, th;
        static GLenum tfmt;
        GLenum want = f->bpp == 4 ? GL_RGBA8 : GL_RGB565;
        mem = (const uint8_t *)xbox_GetMemoryOffset() + f->va;
        if (!tex) {
            glGenTextures(1, &tex);
            glGenFramebuffers(1, &fbo);
        }
        active_tex(0);
        glBindTexture(GL_TEXTURE_2D, tex);
        if (tw != f->w || th != f->h || tfmt != want) {
            glDeleteTextures(1, &tex);         /* immutable storage: a new one */
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexStorage2D(GL_TEXTURE_2D, 1, want, f->w, f->h);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            tw = f->w;
            th = f->h;
            tfmt = want;
        }
        if (f->bpp == 4) {
            if (cap < (size_t)f->w * f->h * 4) {
                free(rgba);
                cap = (size_t)f->w * f->h * 4;
                if (!(rgba = (uint8_t *)malloc(cap))) {
                    cap = 0;
                    return;
                }
            }
            for (y = 0; y < f->h; y++) {
                const uint32_t *src = (const uint32_t *)(mem + (size_t)y * f->pitch);
                uint32_t *d = (uint32_t *)rgba + (size_t)y * f->w;
                for (x = 0; x < f->w; x++) {    /* BGRX in memory -> RGBA */
                    uint32_t v = src[x];
                    d[x] = ((v >> 16) & 0xFFu) | (v & 0xFF00u) | ((v & 0xFFu) << 16)
                         | 0xFF000000u;
                }
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f->w, f->h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        } else {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(f->pitch / 2));
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f->w, f->h, GL_RGB,
                            GL_UNSIGNED_SHORT_5_6_5, mem);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    vc3_quest_frame(fbo, f->w, f->h, 1, f->w, f->h, NULL);
    sh_reset();
}
#endif

/* The next frame's scale (s_sx, s_sy, s_hud_sc), at the flip once the view
 * is latched (on: the host has an immersive view). The game's screen is the
 * presented surface's guest size (640x480 in VC3), a third wider at 16:9.
 *   immersive, a fixed detail (1x-4x): that, the HUD at 1x;
 *   immersive, native: each eye's picture (a screen-shaped surface, the
 *     whole field of view) exactly eye_w x eye_h, every other surface at the
 *     same scale; the HUD as fine as its panel is on the display -- a guest
 *     pixel of it spans 1/kx of a tangent, a display pixel (tan right - tan
 *     left) / eye_w -- in quarter steps, at most 4x;
 *   a headset's virtual screen: the game's picture the size it is on the
 *     display (vc3_vr_screen_px), in eighth steps, 1x-4x;
 *   otherwise the Resolution setting. */
static void scale_pick(int on)
{
    Surf *pf = surf_find(nv2a_backend_presented_va());
    float gw = 640.0f, gh = 480.0f, lw, sx, sy;
    int w, h;
    if (pf && pf->w * 3 == pf->h * 4) {
        gw = (float)pf->w;
        gh = (float)pf->h;
    }
    lw = s_wide ? gw * 4.0f / 3.0f : gw;
    s_hud_sc = 1.0f;
    if (on && s_view.scale >= 1 && s_view.scale <= VC3_SCALE_MAX) {
        s_sx = s_sy = (float)s_view.scale;
        return;
    }
    if (on && s_view.eye_w > 0 && s_view.eye_h > 0) {
        /* widened, the drawn width is the unwidened one's 4/3 (surf_alloc) */
        s_sx = (s_wide ? (float)s_view.eye_w * 0.75f : (float)s_view.eye_w) / gw;
        s_sy = (float)s_view.eye_h / gh;
        if (s_gp_ok) {
            VrGameProj zg = zoom_proj(&s_gp);
            float tx = s_view.tan_fov[0][1] - s_view.tan_fov[0][0];
            float ty = s_view.tan_fov[0][2] - s_view.tan_fov[0][3];
            if (tx > 0.0f && ty > 0.0f && fabsf(zg.kx) > 1.0f && fabsf(zg.ky) > 1.0f) {
                float hx = (float)s_view.eye_w / tx / fabsf(zg.kx);
                float hy = (float)s_view.eye_h / ty / fabsf(zg.ky);
                float hs = ceilf((hx > hy ? hx : hy) * 4.0f) / 4.0f;
                s_hud_sc = hs < 1.0f ? 1.0f : hs > 4.0f ? 4.0f : hs;
            }
        }
        return;
    }
    if (!on && vc3_vr_screen_px(&w, &h)) {
        sx = ceilf((float)w / lw * 8.0f) / 8.0f;
        sy = ceilf((float)h / gh * 8.0f) / 8.0f;
        s_sx = sx < 1.0f ? 1.0f : sx > 4.0f ? 4.0f : sx;
        s_sy = sy < 1.0f ? 1.0f : sy > 4.0f ? 4.0f : sy;
        return;
    }
    s_sx = s_sy = (float)vc3_scale_get();
}

static void gl_flip(void)
{
    if (s_fx.on)                    /* the effect target's run ends with the frame */
        fx_resolve();
    if (genv("GL_EFFECT_LOG") && s_fx.n_this)   /* VC3_GL_EFFECT_LOG=1 */
        fprintf(stderr, "[FX] frame %u: %u qualifying batches, %u runs so far\n", s_frame,
                s_fx.n_this, s_fx.runs);
    s_fx.n_prev = s_fx.n_this;
    s_fx.n_this = 0;
    int i;

    if (gl_ready()) {
        caps_off();                     /* the flip's own drawing has no clip planes */
        z_flip();
    }

    if (s_tm_on < 0)
        s_tm_on = genv("GL_TIMING") != NULL;
    if (s_tm_on > 0) {
        static uint64_t t0;
        uint64_t n = tm_now();
        if (!t0)
            t0 = n;
        if (n - t0 >= 1000000000ull) {
            fprintf(stderr, "[GLTIME] state %.0f ms | tex0 %.0f | rc+lmap %.0f | upload+attribs %.0f"
                    " | draw %.0f ms\n", s_tm_ns[0] / 1e6, s_tm_ns[1] / 1e6, s_tm_ns[2] / 1e6,
                    s_tm_ns[3] / 1e6, s_tm_ns[4] / 1e6);
            memset(s_tm_ns, 0, sizeof s_tm_ns);
            t0 = n;
        }
    }

    if (!gl_ready())
        return;
    GLCK("flip: before (elsewhere)", NULL, nv2a_backend_presented_va());
    if (s_ftr == 1) {
        s_ftr = 2;
        fprintf(stderr, "[GLTR] end of frame\n");
    } else if (s_ftr == 0 && genv("GL_FTRACE")) {
        FILE *fl = fopen(genv("GL_FTRACE"), "rb");
        if (fl) {
            fclose(fl);
            s_ftr = 1;
            fprintf(stderr, "[GLTR] frame begins\n");
        }
    }
#if !defined(VC3_GLES)
    /* The desktop's immersive test (vr_view_desktop.c) has no headset
     * layer to show the HUD picture on. It goes over each eye, whole and
     * blended as the headset's quad layer blends it (premultiplied alpha,
     * XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT): the HUD's
     * elements over the world, the rest transparent. It used to be copied,
     * opaque, into the right eye's corner -- a black panel there, as the
     * picture is clear (0,0,0,0) wherever there is no HUD.
     * VC3_IMM_HUD_INSET=1: that copy instead, to see the picture alone. */
    if (s_imm && s_hud_fbo && s_hud_frame == s_frame && genv("IMMERSIVE_TEST")) {
        Surf *pf = surf_find(nv2a_backend_presented_va());
        static GLuint prog;
        static GLint u_img, u_org, u_size;
        if (!prog && !(genv("IMM_HUD_INSET") && genv("IMM_HUD_INSET")[0] == '1')
            && (prog = link_quad(FS_HUD))) {
            u_img = glGetUniformLocation(prog, "u_img");
            u_org = glGetUniformLocation(prog, "u_org");
            u_size = glGetUniformLocation(prog, "u_size");
        }
        if (pf && pf->st && pf->fbo) {
            clear_flush(pf, GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
            glColorMask(1, 1, 1, 1);
            s_rs_valid = 0;
            if (prog) {
                int eye;
                glBindFramebuffer(GL_FRAMEBUFFER, pf->fbo);
                static GLuint vao;              /* no attributes: the quad is gl_VertexID */
                glUseProgram(prog);
                s_cur_prog = prog;
                if (!vao)
                    glGenVertexArrays(1, &vao);
                /* (not the CPU path's s_vao: its arrays 0-2 are enabled from
                 * the start and get a buffer only at that path's first draw,
                 * and with every 2D batch on the GPU that may not have come:
                 * GL_INVALID_OPERATION, nothing drawn) */
                glBindVertexArray(vao);
                s_cur_vao = vao;
                bind_tex(0, s_hud_tex);
                glUniform1i(u_img, 0);
                glUniform2f(u_size, (GLfloat)pf->pw, (GLfloat)pf->ph);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_STENCIL_TEST);
                glDisable(GL_CULL_FACE);
                caps_off();
                glEnable(GL_BLEND);
                glBlendEquation(GL_FUNC_ADD);
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                for (eye = 0; eye < 2; eye++) {
                    glViewport(eye * pf->pw, 0, pf->pw, pf->ph);
                    glUniform2f(u_org, (GLfloat)(eye * pf->pw), 0.0f);
                    glDrawArrays(GL_TRIANGLES, 0, 3);
                }
                glViewport(0, 0, pf->pw, pf->ph);
            } else {
                glBindFramebuffer(GL_READ_FRAMEBUFFER, s_hud_fbo);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, pf->fbo);
                glBlitFramebuffer(0, 0, s_hud_w, s_hud_h, pf->pw + pf->pw / 2, 0, 2 * pf->pw,
                                  pf->ph / 2, GL_COLOR_BUFFER_BIT, GL_LINEAR);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, pf->fbo);
            GLCK("flip: desktop HUD overlay", NULL, pf->va);
        }
    }
#endif
    /* Only the surface being presented goes back to guest memory: render
     * targets the title samples are read from their framebuffers on the GPU,
     * and reading every dirty surface back each flip, synchronously, was half
     * the GPU thread. VC3_GL_READBACK_ALL=1 restores that, for a title that
     * reads a render target with the CPU. */
    {
        static int all = -1;
        uint32_t shown = nv2a_backend_presented_va();
        if (all < 0)
            all = genv("GL_READBACK_ALL") != NULL;
        for (i = 0; i < s_nsurfs; i++) {
            if (!s_surfs[i].dirty)
                continue;
            if (shown && s_surfs[i].va == shown && !all) {
#if defined(VC3_GLES)
                /* The Quest shows the drawn surface itself (quest_present):
                 * guest memory gets the frame only if VC3_GL_READBACK=1.
                 * Reading it back every flip cost a resolve and, once the
                 * GPU ran a frame behind, a wait on the mapped buffer that
                 * held 3D at 2x to 30 fps or less. */
                static int rb = -1;
                if (rb < 0)
                    rb = genv("GL_READBACK") && genv("GL_READBACK")[0] == '1';
                if (!rb) {
                    s_surfs[i].dirty = 0;
                    continue;
                }
#endif
                surf_readback_async(&s_surfs[i]);
                GLCK("flip: readback of the shown surface", NULL, s_surfs[i].va);
            }
            else if (all || !shown) {
                surf_readback(&s_surfs[i]);
                GLCK("flip: readback (other surface)", NULL, s_surfs[i].va);
            }
        }
    }
    gt_flip();
    GLCK("flip: GPU timing queries", NULL, 0);
    /* The frame's projection (the HUD panel, the aim): the one of the
     * stereo surface the most 3D went into -- the scene. */
    {
        uint32_t best = 0;
        for (i = 0; i < s_nsurfs; i++) {
            Surf *q = &s_surfs[i];
            if (q->st && q->gp_ok && q->n3d > best) {
                best = q->n3d;
                s_gp = q->gp;
                s_gp_ok = 1;
            }
            q->n3d = 0;
        }
        if (best)
            zoom_note(s_gp.kx);
        s_3d_prev = s_3d_this;
        s_3d_this = 0;
    }
#if defined(VC3_GLES)
    gt_surface(GT_PRESENT);             /* the copy to the headset, timed apart */
    quest_present();
    GLCK("flip: quest_present", NULL, 0);
#endif
    /* The picture the window shows: the presented surface's guest size,
     * widened as it is drawn. Then the next frame's scale and aspect. */
    {
        Surf *pf = surf_find(nv2a_backend_presented_va());
        if (pf)
            xbox_FramebufferWindowSetPicture(
                (uint32_t)(pf->wide ? (pf->w * 4 / 3 + 1) & ~1 : pf->w) * (pf->st ? 2 : 1),
                (uint32_t)pf->h);
    }
    s_wide = vc3_wide_get();
    /* The next frame's view: immersive when the host has a pose for it and
     * this frame had 3D in it (a frame without -- a movie, a menu -- goes
     * on the virtual screen). Immersive draws both eyes, as stereo does. */
    {
        uint32_t shown = nv2a_backend_presented_va();
        int on;
        int k;
        for (k = 0; k < s_nshown && shown != s_shown_va[k]; k++)
            ;
        if (shown && k == s_nshown) {
            memmove(&s_shown_va[1], &s_shown_va[0], (size_t)(s_nshown - 1) * sizeof s_shown_va[0]);
            s_shown_va[0] = shown;
        }

        on = vc3_vr_view_latch(&s_view);
        scale_pick(on);
        s_stereo = vc3_stereo_get() || on;
        s_imm = on && s_stereo && s_3d_prev > 0 && s_gp_ok;
        fov_frame();
        if (genv("IMM_LOG")) {
            static uint32_t n;
            if (++n % 60 == 0)
                fprintf(stderr, "[IMM] %s: 3D batches %u, projection %s (kx %.1f ky %.1f cx %.1f"
                        " cy %.1f 1/pz %.3f), zoom %.2f (baseline %.1f), batches read %u,"
                        " not perspective %u\n",
                        s_imm ? "immersive" : on ? "screen (no 3D)" : "off", s_3d_prev,
                        s_gp_ok ? "known" : "unknown", s_gp.kx, s_gp.ky, s_gp.cx, s_gp.cy,
                        s_gp.inv_pz, s_kbase > 0 ? fabsf(s_gp.kx) / s_kbase : 1.0f, s_kbase,
                        s_gp_good, s_gp_bad);
        }
    }
    GLCK("flip: end", NULL, 0);
    glck_second();
    s_frame++;
    if (genv("FPS_LOG")) {            /* wall-clock flips per second */
        /* With the vblanks delivered over the same second: flips below the
         * vblank rate are frames that missed one. */
        extern uint32_t xbox_VblankCount(void);
        static struct timespec t0;
        static uint32_t f0, v0;
        static uint64_t p0, m0;
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (!t0.tv_sec) {
            t0 = t;
            f0 = s_frame;
            v0 = xbox_VblankCount();
            p0 = xbox_FramebufferWindowPresentCount();
            m0 = s_ip.made;
        } else if (t.tv_sec - t0.tv_sec >= 1) {
            double dt = (double)(t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
            uint32_t v = xbox_VblankCount();
            uint64_t p = xbox_FramebufferWindowPresentCount();
            int fps = vc3_fps_get();
            /* With the window up, what it put on screen; above 60 fps, the
             * in-between frames made (headless shows nothing). */
            fprintf(stderr, "[FPS] %.1f vblank %.1f", (s_frame - f0) / dt, (v - v0) / dt);
            if (xbox_FramebufferWindowOpen())
                fprintf(stderr, " shown %.1f", (double)(p - p0) / dt);
            if (fps > 60)
                fprintf(stderr, " made %.1f (display %d)", (double)(s_ip.made - m0) / dt, fps);
            fprintf(stderr, "\n");
            t0 = t;
            f0 = s_frame;
            v0 = v;
            p0 = p;
            m0 = s_ip.made;
        }
    }
    texstat_flip(s_frame);
    if (s_st.on && s_frame % 300 == 0) {
        fprintf(stderr, "[GL] %u flips: %u draws (%u tris), %u textured, %u "
                "no texture, %u from surfaces, %u uploads (%u mipmapped), %u binds "
                "without a hash; surfaces:",
                300u, s_st.draws, s_st.tris, s_st.textured, s_st.untex,
                s_st.rt, s_st.uploads, s_st.mipped, s_st.qskip);
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

/* Whether the flip writes nothing into guest memory for the executor's
 * present: the Quest shows the drawn frame itself and reads nothing back
 * (unless VC3_GL_READBACK=1 or VC3_GL_READBACK_ALL); the Mac's window shows
 * guest memory. Then the decode thread need not wait for the flip. */
static int gl_flip_async(void)
{
#if defined(VC3_GLES)
    /* on the decode thread: ch_env, not genv (whose cache is the GL thread's) */
    const char *rb = ch_env("GL_READBACK");
    return !(rb && rb[0] == '1') && !ch_env("GL_READBACK_ALL");
#else
    /* VC3_GL_FLIP_ASYNC=1: the same on the Mac, to measure the overlap (the
     * window then shows guest memory as the readback left it: a frame late) */
    const char *e = ch_env("GL_FLIP_ASYNC");
    return e && e[0] == '1';
#endif
}

static const Nv2aBackend s_gl_backend = { gl_clear, gl_draw, gl_flip, gl_draw_raw, gl_flip_async };

/* Register unless VC3_GL=0 (the executor's software rasteriser then draws). */
void vc3_gl_backend_register(void)
{
    const char *e = genv("GL");

    if (e && e[0] == '0') {
        fprintf(stderr, "[GL] off (VC3_GL=0): software rasteriser\n");
        return;
    }
    s_st.on = genv("GL_STATS") != NULL;
    texstat_init();
    s_sx = s_sy = (float)vc3_scale_get();
    s_wide = vc3_wide_get();
    s_stereo = vc3_stereo_get();
    {
        const ChihiroProfile *gp = chihiro_profile();
        s_hud_place = gp->gpu.hud_place;
        if (gp->gpu.picture_min_w > 0)
            s_pic_min_w = gp->gpu.picture_min_w;
        s_lazy_rt = gp->gpu.lazy_rt >= 0;
        if (gp->gpu.stereo_depth > 0.0f)
            s_st_depth = gp->gpu.stereo_depth;
        if (gp->gpu.stereo_sep > 0.0f)
            s_st_sep = gp->gpu.stereo_sep;
        if (gp->gpu.zoom_tolerance > 0.0f)
            s_zoom_tol = gp->gpu.zoom_tolerance;
        if (gp->gpu.buffers > 0)
            s_nshown = gp->gpu.buffers < SHOWN_MAX ? gp->gpu.buffers : SHOWN_MAX;
    }
    if (genv("STEREO_SEP"))
        s_st_sep = (float)atof(genv("STEREO_SEP"));
    if (genv("STEREO_DEPTH"))
        s_st_depth = (float)atof(genv("STEREO_DEPTH"));
    e = genv("GL_COMBINERS");
    s_use_rc = e && e[0] == '1';
    nv2a_backend_register(&s_gl_backend);
    fprintf(stderr, "[GL] OpenGL back end registered\n");
}
