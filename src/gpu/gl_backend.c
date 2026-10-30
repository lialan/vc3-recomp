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
static GLuint s_vao, s_vao_raw, s_white;

/* A linked program and its uniform locations, looked up once at link time:
 * glGetUniformLocation per draw was a string search per call. The fragment
 * shader is shared, so every program has the fragment uniforms; vertex
 * uniforms a program lacks read -1, which GL ignores. */
typedef struct {
    GLuint prog;
    GLint surf, depth, texsize, textured, atest, afunc, aref;
    GLint rc, cicw, aicw, cocw, aocw, f0, f1, fin0, fin1, ctl, sprog, fc0, fc1,
          fogc, on, ts, iscube, dbgon, dbgcol, lmap, rcdbg;
    /* The fixed-function vertex shader (Nv2aFixedState). */
    GLint comp, mv, proj, tm, c59, skin, usecomp, lighting, lkind, light, samb,
          matalpha, colmat, flipn, hasnrm, hascol, texgen, tgon, tmon, ascale,
          packed, aa, vpz, nearz, fogen, fogmode, fogp, uvs0;
} Prog;
static Prog s_pcpu, s_praw, *U = &s_pcpu;
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
    "uniform mat4 u_comp, u_mv[4], u_proj, u_tm[4];\n"   /* uploaded transposed */
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
    "  vec2 ndc = s.xy / u_surf * 2.0 - 1.0;\n"
    "  float zn = (s.z - u_depth.x) / (u_depth.y - u_depth.x) * 2.0 - 1.0;\n"
    "  bool bad = abs(w) <= 1e-6 || any(isnan(o_screen)) || any(isinf(o_screen));\n"
    "  gl_Position = bad ? vec4(0.0, 0.0, 0.0, 1.0) : vec4(ndc * w, zn * w, w);\n"
    "  gl_ClipDistance[0] = bad ? -1e30 : w - 1e-3;\n"
    "  gl_ClipDistance[1] = bad ? -1e30 : (u_nearz != 0 ? (s.z - u_vpz) * w : 1.0);\n"
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
                genv("VC3_GL_ADBG") ? atoi(genv("VC3_GL_ADBG")) : 0);
    glUniform1i(glGetUniformLocation(p->prog, "u_nofog"),
                genv("VC3_GL_NOFOG") != NULL);
    glUniform1i(p->rcdbg, genv("VC3_GL_RCDBG") ? atoi(genv("VC3_GL_RCDBG")) : 0);
    return 1;
}

/* ── Streaming buffers ─────────────────────────────────────── */

/* Vertex and index data stream through two large buffers, appended to and
 * orphaned when full, instead of a glBufferData per draw: a fresh allocation
 * for every one of ~30000 draws a second. Writes are unsynchronised -- a
 * range is never written twice before the orphan that ends its use. GL 4.1
 * on macOS has no persistent mapping (ARB_buffer_storage). */
typedef struct {
    GLuint buf;
    GLenum target;
    GLsizeiptr size, at;
} Ring;
static Ring s_vring = { 0, GL_ARRAY_BUFFER, 16 << 20, 0 };
static Ring s_iring = { 0, GL_ELEMENT_ARRAY_BUFFER, 4 << 20, 0 };

/* Map `bytes` at a 16-byte boundary; the buffer stays bound to its target.
 * NULL if bytes cannot fit even an empty buffer. */
