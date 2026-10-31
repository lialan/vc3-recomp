/*
 * vr_math.h - the matrix work of the headset's immersive view, header-only
 * so the Quest host, the GL back end and tests/vr_math_test.c share it.
 *
 * Conventions. "Game eye space" is the game's camera space as Direct3D has
 * it: x right, y up, z forward, in game units (VC3's are about metres).
 * "GL view space" is the same with z backward (x, y, -z). The game's
 * projection maps game eye space to the screen in guest pixels:
 *
 *     sx = kx * ex / ez + cx,    sy = ky * ey / ez + cy,    w = ez * pz
 *
 * (ky is negative: the screen's y runs down.) VrGameProj holds kx, ky, cx,
 * cy and 1 / pz, so a vertex's eye position comes back from its screen
 * position and w alone, whichever path transformed it.
 *
 * Matrices here are column-major (GL's), m[col * 4 + row], unless a name
 * says row-major (the NV2A's: result[i] = sum_j m[4i + j] v[j]).
 */
#ifndef VC3_VR_MATH_H
#define VC3_VR_MATH_H

#include <math.h>
#include <string.h>

typedef struct {
    float kx, ky, cx, cy, inv_pz;
} VrGameProj;

/* o = a * b, column-major */
static inline void vr_mul(float *o, const float *a, const float *b)
{
    float r[16];
    int c, k;
    for (c = 0; c < 4; c++)
        for (k = 0; k < 4; k++)
            r[c * 4 + k] = a[k] * b[c * 4] + a[4 + k] * b[c * 4 + 1] + a[8 + k] * b[c * 4 + 2]
                         + a[12 + k] * b[c * 4 + 3];
    memcpy(o, r, 64);
}

/* v by quaternion q (x y z w) */
static inline void vr_qrot(const float q[4], const float v[3], float o[3])
{
    float tx = 2 * (q[1] * v[2] - q[2] * v[1]);
    float ty = 2 * (q[2] * v[0] - q[0] * v[2]);
    float tz = 2 * (q[0] * v[1] - q[1] * v[0]);
    o[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    o[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    o[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

/* a * b, quaternions */
static inline void vr_qmul(const float a[4], const float b[4], float o[4])
{
    float r[4];
    r[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    r[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    r[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    r[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    memcpy(o, r, sizeof r);
}

/* The inverse of the pose (q, p): the view matrix of an eye there. */
static inline void vr_view_from_pose(const float q[4], const float p[3], float m[16])
{
    const float qi[4] = { -q[0], -q[1], -q[2], q[3] };
    const float ex[3] = { 1, 0, 0 }, ey[3] = { 0, 1, 0 }, ez[3] = { 0, 0, 1 };
    float c0[3], c1[3], c2[3], t[3] = { -p[0], -p[1], -p[2] }, rt[3];
    vr_qrot(qi, ex, c0);
    vr_qrot(qi, ey, c1);
    vr_qrot(qi, ez, c2);
    vr_qrot(qi, t, rt);
    memset(m, 0, 64);
    m[0] = c0[0]; m[1] = c0[1]; m[2] = c0[2];
    m[4] = c1[0]; m[5] = c1[1]; m[6] = c1[2];
    m[8] = c2[0]; m[9] = c2[1]; m[10] = c2[2];
    m[12] = rt[0]; m[13] = rt[1]; m[14] = rt[2]; m[15] = 1;
}

/* An OpenXR field of view (tangents of its angles: left and down negative)
 * as a GL projection, depth -1..1 between near and far. */
static inline void vr_proj_fov(float l, float r, float u, float d, float n, float f, float m[16])
{
    memset(m, 0, 64);
    m[0] = 2 / (r - l);
    m[5] = 2 / (u - d);
    m[8] = (r + l) / (r - l);
    m[9] = (u + d) / (u - d);
    m[10] = -(f + n) / (f - n);
    m[11] = -1;
    m[14] = -2 * f * n / (f - n);
}

/* General 4x4 inverse (either layout: the inverse of the transpose is the
 * transpose of the inverse). 0 if singular. */
static inline int vr_inverse(const float *m, float *o)
{
    float inv[16], det;
    int i;
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15]
           + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15]
           - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15]
           + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14]
            - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15]
           - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15]
           + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15]
           - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14]
            + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15]
           + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15]
           - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15]
            + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14]
            - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11]
           - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11]
           + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11]
            - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10]
            + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f || !isfinite(det))
        return 0;
    det = 1.0f / det;
    for (i = 0; i < 16; i++)
        o[i] = inv[i] * det;
    return 1;
}

