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

/* The walk for eye 0 of a frame: vp its matrix (Vc3VrView.vp[0]); its
 * picture k (the dynamic resolution's share) of eye_w x eye_h from the
 * top-left of the side-by-side target (immersive.glsl); the aim yaw, pitch.
 * 0: the ray's line has no texel in the eye's picture (the aim out of the
 * eye's view, or behind it) -- nothing to read, nothing met. */
static inline int vr_shot_plan(const float vp[16], float k, uint32_t eye_w, uint32_t eye_h, float yaw, float pitch,
                               VrShotPlan *p)
{
    float d[3], a[4], b[4], V[3], C[3];
    double g_hi, g_lo, lo[2], hi[2], x[2], y[2], len;
    int i;
    if (!(k > 0.0f) || k > 1.0f || !eye_w || !eye_h || !isfinite(yaw) || !isfinite(pitch))
        return 0;
    vr_shot_dir(yaw, pitch, d);
    for (i = 0; i < 4; i++) {
        a[i] = vp[i] * d[0] + vp[4 + i] * d[1] + vp[8 + i] * d[2];
        b[i] = vp[12 + i];
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
    /* at most VR_SHOT_SPAN texels, from the far end */
    len = hypot(p->c[0], p->c[1]);
    if (len * (g_hi - g_lo) > VR_SHOT_SPAN)
        g_hi = g_lo + VR_SHOT_SPAN / len;
    p->g_hi = (float)g_hi;
    p->g_lo = (float)g_lo;
    for (i = 0; i < 2; i++) {
        x[i] = p->v[0] + p->c[0] * (i ? g_hi : g_lo);
        y[i] = p->v[1] + p->c[1] * (i ? g_hi : g_lo);
    }
    {
        double x0 = floor(x[0] < x[1] ? x[0] : x[1]), x1 = floor(x[0] < x[1] ? x[1] : x[0]);
        double y0 = floor(y[0] < y[1] ? y[0] : y[1]), y1 = floor(y[0] < y[1] ? y[1] : y[0]);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > hi[0]) x1 = floor(hi[0]);
        if (y1 > hi[1]) y1 = floor(hi[1]);
        if (x1 < x0 || y1 < y0 || x1 - x0 + 1 > VR_SHOT_BOX || y1 - y0 + 1 > VR_SHOT_BOX)
            return 0;
        p->x0 = (int32_t)x0;
        p->y0 = (int32_t)y0;
        p->w = (uint32_t)(x1 - x0) + 1;
        p->h = (uint32_t)(y1 - y0) + 1;
    }
    p->steps = (uint32_t)ceil(len * (g_hi - g_lo)) + 2;
    return 1;
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
    const uint32_t *word = (const uint32_t *)texels;
    const float *depth = (const float *)texels;
    double last = p->g_hi;
    uint32_t i;
    if (!p->w || !p->h || p->steps < 2 || !(p->aw > 0.0f))
        return 0.0f;
    for (i = 0; i < p->steps; i++) {
        const double g = p->g_hi + ((double)p->g_lo - p->g_hi) * i / (p->steps - 1);
        long x = (long)floor(p->v[0] + p->c[0] * g) - p->x0, y = (long)floor(p->v[1] + p->c[1] * g) - p->y0;
        size_t at;
        double scene, ray;
        x = x < 0 ? 0 : x >= (long)p->w ? (long)p->w - 1 : x;
        y = y < 0 ? 0 : y >= (long)p->h ? (long)p->h - 1 : y;
        at = (size_t)y * p->w + (size_t)x;
        scene = d24 ? (word[at] & 0xFFFFFFu) / 16777215.0 : (double)depth[at];
        ray = 0.5 * (p->v[2] + p->c[2] * g) + 0.5;
        if (ray >= scene) {
            double gz = (2.0 * scene - 1.0 - p->v[2]) / p->c[2], t;
            if (i == 0 && gz > p->g_hi) {
                if (!(gz <= 1.0 / VR_SHOT_NEAR))
                    return 0.0f;
            } else if (!(gz <= last)) {
                gz = last;
            }
            if (!(gz >= g))
                gz = g;
            t = (1.0 / gz - p->bw) / p->aw;
            return t > 0.0 && isfinite(t) ? (float)t : 0.0f;
        }
        last = g;
    }
    return 0.0f;
}

#endif