static void *ring_map(Ring *r, GLsizeiptr bytes, GLsizeiptr *offset)
{
    GLsizeiptr at = (r->at + 15) & ~(GLsizeiptr)15;

    if (bytes > r->size)
        return NULL;
    if (!r->buf)
        glGenBuffers(1, &r->buf);
    glBindBuffer(r->target, r->buf);
    if (!r->at || at + bytes > r->size) {
        glBufferData(r->target, r->size, NULL, GL_STREAM_DRAW);
        at = 0;
    }
    *offset = at;
    r->at = at + bytes;
    return glMapBufferRange(r->target, at, bytes, GL_MAP_WRITE_BIT
                            | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
}

static GLsizeiptr ring_put(Ring *r, const void *data, GLsizeiptr bytes)
{
    GLsizeiptr off = 0;
    void *p = ring_map(r, bytes, &off);

    if (!p)
        return -1;
    memcpy(p, data, (size_t)bytes);
    glUnmapBuffer(r->target);
    return off;
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
        fprintf(stderr, "[GL] %s, %s\n", glGetString(GL_RENDERER),
                glGetString(GL_VERSION));

        fs = compile(GL_FRAGMENT_SHADER, FS);
        if (!prog_link(&s_pcpu, compile(GL_VERTEX_SHADER, VS), fs, 0)) {
            fprintf(stderr, "[GL] program link failed; back end off\n");
            s_failed = 1;
            return 0;
        }
        /* Without it every batch is transformed on the CPU, as before. */
        if (!prog_link(&s_praw, compile(GL_VERTEX_SHADER, VS_FF), fs, 1))
            fprintf(stderr, "[GL] fixed-function shader failed; raw draws declined\n");

        /* The CPU path's layout; pointers are set per draw, as each batch
         * lands at a different place in the stream. */
        glGenVertexArrays(1, &s_vao);
        glBindVertexArray(s_vao);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glGenVertexArrays(1, &s_vao_raw);

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

/* Sampling state lives in sampler objects, one per (filter, wrap) pair the
 * titles use, bound per unit: setting filter and wrap on the texture at
 * every bind -- five glTexParameteri per draw -- made the Metal-backed driver
 * rebuild its sampler and texture state for nearly every draw. The alpha
 * swizzle is a texture parameter and is set only when it changes. */
static int s_unit;
static GLuint s_unit_sampler[8];

static void active_tex(int unit)
{
    if (unit != s_unit) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
        s_unit = unit;
    }
}

static GLint gl_wrap(uint32_t mode);

static void tex_sampling(GLuint tex, const Nv2aTexture *t)
{
    static GLuint samplers[2][2][3][3];
    static GLint swz[16384];               /* by texture name; 0: unknown */
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    GLint wu = gl_wrap(t->addr_u), wv = gl_wrap(t->addr_v);
    int iu = wu == GL_REPEAT ? 0 : wu == GL_MIRRORED_REPEAT ? 1 : 2;
    int iv = wv == GL_REPEAT ? 0 : wv == GL_MIRRORED_REPEAT ? 1 : 2;
    GLuint *sm = &samplers[mag == 1][min == 1][iu][iv];
    GLint a = fmt_has_alpha(t->color) ? GL_ALPHA : GL_ONE;

    if (!*sm) {
        glGenSamplers(1, sm);
        /* SET_TEXTURE_FILTER: 1 is point sampling, anything else filters. */
        glSamplerParameteri(*sm, GL_TEXTURE_MAG_FILTER, mag == 1 ? GL_NEAREST : GL_LINEAR);
        glSamplerParameteri(*sm, GL_TEXTURE_MIN_FILTER, min == 1 ? GL_NEAREST : GL_LINEAR);
        glSamplerParameteri(*sm, GL_TEXTURE_WRAP_S, wu);
        glSamplerParameteri(*sm, GL_TEXTURE_WRAP_T, wv);
    }
    if (s_unit_sampler[s_unit] != *sm) {
        glBindSampler((GLuint)s_unit, *sm);
        s_unit_sampler[s_unit] = *sm;
    }
    if (tex >= 16384 || swz[tex] != a) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, a);
        if (tex < 16384)
            swz[tex] = a;
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
        glBindTexture(GL_TEXTURE_2D, rt->color);
        bound = rt->color;
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
        bound = e->tex;
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
    tex_sampling(bound, t);
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
    if (s_self_w != f->w || s_self_h != f->h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, f->w, f->h, 0, GL_BGRA,
                     GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
        s_self_w = f->w;
        s_self_h = f->h;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, f->fbo);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, f->w, f->h);
    tex_sampling(s_self_tex, t);
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
        const char *e = genv("VC3_GL_CULL");
        cull_mode = e ? atoi(e) : 0;
    }
    if (s_rs_valid && !memcmp(&s_rs_last, rs, sizeof *rs))
        goto uniforms;
    s_rs_last = *rs;
    s_rs_valid = 1;
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
uniforms:
    glUniform1i(U->atest, rs->alpha_test_enable && !genv("VC3_GL_NOATEST") ? 1 : 0);
    glUniform1i(U->afunc, (GLint)rs->alpha_func);
    glUniform1f(U->aref, (float)(rs->alpha_ref & 0xFF));
    {
        float dmin = rs->depth_min, dmax = rs->depth_max;
        if (!(dmax > dmin)) {
            dmin = 0.0f;
            dmax = 16777215.0f;
        }
        glUniform2f(U->depth, dmin, dmax);
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
    s_rs_valid = 0;                    /* masks change below */
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
    glUniform1i(U->rc, 1);
    glUniform1uiv(U->cicw, 8, rc->color_icw);
    glUniform1uiv(U->aicw, 8, rc->alpha_icw);
    glUniform1uiv(U->cocw, 8, rc->color_ocw);
    glUniform1uiv(U->aocw, 8, rc->alpha_ocw);
    glUniform4fv(U->f0, 8, f0);
    glUniform4fv(U->f1, 8, f1);
    glUniform1ui(U->fin0, rc->final0);
    glUniform1ui(U->fin1, rc->final1);
    glUniform1ui(U->ctl, rc->control);
    glUniform1ui(U->sprog, rc->stage_program);
    nv2a_rc_unpack(rc->final_c0, c);
    glUniform4fv(U->fc0, 1, c);
    nv2a_rc_unpack(rc->final_c1, c);
    glUniform4fv(U->fc1, 1, c);
    /* FOG_COLOR is R in bits 0-7, the reverse of a D3DCOLOR. */
    nv2a_rc_unpack(b->fog_color, c);
    glUniform3f(U->fogc, c[2], c[1], c[0]);
    glUniform1iv(U->on, 4, on);
    glUniform1iv(U->iscube, 4, cube);
    glUniform1i(U->dbgon,
                !(b->stage[0] && surf_find(b->stage[0]->offset)));
    glUniform2fv(U->ts, 4, ts);
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
                    rc->color_icw[1], rc->color_ocw[1], b->ext ? b->ext[0].specular : 0,
                    b->stage[0] ? b->stage[0]->offset : 0,
                    b->stage[1] ? b->stage[1]->offset : 0);
        }
        glUniform3f(U->dbgcol, r, g, bl);
        if (genv("VC3_GL_RCDBG") && atoi(genv("VC3_GL_RCDBG")) >= 6)
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

