/*
 * vr_shot.h - where the rifle's shot meets the scene, from the immersive
 * view's depth (vk_immersive.c), for the dot the headset draws there.
 *
 * The game's shot leaves its camera -- the origin of the GL view space the
 * eyes are drawn from (vr_view.h) -- along the aim (yaw + right, pitch + up,
 * as ChVrRifle). Seen from an eye, the ray's points lie on a line in the
 * eye's picture that runs out to the aim's vanishing point. The renderer
 * copies the eye's depth along that line (a box round it, never more than
 * VR_SHOT_SPAN texels of it) and walks it from the near end: the first
 * texel where the ray is behind what was drawn there is where the shot
 * lands. The eye sits close to the camera, so the line is short, and the
 * whole of the ray beyond a few metres is within a few texels of the
 * vanishing point.
 *
 * Along the line, with g = 1 / (the point's depth in front of the eye, its
 * clip w, metres), the texel is v + c g (x, y: of the side-by-side target,
 * y down), its GL depth (clip z / w) v[2] + c[2] g, and its distance along
 * the ray t = (1 / g - bw) / aw game units: all linear in g, which is 0 at
 * the vanishing point.
 *
 * Header-only, as vr_math.h: the renderer and tests/vr_shot_test.c.
 */
#ifndef VC3_VR_SHOT_H
#define VC3_VR_SHOT_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* The walk starts this far in front of the eye (metres: the rifle in the
 * hands is no target) and reads at most VR_SHOT_SPAN texels of the line,
 * the ones toward the vanishing point: on the Quest (~1100 texels a radian,
 * the eye ~0.1 m off the camera) everything beyond ~1 m. */
#define VR_SHOT_NEAR 0.5f
#define VR_SHOT_SPAN 128
/* The box the walk reads: at most this many texels each way. */
#define VR_SHOT_BOX (VR_SHOT_SPAN + 2)

typedef struct {
    int32_t x0, y0;             /* the box of the target the walk reads: x0, y0, w x h texels */
    uint32_t w, h;
    float v[3], c[3];           /* texel x, y and GL depth: v + c g */
    float g_hi, g_lo;           /* the walk: from g_hi (its near end) to g_lo (its far end) */
    float aw, bw;               /* t = (1 / g - bw) / aw */
    uint32_t steps;             /* texels along it (about one each) */
} VrShotPlan;

/* The aim's direction in GL view space (unit). */
static inline void vr_shot_dir(float yaw, float pitch, float d[3])
{
    d[0] = sinf(yaw) * cosf(pitch);
    d[1] = sinf(pitch);
    d[2] = -cosf(yaw) * cosf(pitch);
}

/* The line of the ray from o along d (GL view space, game units; o NULL:
 * the game camera) in eye 0's picture -- vp its matrix, the picture k of
 * eye_w x eye_h from the target's top-left: p's v, c, aw, bw, and the part
 * of the walk in the picture, g from *g_hi down to *g_lo; hi: the
 * picture's last texel coordinates. 0: no texel of it there. */