/* The game's projection from an NV2A fixed-function batch's matrices (all
 * row-major): the projection-with-viewport P is the composite times the
 * inverse model-view, or the projection matrix itself when the batch is
 * skinned; then the screen is (P x / P w + c59) * aa. 1 if P is a plain
 * perspective (no shear, w from ez alone), which is what the immersive view
 * can re-project. */
static inline int vr_game_proj_from(const float comp[16], const float mv[16], int use_comp,
                                    const float proj[16], float aa_x, float aa_y,
                                    const float c59[2], VrGameProj *g)
{
    float P[16], inv[16];
    float s;
    if (use_comp) {
        int i, j, k;
        if (!vr_inverse(mv, inv))
            return 0;
        for (i = 0; i < 4; i++)
            for (j = 0; j < 4; j++) {
                float a = 0;
                for (k = 0; k < 4; k++)
                    a += comp[4 * i + k] * inv[4 * k + j];
                P[4 * i + j] = a;
            }
    } else {
        memcpy(P, proj, sizeof P);
    }
    s = fabsf(P[0]) + fabsf(P[5]);
    if (!(s > 0) || !isfinite(s) || fabsf(P[14]) < 1e-6f)
        return 0;
    /* plain perspective: no x-y shear, w from z only */
    if (fabsf(P[1]) > 1e-3f * s || fabsf(P[4]) > 1e-3f * s
        || fabsf(P[12]) > 1e-4f * fabsf(P[14]) || fabsf(P[13]) > 1e-4f * fabsf(P[14])
        || fabsf(P[15]) > 1e-3f * fabsf(P[14]) || fabsf(P[3]) > 1e-3f * s
        || fabsf(P[7]) > 1e-3f * s)
        return 0;
    g->inv_pz = 1.0f / P[14];
    g->kx = aa_x * P[0] / P[14];
    g->ky = aa_y * P[5] / P[14];
    g->cx = aa_x * (P[2] / P[14] + c59[0]);
    g->cy = aa_y * (P[6] / P[14] + c59[1]);
    return isfinite(g->kx) && isfinite(g->ky) && isfinite(g->cx) && isfinite(g->cy);
}

/* A screen position and its w back to game eye space. */
static inline void vr_unproject(const VrGameProj *g, float sx, float sy, float w, float e[3])
{
    float ez = w * g->inv_pz;
    e[0] = (sx - g->cx) / g->kx * ez;
    e[1] = (sy - g->cy) / g->ky * ez;
    e[2] = ez;
}

/* Game eye space to the screen (guest pixels); 0 behind the camera. */
static inline int vr_project(const VrGameProj *g, const float e[3], float *sx, float *sy)
{
    if (e[2] <= 1e-6f)
        return 0;
    *sx = g->kx * e[0] / e[2] + g->cx;
    *sy = g->ky * e[1] / e[2] + g->cy;
    return 1;
}

/* A screen position and its w to GL view space (x right, y up, z back), for
 * a left- or right-handed game camera alike: forward depth is w / |pz|.
 * The shaders do the same. */
static inline void vr_unproject_gl(const VrGameProj *g, float sx, float sy, float w, float v[3])
{
    float ez = w * g->inv_pz;
    v[0] = (sx - g->cx) / g->kx * ez;
    v[1] = (sy - g->cy) / g->ky * ez;
    v[2] = -w * fabsf(g->inv_pz);
}

/* GL view space to the screen; 0 behind the camera. */
static inline int vr_project_gl(const VrGameProj *g, const float v[3], float *sx, float *sy)
{
    float w = -v[2] / fabsf(g->inv_pz), ez = w * g->inv_pz;
    if (w <= 1e-6f)
        return 0;
    *sx = g->kx * v[0] / ez + g->cx;
    *sy = g->ky * v[1] / ez + g->cy;
    return 1;
}

/* One eye's matrix for the immersive view: GL view space of the game's
 * camera (game units) to that eye's clip space. The eye is at (q, p) in the
 * anchor's frame, metres; units: game units per metre. */
static inline void vr_eye_matrix(const float q[4], const float p[3], const float tan_fov[4],
                                 float units, float near_m, float far_m, float out[16])
{
    float view[16], proj[16], s[16];
    vr_view_from_pose(q, p, view);
    vr_proj_fov(tan_fov[0], tan_fov[1], tan_fov[2], tan_fov[3], near_m, far_m, proj);
    memset(s, 0, sizeof s);
    s[0] = s[5] = s[10] = 1.0f / units;
    s[15] = 1;
    vr_mul(out, view, s);
    vr_mul(out, proj, out);
}

#endif