/* Upload each attribute stream min..max once, at its own stride, and point
 * the roles at it. Attributes interleaved in one vertex (offsets within a
 * stride of each other) share one copy. Returns 0 if they cannot fit. */
static int raw_attribs(const Nv2aRawGeometry *g, const int slot[8])
{
    struct { const uint8_t *base; uint32_t stride, bytes; GLsizeiptr off; } grp[8];
    int ngrp = 0, gi[8], r, k;
    GLfloat scale[8][4];
    GLint packed = 0;
    GLsizeiptr total = 0, off = 0;
    uint8_t *dst;

    for (r = 0; r < 8; r++) {
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
    if (total) {
        if (!(dst = (uint8_t *)ring_map(&s_vring, total, &off)))
            return 0;
        for (k = 0; k < ngrp; k++) {
            memcpy(dst + grp[k].off, grp[k].base, grp[k].bytes);
            grp[k].off += off;
        }
        glUnmapBuffer(GL_ARRAY_BUFFER);
    }
    for (r = 0; r < 8; r++) {
        const Nv2aAttrib *a = slot[r] >= 0 ? &g->attr[slot[r]] : NULL;
        GLuint loc = (GLuint)r;
        int c;

        for (c = 0; c < 4; c++)
            scale[r][c] = 1.0f;
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
            case 0:  glVertexAttribPointer(loc, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, stride, at); break;
            case 4:  glVertexAttribPointer(loc, n, GL_UNSIGNED_BYTE, GL_TRUE, stride, at); break;
            case 1:
                for (c = 0; c < n; c++)
                    scale[r][c] = 1.0f / 32767.0f;
                /* fall through */
            case 5:  glVertexAttribPointer(loc, n, GL_SHORT, GL_FALSE, stride, at); break;
            case 6:
                glVertexAttribPointer(loc, 2, GL_UNSIGNED_SHORT, GL_FALSE, stride, at);
                packed |= 1 << r;
                break;
            default: glVertexAttribPointer(loc, n, GL_FLOAT, GL_FALSE, stride, at); break;
            }
            glEnableVertexAttribArray(loc);
        }
    }
    glUniform4fv(U->ascale, 8, &scale[0][0]);
    glUniform1i(U->packed, packed);
    return 1;
}

/* The fixed-function state, re-sent only when it changed: consecutive
 * batches of one mesh share nearly all of it. */