static inline int vr_shot_line(const float vp[16], float k, uint32_t eye_w, uint32_t eye_h, const float o[3],
                               const float d[3], VrShotPlan *p, double *g_hi_out, double *g_lo_out, double hi[2])
{
    float a[4], b[4], V[3], C[3];
    double g_hi, g_lo, lo[2];
    int i;
    for (i = 0; i < 4; i++) {
        a[i] = vp[i] * d[0] + vp[4 + i] * d[1] + vp[8 + i] * d[2];
        b[i] = o ? vp[i] * o[0] + vp[4 + i] * o[1] + vp[8 + i] * o[2] + vp[12 + i] : vp[12 + i];
    }
    if (!(a[3] > 1e-6f))
        return 0;                       /* aimed away from the eye's forward half: no vanishing point */
    for (i = 0; i < 3; i++) {
        V[i] = a[i] / a[3];
        C[i] = b[i] - b[3] * V[i];
    }
    if (!(C[2] < 0.0f))
        return 0;                       /* not a GL perspective (depth grows with g) */
    /* into the target's texels: x right from eye 0's left edge, y down */
    {
        const float sx = 0.5f * k * (float)eye_w, sy = 0.5f * k * (float)eye_h;
        p->v[0] = (V[0] + 1.0f) * sx;
        p->v[1] = (1.0f - V[1]) * sy;
        p->v[2] = V[2];
        p->c[0] = C[0] * sx;
        p->c[1] = -C[1] * sy;
        p->c[2] = C[2];
    }
    p->aw = a[3];
    p->bw = b[3];
    /* near: VR_SHOT_NEAR in front of the eye and not behind the camera
     * (t >= 0); far: inside the far plane (GL depth below 1), where a
     * cleared texel is never met */
    g_hi = 1.0 / (b[3] > VR_SHOT_NEAR ? b[3] : VR_SHOT_NEAR);
    g_lo = V[2] > 1.0f - 1e-5f ? (V[2] - (1.0 - 1e-5)) / -C[2] : 0.0;
    if (g_lo < 0.0)
        g_lo = 0.0;
    /* the part in the eye's picture: x(g), y(g) within it */
    lo[0] = lo[1] = 0.0;
    hi[0] = k * (double)eye_w - 1e-3;
    hi[1] = k * (double)eye_h - 1e-3;
    for (i = 0; i < 2; i++) {
        const double v = p->v[i], c = p->c[i];
        if (fabs(c) < 1e-12) {
            if (v < lo[i] || v > hi[i])
                return 0;
            continue;
        }
        {
            const double e0 = (lo[i] - v) / c, e1 = (hi[i] - v) / c;
            const double gmin = e0 < e1 ? e0 : e1, gmax = e0 < e1 ? e1 : e0;
            if (gmin > g_lo)
                g_lo = gmin;
            if (gmax < g_hi)
                g_hi = gmax;
        }
    }
    if (!(g_lo <= g_hi))
        return 0;
    *g_hi_out = g_hi;
    *g_lo_out = g_lo;
    return 1;
}

/* The box of the line's texels from g_a to g_b, clamped to the picture
 * (hi: its last texel coordinates); 0 if more than box texels either way. */
static inline int vr_shot_box(const VrShotPlan *p, double g_a, double g_b, const double hi[2], double box,
                              int32_t *bx, int32_t *by, uint32_t *bw, uint32_t *bh)
{
    double x[2], y[2];
    int i;
    for (i = 0; i < 2; i++) {
        x[i] = p->v[0] + p->c[0] * (i ? g_b : g_a);
        y[i] = p->v[1] + p->c[1] * (i ? g_b : g_a);
    }
    {
        double x0 = floor(x[0] < x[1] ? x[0] : x[1]), x1 = floor(x[0] < x[1] ? x[1] : x[0]);
        double y0 = floor(y[0] < y[1] ? y[0] : y[1]), y1 = floor(y[0] < y[1] ? y[1] : y[0]);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > hi[0]) x1 = floor(hi[0]);
        if (y1 > hi[1]) y1 = floor(hi[1]);
        if (x1 < x0 || y1 < y0 || x1 - x0 + 1 > box || y1 - y0 + 1 > box)
            return 0;
        *bx = (int32_t)x0;
        *by = (int32_t)y0;
        *bw = (uint32_t)(x1 - x0) + 1;
        *bh = (uint32_t)(y1 - y0) + 1;
    }
    return 1;
}

/* The walk for eye 0 of a frame: vp its matrix (Vc3VrView.vp[0]); its
 * picture k (the dynamic resolution's share) of eye_w x eye_h from the
 * top-left of the side-by-side target (immersive.glsl); the aim yaw, pitch.
 * 0: the ray's line has no texel in the eye's picture (the aim out of the
 * eye's view, or behind it) -- nothing to read, nothing met. */
