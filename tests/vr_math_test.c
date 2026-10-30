/* vr_math_test.c - checks of src/vr_math.h, the immersive view's maths:
 *
 *   cc -O1 -Isrc tests/vr_math_test.c -o /tmp/vr_math_test -lm && /tmp/vr_math_test
 *
 * A Direct3D-style camera is built as the NV2A sees it (projection with the
 * viewport folded in, composite = that * model-view), and the immersive
 * path's steps are checked against it: the projection recovered from the
 * matrices, a vertex's eye position recovered from its screen position and
 * w, an eye's matrix putting a point straight ahead in the middle of its
 * view, and the aim's projection agreeing with the game's. */
#include <stdio.h>
#include <stdlib.h>

#include "vr_math.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b, eps) (fabsf((a) - (b)) <= (eps))

/* row-major helpers, as the NV2A holds them */
static void rm_mul(const float *a, const float *b, float *o)
{
    float r[16];
    int i, j, k;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            float s = 0;
            for (k = 0; k < 4; k++)
                s += a[4 * i + k] * b[4 * k + j];
            r[4 * i + j] = s;
        }
    memcpy(o, r, sizeof r);
}

static void rm_apply(const float *m, const float v[4], float o[4])
{
    int i;
    for (i = 0; i < 4; i++)
        o[i] = m[4 * i] * v[0] + m[4 * i + 1] * v[1] + m[4 * i + 2] * v[2] + m[4 * i + 3] * v[3];
}

/* D3D LH perspective with the 640x480 viewport folded in, row-major */
static void d3d_proj_vp(float fovy_deg, float aspect, float zn, float zf, float W, float H, float *o)
{
    float ys = 1.0f / tanf(fovy_deg * 0.5f * 3.14159265f / 180.0f), xs = ys / aspect;
    float P[16] = { xs, 0, 0, 0,  0, ys, 0, 0,  0, 0, zf / (zf - zn), -zn * zf / (zf - zn),  0, 0, 1, 0 };
    float V[16] = { W / 2, 0, 0, W / 2,  0, -H / 2, 0, H / 2,  0, 0, 1, 0,  0, 0, 0, 1 };
    rm_mul(V, P, o);
}

/* a model-view: a turn about y and a move, row-major */
static void some_mv(float *o)
{
    float a = 0.4f, c = cosf(a), s = sinf(a);
    float m[16] = { c, 0, -s, 1.5f,  0, 1, 0, -0.7f,  s, 0, c, 3.0f,  0, 0, 0, 1 };
    memcpy(o, m, sizeof m);
}