static void raw_uniforms(const Nv2aRawGeometry *g)
{
    static Nv2aFixedState last;
    static int valid;
    const Nv2aFixedState *ff = g->ff;
    GLint lk[8], tg[4], tm[4], i;
    GLuint gen[16];

    glUniform2f(U->aa, g->aa_sx, g->aa_sy);
    glUniform1f(U->vpz, g->vp_off_z);
    glUniform1i(U->nearz, (GLint)g->near_z);
    glUniform1i(U->fogen, (GLint)g->fog_enable);
    glUniform1ui(U->fogmode, g->fog_mode);
    glUniform2f(U->fogp, g->fog_param[0], g->fog_param[1]);
    glUniform2f(U->uvs0, g->uv_scale0[0], g->uv_scale0[1]);
    if (valid && !memcmp(&last, ff, sizeof last))
        return;
    last = *ff;
    valid = 1;
    glUniformMatrix4fv(U->comp, 1, GL_TRUE, ff->composite);
    glUniformMatrix4fv(U->mv, 4, GL_TRUE, &ff->mv[0][0]);
    glUniformMatrix4fv(U->proj, 1, GL_TRUE, ff->proj);
    glUniformMatrix4fv(U->tm, 4, GL_TRUE, &ff->texmat[0][0]);
    glUniform4fv(U->c59, 1, ff->c59);
    glUniform1ui(U->skin, ff->skin_mode);
    glUniform1i(U->usecomp, (GLint)ff->use_composite);
    glUniform1i(U->lighting, (GLint)ff->lighting);
    glUniform2i(U->colmat, (GLint)ff->colmat_amb, (GLint)ff->colmat_dif);
    glUniform1i(U->flipn, (GLint)ff->flip_normals);
    glUniform1i(U->hasnrm, (GLint)ff->has_normal);
    glUniform1i(U->hascol, (GLint)ff->has_color);
    glUniform4fv(U->samb, 1, ff->scene_amb);
    glUniform1f(U->matalpha, ff->mat_alpha);
    for (i = 0; i < 8; i++)
        lk[i] = (GLint)ff->light_kind[i];
    glUniform1iv(U->lkind, 8, lk);
    glUniform4fv(U->light, 40, &ff->light[0][0]);
    for (i = 0; i < 4; i++) {
        tg[i] = (GLint)ff->texgen_on[i];
        tm[i] = (GLint)ff->texmat_on[i];
    }
    memcpy(gen, ff->texgen, sizeof gen);
    glUniform4uiv(U->texgen, 4, gen);
    glUniform1iv(U->tgon, 4, tg);
    glUniform1iv(U->tmon, 4, tm);
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
    int slot[8];

    raw_roles(g, slot);
    if (!raw_attribs(g, slot))
        return 0;
    raw_uniforms(g);
    *ioff = ring_put(&s_iring, g->indices, (GLsizeiptr)g->index_count * 2);
    return *ioff >= 0;
}

/* VC3_GL_STATS: raw draws and why some were declined. */
static struct { uint32_t draws, tris, declined; } s_rst;