static inline int vr_shot_plan(const float vp[16], float k, uint32_t eye_w, uint32_t eye_h, float yaw, float pitch,
                               VrShotPlan *p)
{
    float d[3];
    double g_hi, g_lo, hi[2], len;
    if (!(k > 0.0f) || k > 1.0f || !eye_w || !eye_h || !isfinite(yaw) || !isfinite(pitch))
        return 0;
    vr_shot_dir(yaw, pitch, d);
    if (!vr_shot_line(vp, k, eye_w, eye_h, NULL, d, p, &g_hi, &g_lo, hi))
        return 0;
    /* at most VR_SHOT_SPAN texels, from the far end */
    len = hypot(p->c[0], p->c[1]);
    if (len * (g_hi - g_lo) > VR_SHOT_SPAN)
        g_hi = g_lo + VR_SHOT_SPAN / len;
    p->g_hi = (float)g_hi;
    p->g_lo = (float)g_lo;
    if (!vr_shot_box(p, g_lo, g_hi, hi, VR_SHOT_BOX, &p->x0, &p->y0, &p->w, &p->h))
        return 0;
    p->steps = (uint32_t)ceil(len * (g_hi - g_lo)) + 2;
    return 1;
}

/* One step of a walk (step i at g, the scene's depth there, last: the
 * step before's g): 1 when the walk ends there, *t its result (0: nothing
 * met); 0 to go on. */
static inline int vr_shot_step(const VrShotPlan *p, uint32_t i, double g, double scene, double *last, float *t)
{
    const double ray = 0.5 * (p->v[2] + p->c[2] * g) + 0.5;
    if (ray >= scene) {
        double gz = (2.0 * scene - 1.0 - p->v[2]) / p->c[2], tt;
        if (i == 0 && gz > p->g_hi) {
            if (!(gz <= 1.0 / VR_SHOT_NEAR)) {
                *t = 0.0f;
                return 1;
            }
        } else if (!(gz <= *last)) {
            gz = *last;
        }
        if (!(gz >= g))
            gz = g;
        tt = (1.0 / gz - p->bw) / p->aw;
        *t = tt > 0.0 && isfinite(tt) ? (float)tt : 0.0f;
        return 1;
    }
    *last = g;
    return 0;
}

/* A texel of a box (row-major, w wide, 4 bytes each) as 0..1 depth */
static inline double vr_shot_texel(const void *texels, size_t at, int d24)
{
    return d24 ? (((const uint32_t *)texels)[at] & 0xFFFFFFu) / 16777215.0 : (double)((const float *)texels)[at];
}

/* The walk over the box's texels (row-major, w x h, 4 bytes each: D24 in
 * the low 24 bits when d24, else a float -- 0..1 Vulkan depth, the GL
 * depth's (z + 1) / 2): the distance along the ray where it first goes
 * behind what was drawn (game units), placed where the ray's depth is the
 * texel's, between that step and the one before; 0: nothing met. Behind
 * something at the walk's very first texel: where the ray is as deep as
 * it -- unless it is nearer than VR_SHOT_NEAR to the eye, not the world but
 * a picture over it (Silent Scope 2's pause darkens the world with a
 * full-screen layer at the near plane): nothing met. */
static inline float vr_shot_march(const VrShotPlan *p, const void *texels, int d24)
{
    double last = p->g_hi;
    float t;
    uint32_t i;
    if (!p->w || !p->h || p->steps < 2 || !(p->aw > 0.0f))
        return 0.0f;
    for (i = 0; i < p->steps; i++) {
        const double g = p->g_hi + ((double)p->g_lo - p->g_hi) * i / (p->steps - 1);
        long x = (long)floor(p->v[0] + p->c[0] * g) - p->x0, y = (long)floor(p->v[1] + p->c[1] * g) - p->y0;
        x = x < 0 ? 0 : x >= (long)p->w ? (long)p->w - 1 : x;
        y = y < 0 ? 0 : y >= (long)p->h ? (long)p->h - 1 : y;
        if (vr_shot_step(p, i, g, vr_shot_texel(texels, (size_t)y * p->w + (size_t)x, d24), &last, &t))
            return t;
    }
    return 0.0f;
}