static void test_projection_recovery(void)
{
    float Pvp[16], mv[16], comp[16], zero[16] = { 0 }, c59[2] = { 0.53125f, 0.53125f };
    VrGameProj g;
    float pos[4] = { 0.3f, 1.2f, 4.0f, 1.0f }, eye[4], clip[4];
    float sx, sy, v[3], e[3];

    d3d_proj_vp(60.0f, 4.0f / 3.0f, 0.1f, 1000.0f, 640, 480, Pvp);
    some_mv(mv);
    rm_mul(Pvp, mv, comp);
    CHECK(vr_game_proj_from(comp, mv, 1, zero, 1.0f, 1.0f, c59, &g), "composite path not recovered");
    CHECK(NEAR(g.cx, 320.53125f, 1e-3f) && NEAR(g.cy, 240.53125f, 1e-3f), "centre %f %f", g.cx, g.cy);
    CHECK(NEAR(g.kx, 320.0f / tanf(0.5236f) * 0.75f, 0.5f), "kx %f", g.kx);
    CHECK(g.ky < 0, "ky should be negative (screen y down): %f", g.ky);

    /* a vertex through the composite, as the NV2A does it, then back */
    rm_apply(mv, pos, eye);
    rm_apply(comp, pos, clip);
    sx = clip[0] / clip[3] + c59[0];
    sy = clip[1] / clip[3] + c59[1];
    vr_unproject(&g, sx, sy, clip[3], e);
    CHECK(NEAR(e[0], eye[0], 1e-3f) && NEAR(e[1], eye[1], 1e-3f) && NEAR(e[2], eye[2], 1e-3f),
          "eye %f %f %f, recovered %f %f %f", eye[0], eye[1], eye[2], e[0], e[1], e[2]);
    vr_unproject_gl(&g, sx, sy, clip[3], v);
    CHECK(NEAR(v[0], eye[0], 1e-3f) && NEAR(v[1], eye[1], 1e-3f) && NEAR(v[2], -eye[2], 1e-3f),
          "GL view %f %f %f", v[0], v[1], v[2]);
    {   /* and forward again */
        float px, py;
        CHECK(vr_project_gl(&g, v, &px, &py) && NEAR(px, sx, 1e-2f) && NEAR(py, sy, 1e-2f),
              "reprojected %f %f vs %f %f", px, py, sx, sy);
    }

    /* the skinned path: the projection matrix itself */
    CHECK(vr_game_proj_from(zero, mv, 0, Pvp, 1.0f, 1.0f, c59, &g), "projection path not recovered");
    CHECK(NEAR(g.cx, 320.53125f, 1e-3f), "skinned centre %f", g.cx);

    /* antialiasing scale: screen doubled */
    CHECK(vr_game_proj_from(comp, mv, 1, zero, 2.0f, 2.0f, c59, &g) && NEAR(g.cx, 641.0625f, 1e-3f),
          "aa centre %f", g.cx);

    /* an orthographic (2D) matrix is not a perspective */
    {
        float ortho[16] = { 1, 0, 0, 0,  0, -1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
        CHECK(!vr_game_proj_from(zero, mv, 0, ortho, 1, 1, c59, &g), "ortho accepted");
    }
    /* a model-view that cannot be inverted */
    CHECK(!vr_game_proj_from(comp, zero, 1, zero, 1, 1, c59, &g), "singular model-view accepted");
}

/* the right-handed case: eye z backward, the projection's w = -ez */
static void test_right_handed(void)
{
    float P[16] = { 1.3f * 320, 0, -320, 0,  0, -1.73f * 240, -240, 0,  0, 0, -1, -0.2f,  0, 0, -1, 0 };
    float mv[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 }, zero[16] = { 0 };
    float c59[2] = { 0, 0 }, pos[4] = { 0.5f, -0.25f, -6.0f, 1 }, clip[4], v[3];
    VrGameProj g;
    CHECK(vr_game_proj_from(zero, mv, 0, P, 1, 1, c59, &g), "RH projection not recovered");
    rm_apply(P, pos, clip);
    CHECK(clip[3] > 0, "RH w %f", clip[3]);
    vr_unproject_gl(&g, clip[0] / clip[3], clip[1] / clip[3], clip[3], v);
    CHECK(NEAR(v[0], 0.5f, 1e-3f) && NEAR(v[1], -0.25f, 1e-3f) && NEAR(v[2], -6.0f, 1e-3f),
          "RH GL view %f %f %f", v[0], v[1], v[2]);
}

static void clip_of(const float m[16], const float v[3], float c[4])
{
    int i;
    for (i = 0; i < 4; i++)
        c[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i];
}

static void test_eye_matrix(void)
{
    const float id[4] = { 0, 0, 0, 1 }, tan_fov[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
    float m[16], c[4];
    const float ahead[3] = { 0, 0, -10 }, left[3] = { -10, 0, -10 };

    /* an eye at the camera, looking ahead: ahead is the middle of its view */
    vr_eye_matrix(id, (const float[3]){ 0, 0, 0 }, tan_fov, 1.0f, 0.05f, 1000.0f, m);
    clip_of(m, ahead, c);
    CHECK(NEAR(c[0] / c[3], 0, 1e-5f) && NEAR(c[1] / c[3], 0, 1e-5f) && c[3] > 0,
          "ahead at %f %f", c[0] / c[3], c[1] / c[3]);
    /* 45 degrees left is the left edge (tan 1) */
    clip_of(m, left, c);
    CHECK(NEAR(c[0] / c[3], -1, 1e-4f), "left edge at %f", c[0] / c[3]);
    CHECK(c[2] / c[3] > -1 && c[2] / c[3] < 1, "depth %f outside", c[2] / c[3]);

    /* head turned 45 degrees left (about +y): what was at the left edge
     * is now in the middle */
    {
        float a = 0.785398f, q[4] = { 0, sinf(a / 2), 0, cosf(a / 2) };
        vr_eye_matrix(q, (const float[3]){ 0, 0, 0 }, tan_fov, 1.0f, 0.05f, 1000.0f, m);
        clip_of(m, left, c);
        CHECK(NEAR(c[0] / c[3], 0, 1e-4f), "turned: left point at %f", c[0] / c[3]);
    }
    /* an eye 3.2 cm to the right sees a point 10 m ahead slightly left */
    {
        vr_eye_matrix(id, (const float[3]){ 0.032f, 0, 0 }, tan_fov, 1.0f, 0.05f, 1000.0f, m);
        clip_of(m, ahead, c);
        CHECK(c[0] / c[3] < 0 && NEAR(c[0] / c[3], -0.0032f, 1e-4f), "right eye: %f", c[0] / c[3]);
    }
    /* game units of 10 per metre: a point 10 units ahead is 1 m ahead; an
     * eye 1 m forward of the camera has it at its own position: depth
     * clipped (behind near) */
    {
        vr_eye_matrix(id, (const float[3]){ 0, 0, -0.5f }, tan_fov, 10.0f, 0.05f, 1000.0f, m);
        clip_of(m, (const float[3]){ 0, 0, -10 }, c);
        CHECK(NEAR(c[3], 0.5f, 1e-4f), "units: w %f (0.5 m ahead)", c[3]);
    }
}

/* The aim: a point on the HUD panel, which is the game's frustum at
 * distance d, projects to the screen position the panel shows there. */
static void test_aim_through_panel(void)
{
    float Pvp[16], mv[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 }, comp[16], zero[16] = { 0 };
    float c59[2] = { 0, 0 };
    VrGameProj g;
    float d = 6.0f, panel_w, panel_h, sx, sy;
    d3d_proj_vp(55.0f, 4.0f / 3.0f, 0.1f, 1000.0f, 640, 480, Pvp);
    rm_mul(Pvp, mv, comp);
    CHECK(vr_game_proj_from(comp, mv, 1, zero, 1, 1, c59, &g), "aim: projection");
    /* the panel: the screen's 640 x 480 at depth d */
    panel_w = d * 640.0f / g.kx;
    panel_h = d * 480.0f / -g.ky;
    /* the panel's top-left corner, a quarter across and down */
    {
        float v[3] = { -panel_w / 2 + panel_w / 4, panel_h / 2 - panel_h / 4, -d };
        CHECK(vr_project_gl(&g, v, &sx, &sy) && NEAR(sx, 160, 0.05f) && NEAR(sy, 120, 0.05f),
              "aim at quarter: %f %f", sx, sy);
    }
    /* behind the camera: no aim */
    {
        float v[3] = { 0, 0, 2 };
        CHECK(!vr_project_gl(&g, v, &sx, &sy), "aim behind the camera accepted");
    }
}

int main(void)
{
    test_projection_recovery();
    test_right_handed();
    test_eye_matrix();
    test_aim_through_panel();
    printf(fails ? "%d checks failed\n" : "all vr_math checks pass\n", fails);
    return fails != 0;
}