static void gl_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aRawGeometry *raw = b->raw;
    Surf *f;
    float tw = 1.0f, th = 1.0f;
    int textured = 0;
    uint32_t tris = raw ? (raw->prim == 5 ? raw->index_count / 3 : raw->index_count - 2)
                        : b->count / 3;
    GLsizeiptr ioff = 0;

    if ((!raw && !b->count) || !gl_ready() || !(f = surf_bind(s)))
        return;
    U = raw ? &s_praw : &s_pcpu;
    {
        /* Consecutive batches mostly share program and layout. */
        static GLuint cur_prog, cur_vao;
        GLuint vao = raw ? s_vao_raw : s_vao;
        if (cur_prog != U->prog) {
            glUseProgram(U->prog);
            cur_prog = U->prog;
        }
        if (cur_vao != vao) {
            glBindVertexArray(vao);
            cur_vao = vao;
        }
    }
    apply_state(b->state);
    glUniform2f(U->surf, (float)f->w, (float)f->h);
    active_tex(0);
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
    glUniform1i(U->textured, textured);
    glUniform2f(U->texsize, tw, th);
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
    if (!raw && genv("VC3_GL_RCCHECK"))
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
        int has_cube = 0, need_ext = 0;
        if (has_ext && genv("VC3_GL_CUBE_RC")) {
            int st;
            for (st = 0; st < 4; st++)
                if (b->stage[st] && b->stage[st]->cube
                    && ((b->rc->stage_program >> (st * 5)) & 0x1F) == 3)
                    has_cube = 1;
        }
        if (has_ext && (s_use_rc || has_cube)) {
            bind_combiners(f, b);
            need_ext = 1;
        } else {
            int lmap = has_ext && lightmap_batch(b);
            glUniform1i(U->rc, 0);
            glUniform1i(U->lmap, 0);
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
                    glUniform2fv(U->ts, 4, ts);
                    glUniform1ui(U->sprog, b->rc->stage_program);
                    glUniform1i(U->lmap, 1);
                    need_ext = 1;
                }
                active_tex(0);
            }
        }
        if (raw) {
            if (!raw_submit(raw, &ioff)) {
                s_rst.declined++;
                return;
            }
        } else {
            GLuint k;
            GLsizeiptr off;
            if (need_ext)
                bind_ext(b);
            else
                for (k = 3; k <= 8; k++)
                    glDisableVertexAttribArray(k);
            off = ring_put(&s_vring, b->vertices, (GLsizeiptr)(b->count * sizeof(Nv2aVertex)));
            if (off < 0)
                return;
            glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Nv2aVertex),
                                  (void *)(off + offsetof(Nv2aVertex, x)));
            glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Nv2aVertex),
                                  (void *)(off + offsetof(Nv2aVertex, diffuse)));
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Nv2aVertex),
                                  (void *)(off + offsetof(Nv2aVertex, u)));
        }
    }
    if (raw) {
        if (raw->check_out)
            raw_capture(raw);
        glEnable(GL_CLIP_DISTANCE0);
        glEnable(GL_CLIP_DISTANCE1);
        glEnable(GL_DEPTH_CLAMP);
        s_rst.draws++;
        s_rst.tris += tris;
    }
    if (b->zpass_out) {
        /* A visibility test: count the pixels that pass, for GET_REPORT.
         * Reading the result waits for the GPU, but only test batches pay. */
        static GLuint q;
        GLuint passed = 0;
        if (!q)
            glGenQueries(1, &q);
        glBeginQuery(GL_SAMPLES_PASSED, q);
        if (raw)
            glDrawElements(raw_mode(raw->prim), (GLsizei)raw->index_count,
                           GL_UNSIGNED_SHORT, (void *)ioff);
        else
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)b->count);
        glEndQuery(GL_SAMPLES_PASSED);
        glGetQueryObjectuiv(q, GL_QUERY_RESULT, &passed);
        *b->zpass_out += passed;
        s_st.zpass_batches++;
        s_st.zpass_pixels += passed;
    } else if (raw) {
        glDrawElements(raw_mode(raw->prim), (GLsizei)raw->index_count,
                       GL_UNSIGNED_SHORT, (void *)ioff);
    } else {
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)b->count);
    }
    if (raw) {
        glDisable(GL_CLIP_DISTANCE0);
        glDisable(GL_CLIP_DISTANCE1);
        glDisable(GL_DEPTH_CLAMP);
    }
    if (!raw && genv("VC3_GL_RCCHECK") && b->rc && b->ext && s_use_rc
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
            glUniform1i(loc, genv("VC3_GL_RCDBG") ? atoi(genv("VC3_GL_RCDBG")) : 0);
            fprintf(stderr, "[RCGPU] at %.0f,%.0f: v_col.a %u on0 %u on1 %u | comb.a %u t0.a %u t1.a %u"
                    " | diffuse[0] %08X\n", cx, cy, px13[0], px13[1], px13[2],
                    px12[0], px12[1], px12[2], b->vertices[0].diffuse);
        }
    }
    active_tex(0);
    f->dirty = 1;
    f->memq_valid = 0;
    f->cpu_owned = 0;
}

/* Nv2aBackend.draw_raw: fixed-function batches, when the shader linked and
 * the batch fits the streams. Anything else goes back to the CPU path. */
static int gl_draw_raw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aRawGeometry *g = b->raw;
    uint32_t r;

    if (!g || g->kind != NV2A_RAW_FIXED || !g->ff || g->index_count < 3
        || !gl_ready() || !s_praw.prog)
        return 0;
    for (r = 0; r < 16; r++)
        if (g->attr[r].data && g->attr[r].bytes > (uint32_t)(s_vring.size / 8))
            return 0;
    if ((GLsizeiptr)g->index_count * 2 > s_iring.size)
        return 0;
    gl_draw(s, b);
    return 1;
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

static const Nv2aBackend s_gl_backend = { gl_clear, gl_draw, gl_flip, gl_draw_raw };

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