/* A ray from anywhere -- the gun's muzzle (the profile's vr.aim_world),
 * not the game camera: an eye half a metre off it sees the ray's line run
 * ~500 texels at a metre out (the Quest's ~1100 a radian). The walk reads
 * up to VR_SHOT_RAY_SPAN texels of it, toward the vanishing point as the
 * rifle's, in segments of VR_SHOT_SEG steps, each its own small box --
 * VR_SHOT_SEG_BYTES of the buffer apiece, segment j at j times that --
 * rather than one box round a long diagonal. */
#define VR_SHOT_SEG 32
#define VR_SHOT_SEGS 32
#define VR_SHOT_SEG_BOX (VR_SHOT_SEG + 2)
#define VR_SHOT_SEG_BYTES ((size_t)VR_SHOT_SEG_BOX * VR_SHOT_SEG_BOX * 4)
#define VR_SHOT_RAY_SPAN (VR_SHOT_SEG * VR_SHOT_SEGS - 2)

typedef struct {
    VrShotPlan line;            /* the line and the walk (v, c, aw, bw, g_hi, g_lo, steps; its box: the first segment's) */
    uint32_t segs;              /* steps j * VR_SHOT_SEG on are read from box[j] */
    struct { int32_t x0, y0; uint32_t w, h; } box[VR_SHOT_SEGS];
} VrShotRay;

/* The walk of the ray from o along d (GL view space of the game camera,
 * game units; d need not be unit -- the walk's distances are along it
 * normalised) through eye 0 of a frame, as vr_shot_plan's (vp, k, eye_w,
 * eye_h). 0: none (no texel of the ray in the eye's picture). */
static inline int vr_shot_plan_ray(const float vp[16], float k, uint32_t eye_w, uint32_t eye_h, const float o[3],
                                   const float d[3], VrShotRay *r)
{
    VrShotPlan *p = &r->line;
    float u[3];
    double g_hi, g_lo, hi[2], len, n;
    uint32_t j;
    int i;
    if (!(k > 0.0f) || k > 1.0f || !eye_w || !eye_h)
        return 0;
    for (i = 0; i < 3; i++)
        if (!isfinite(o[i]) || !isfinite(d[i]))
            return 0;
    n = sqrt((double)d[0] * d[0] + (double)d[1] * d[1] + (double)d[2] * d[2]);
    if (!(n > 1e-9))
        return 0;
    for (i = 0; i < 3; i++)
        u[i] = (float)(d[i] / n);
    if (!vr_shot_line(vp, k, eye_w, eye_h, o, u, p, &g_hi, &g_lo, hi))
        return 0;
    len = hypot(p->c[0], p->c[1]);
    if (len * (g_hi - g_lo) > VR_SHOT_RAY_SPAN)
        g_hi = g_lo + VR_SHOT_RAY_SPAN / len;
    p->g_hi = (float)g_hi;
    p->g_lo = (float)g_lo;
    p->steps = (uint32_t)ceil(len * (g_hi - g_lo)) + 2;
    r->segs = (p->steps + VR_SHOT_SEG - 1) / VR_SHOT_SEG;
    for (j = 0; j < r->segs; j++) {     /* each segment's box: its steps' texels, as the walk finds them */
        const uint32_t i0 = j * VR_SHOT_SEG, i1 = i0 + VR_SHOT_SEG - 1 < p->steps - 1 ? i0 + VR_SHOT_SEG - 1 : p->steps - 1;
        const double ga = p->g_hi + ((double)p->g_lo - p->g_hi) * i0 / (p->steps - 1);
        const double gb = p->g_hi + ((double)p->g_lo - p->g_hi) * i1 / (p->steps - 1);
        if (!vr_shot_box(p, ga, gb, hi, VR_SHOT_SEG_BOX, &r->box[j].x0, &r->box[j].y0, &r->box[j].w, &r->box[j].h))
            return 0;
    }
    p->x0 = r->box[0].x0;
    p->y0 = r->box[0].y0;
    p->w = r->box[0].w;
    p->h = r->box[0].h;
    return 1;
}

/* Seen from an eye well off the ray, something drawn in front of it -- a
 * zombie's arm, an effect near the camera -- hides the ray's way on behind
 * it without the ray touching it: where the ray first goes behind a
 * surface more than VR_SHOT_THICK (metres, eye depth) in front of where
 * the ray was a step before, it passes behind it, and the walk goes on. */
#define VR_SHOT_THICK 0.3

/* The ray's walk over its segments' boxes (texels: segment j's box at j *
 * VR_SHOT_SEG_BYTES, as vr_shot_march's): the distance along it from o
 * where it first meets what was drawn, game units -- the first texel and
 * the meeting placed as vr_shot_march's -- passing behind what lies more
 * than VR_SHOT_THICK in front of it, and seeing through what was drawn
 * nearer than VR_SHOT_NEAR to the eye (the walk never starts nearer: a
 * picture over the world, not the world -- House of the Dead III draws its
 * shots' flashes into the eyes at 27 cm). Hidden to the walk's end (it met
 * something behind what hides it): where it first went out of sight. 0:
 * nothing met; -1: nothing met where the eye sees the world, but such a
 * picture hid part of the ray (a shot's flash over where the laser ends):
 * not known -- the last frame's answer stands. */
static inline float vr_shot_march_ray(const VrShotRay *r, const void *texels, int d24)
{
    const VrShotPlan *p = &r->line;
    double last = p->g_hi;
    float t, hidden = 0.0f;
    int behind = 0, covered = 0;
    uint32_t i;
    if (!r->segs || r->segs > VR_SHOT_SEGS || p->steps < 2 || !(p->aw > 0.0f))
        return 0.0f;
    for (i = 0; i < p->steps; i++) {
        const uint32_t j = i / VR_SHOT_SEG;
        const double g = p->g_hi + ((double)p->g_lo - p->g_hi) * i / (p->steps - 1);
        long x = (long)floor(p->v[0] + p->c[0] * g) - r->box[j].x0, y = (long)floor(p->v[1] + p->c[1] * g) - r->box[j].y0;
        double scene, gz;
        x = x < 0 ? 0 : x >= (long)r->box[j].w ? (long)r->box[j].w - 1 : x;
        y = y < 0 ? 0 : y >= (long)r->box[j].h ? (long)r->box[j].h - 1 : y;
        scene = vr_shot_texel((const char *)texels + j * VR_SHOT_SEG_BYTES, (size_t)y * r->box[j].w + (size_t)x, d24);
        gz = (2.0 * scene - 1.0 - p->v[2]) / p->c[2];   /* 1 / the surface's eye depth there */
        if (gz > 1.0 / VR_SHOT_NEAR) {      /* a picture over the world */
            last = g;
            covered = 1;
            continue;
        }
        if (i > 0 && 0.5 * (p->v[2] + p->c[2] * g) + 0.5 >= scene && gz > 0.0 && 1.0 / gz < 1.0 / last - VR_SHOT_THICK) {
            if (!behind) {      /* out of sight: where, should it stay so */
                double l = last;
                behind = vr_shot_step(p, i, g, scene, &l, &hidden);
            }
            last = g;
            continue;
        }
        if (vr_shot_step(p, i, g, scene, &last, &t))
            return t;
    }
    return behind ? hidden : covered ? -1.0f : 0.0f;
}

#endif
