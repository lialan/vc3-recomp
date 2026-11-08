/* vr_shot_test.c - checks of runtime/src/host/vr_shot.h, where the rifle's
 * shot meets the scene from an eye's depth (scripts/vr-rifle-test.sh): an
 * eye off the game camera, its depth drawn by casting a ray through each
 * texel into a made-up scene (a wall, a post in front of part of it), the
 * shot's ray marched through it against the true intersection. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vr_math.h"
#include "vr_shot.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b, e) do { double _a = (a), _b = (b); if (!(fabs(_a - _b) <= (e))) { \
    printf("FAIL %s:%d: %s = %g, want %g (+-%g)\n", __FILE__, __LINE__, #a, _a, _b, (double)(e)); fails++; } } while (0)

#define DEG (3.14159265358979323846 / 180.0)
#define EYE 1024

/* The scene, in the game camera's GL view space (game units): a wall at
 * z = -wall (0: none); a post in front of it at z = -post covering x >=
 * post_x (post 0: none). Returns the nearest t > 0 along o + t d, or 0. */
typedef struct { double wall, post, post_x, layer, post_w; } Scene;   /* layer: every texel at this depth (a picture over the world); post_w: the post only that wide (0: on for good) */
static double scene_hit(const Scene *s, const double o[3], const double d[3])
{
    double best = 0;
    if (s->post > 0 && d[2] < 0) {
        const double t = (-s->post - o[2]) / d[2];
        if (t > 0 && o[0] + t * d[0] >= s->post_x && (s->post_w <= 0 || o[0] + t * d[0] <= s->post_x + s->post_w))
            best = t;
    }
    if (s->wall > 0 && d[2] < 0) {
        const double t = (-s->wall - o[2]) / d[2];
        if (t > 0 && (!best || t < best))
            best = t;
    }
    return best;
}

/* An eye at (q, p) metres in the anchor's frame, a symmetric 100-degree
 * field, game units: units per metre */
typedef struct { float vp[16], inv[16]; } Eye;
static void eye_make(Eye *e, const float q[4], const float p[3], float units)
{
    const float t = (float)tan(50 * DEG), tan_fov[4] = { -t, t, t, -t };
    vr_eye_matrix(q, p, tan_fov, units, 0.05f, 2000.0f, e->vp);
    vr_inverse(e->vp, e->inv);
}

/* The box's depth as the eye drew the scene: per texel, the ray from the
 * eye through its centre (k of the eye from the top-left). d24: as D24
 * words, else floats. */
static void draw_box(const Eye *e, const Scene *s, const VrShotPlan *p, float k, int d24, void *out)
{
    for (uint32_t j = 0; j < p->h; j++)
        for (uint32_t i = 0; i < p->w; i++) {
            const double nx = (p->x0 + i + 0.5) / (0.5 * k * EYE) - 1, ny = 1 - (p->y0 + j + 0.5) / (0.5 * k * EYE);
            double a[4], b[4], o[3], d[3], n = 0, depth = 1;
            for (int r = 0; r < 4; r++) {    /* the near and far points of the texel's ray */
                a[r] = e->inv[r] * nx + e->inv[4 + r] * ny + e->inv[8 + r] * -1 + e->inv[12 + r];
                b[r] = e->inv[r] * nx + e->inv[4 + r] * ny + e->inv[8 + r] * 1 + e->inv[12 + r];
            }
            for (int r = 0; r < 3; r++) { o[r] = a[r] / a[3]; d[r] = b[r] / b[3] - o[r]; n += d[r] * d[r]; }
            for (int r = 0; r < 3; r++) d[r] /= sqrt(n);
            const double t = scene_hit(s, o, d);
            if (s->layer > 0)
                depth = s->layer;
            else if (t > 0) {
                double c[4];
                for (int r = 0; r < 4; r++)
                    c[r] = e->vp[r] * (o[0] + t * d[0]) + e->vp[4 + r] * (o[1] + t * d[1]) +
                           e->vp[8 + r] * (o[2] + t * d[2]) + e->vp[12 + r];
                depth = 0.5 * c[2] / c[3] + 0.5;
                if (depth > 1) depth = 1;
            }
            if (d24) ((uint32_t *)out)[j * p->w + i] = (uint32_t)lround(depth * 16777215.0) | 0xAB000000u;
            else ((float *)out)[j * p->w + i] = (float)depth;
        }
}

/* The shot along yaw, pitch from the camera through the eye's depth: the
 * march's t against the scene's own; 0 when either is nothing. */
static double shot(const Eye *e, const Scene *s, float k, int d24, float yaw, float pitch, double *want)
{
    static uint32_t box[VR_SHOT_BOX * VR_SHOT_BOX];
    VrShotPlan p;
    float df[3];
    double o[3] = { 0, 0, 0 }, d[3];
    vr_shot_dir(yaw, pitch, df);
    for (int i = 0; i < 3; i++) d[i] = df[i];
    *want = scene_hit(s, o, d);
    if (!vr_shot_plan(e->vp, k, EYE, EYE, yaw, pitch, &p))
        return -1;
    CHECK(p.w >= 1 && p.h >= 1 && p.w <= VR_SHOT_BOX && p.h <= VR_SHOT_BOX);
    CHECK(p.x0 >= 0 && p.y0 >= 0 && p.x0 + p.w <= (uint32_t)ceil(k * EYE) && p.y0 + p.h <= (uint32_t)ceil(k * EYE));
    draw_box(e, s, &p, k, d24, box);
    return vr_shot_march(&p, box, d24);
}

static const float ident[4] = { 0, 0, 0, 1 };

/* the desktop stand-in's eye: 32 mm (as game units) right of the camera */
static void wall_ahead(void)
{
    const float p[3] = { 0.032f, 0, 0 };
    const Scene s = { 6.0, 0, 0, 0 };
    const float aims[][2] = { { 0, 0 }, { 10, 5 }, { -20, -8 }, { 35, 25 } };
    Eye e;
    double want, got;
    eye_make(&e, ident, p, 1.0f);
    for (unsigned i = 0; i < sizeof aims / sizeof *aims; i++) {
        got = shot(&e, &s, 1.0f, 0, (float)(aims[i][0] * DEG), (float)(aims[i][1] * DEG), &want);
        CHECK(want > 0);
        NEAR(got, want, want * 0.002);
        got = shot(&e, &s, 1.0f, 1, (float)(aims[i][0] * DEG), (float)(aims[i][1] * DEG), &want);   /* as D24 */
        NEAR(got, want, want * 0.002);
    }
}

/* the Quest's scale (0.08 game units a metre): the eye 3 cm off and the head
 * 15 cm from where it was recentred, a wall 50 units (625 m) out, and one
 * 0.5 units (6 m) out; at a dynamic resolution of 0.8 */
static void quest_scale(void)
{
    const float p[3] = { 0.15f - 0.032f, -0.05f, 0.08f };
    const float q[4] = { 0, (float)sin(-8 * DEG), 0, (float)cos(-8 * DEG) };   /* head turned 16 deg right */
    const Scene far = { 50.0, 0, 0, 0 }, close = { 0.5, 0, 0, 0 };
    Eye e;
    double want, got;
    eye_make(&e, q, p, 0.08f);
    got = shot(&e, &far, 0.8f, 1, (float)(12 * DEG), (float)(-4 * DEG), &want);
    NEAR(got, want, want * 0.01);
    got = shot(&e, &close, 0.8f, 1, (float)(12 * DEG), (float)(-4 * DEG), &want);
    NEAR(got, want, want * 0.005);
    got = shot(&e, &close, 1.0f, 0, (float)(-5 * DEG), (float)(3 * DEG), &want);
    NEAR(got, want, want * 0.005);
}

/* A post in front of the wall's right: the shot passing left of its edge
 * meets the wall, right of it the post -- its edge as the camera sees it,
 * not as the eye does. */
static void post_edge(void)
{
    const float p[3] = { 0.032f, 0, 0 };
    const Scene s = { 8.0, 3.0, 0.0, 0 };     /* the post's edge straight ahead of the camera */
    Eye e;
    double want, got;
    eye_make(&e, ident, p, 1.0f);
    got = shot(&e, &s, 1.0f, 0, (float)(-1.0 * DEG), 0, &want);
    NEAR(want, 8.0 / cos(1.0 * DEG), 1e-6);
    NEAR(got, want, want * 0.002);
    got = shot(&e, &s, 1.0f, 0, (float)(1.0 * DEG), 0, &want);
    NEAR(want, 3.0 / cos(1.0 * DEG), 1e-6);
    NEAR(got, want, want * 0.01);
    /* the left eye sees past the post's edge where the camera does not */
    {
        const float pl[3] = { -0.032f, 0, 0 };
        eye_make(&e, ident, pl, 1.0f);
        got = shot(&e, &s, 1.0f, 0, (float)(0.2 * DEG), 0, &want);
        NEAR(want, 3.0 / cos(0.2 * DEG), 1e-6);
        NEAR(got, want, want * 0.01);
    }
}

/* nothing drawn (the sky: cleared depth) -- nothing met; the aim out of the
 * eye's view or behind it -- no walk at all */
static void nothing_met(void)
{
    const float p[3] = { 0.032f, 0, 0 };
    const Scene sky = { 0, 0, 0, 0 };
    Eye e;
    VrShotPlan plan;
    double want, got;
    eye_make(&e, ident, p, 1.0f);
    got = shot(&e, &sky, 1.0f, 1, (float)(5 * DEG), (float)(5 * DEG), &want);
    CHECK(want == 0);
    NEAR(got, 0, 0);
    CHECK(!vr_shot_plan(e.vp, 1.0f, EYE, EYE, (float)(80 * DEG), 0, &plan));    /* out of the 100-degree view */
    CHECK(!vr_shot_plan(e.vp, 1.0f, EYE, EYE, (float)(180 * DEG), 0, &plan));   /* behind */
    CHECK(!vr_shot_plan(e.vp, 1.0f, EYE, EYE, NAN, 0, &plan));
    CHECK(!vr_shot_plan(e.vp, 0.0f, EYE, EYE, 0, 0, &plan));
}

/* the eye far off the camera: the line long, read only VR_SHOT_SPAN texels
 * of it toward the vanishing point -- a far wall still met; the box small */
static void long_line(void)
{
    const float p[3] = { 0.5f, 0.3f, 0 };
    const Scene s = { 40.0, 0, 0, 0 };
    Eye e;
    VrShotPlan plan;
    double want, got;
    eye_make(&e, ident, p, 1.0f);
    CHECK(vr_shot_plan(e.vp, 1.0f, EYE, EYE, (float)(3 * DEG), (float)(2 * DEG), &plan));
    CHECK(plan.steps <= VR_SHOT_SPAN + 3);
    CHECK(plan.w <= VR_SHOT_BOX && plan.h <= VR_SHOT_BOX);
    got = shot(&e, &s, 1.0f, 0, (float)(3 * DEG), (float)(2 * DEG), &want);
    NEAR(got, want, want * 0.002);
}

/* a picture over the world at the near plane (Silent Scope 2's pause): not
 * the world -- nothing met, not a hit in the player's face */
static void near_layer(void)
{
    const float p[3] = { 0.032f, 0, 0 };
    const Scene paused = { 6.0, 0, 0, 0.0015 };
    Eye e;
    double want, got;
    eye_make(&e, ident, p, 1.0f);
    got = shot(&e, &paused, 1.0f, 1, (float)(-10 * DEG), (float)(4 * DEG), &want);
    NEAR(got, 0, 0);
}

/* the eye far off the camera, a wall nearer than the walk's start (its
 * VR_SHOT_SPAN texels end short of the near end): met where the ray is as
 * deep as the wall */
static void wall_before_walk(void)
{
    const float p[3] = { 0.5f, 0.3f, 0 };
    const Scene s = { 1.0, 0, 0, 0 };
    Eye e;
    VrShotPlan plan;
    double want, got;
    eye_make(&e, ident, p, 1.0f);
    CHECK(vr_shot_plan(e.vp, 1.0f, EYE, EYE, (float)(-6 * DEG), (float)(-3 * DEG), &plan));
    CHECK(1 / plan.g_hi > 1.2);     /* the walk starts beyond the wall */
    got = shot(&e, &s, 1.0f, 0, (float)(-6 * DEG), (float)(-3 * DEG), &want);
    NEAR(got, want, want * 0.002);
}

/* ── A ray from anywhere: the gun's muzzle (vr_shot_plan_ray) ─────────── */

/* Each segment's box as the eye drew the scene, at its offset. */
static void draw_ray(const Eye *e, const Scene *s, const VrShotRay *r, float k, int d24, unsigned char *out)
{
    for (uint32_t j = 0; j < r->segs; j++) {
        VrShotPlan b = r->line;
        b.x0 = r->box[j].x0;
        b.y0 = r->box[j].y0;
        b.w = r->box[j].w;
        b.h = r->box[j].h;
        CHECK(b.w >= 1 && b.h >= 1 && b.w <= VR_SHOT_SEG_BOX && b.h <= VR_SHOT_SEG_BOX);
        CHECK(b.x0 >= 0 && b.y0 >= 0 && b.x0 + b.w <= (uint32_t)ceil(k * EYE) && b.y0 + b.h <= (uint32_t)ceil(k * EYE));
        draw_box(e, s, &b, k, d24, out + j * VR_SHOT_SEG_BYTES);
    }
}

/* The ray o + t d (game units) through the eye's depth: the march's t
 * against the scene's own along it; -2: no walk. */
static double ray(const Eye *e, const Scene *s, float k, int d24, const float o[3], const float d[3], double *want,
                  VrShotRay *out)
{
    static unsigned char buf[VR_SHOT_SEGS * VR_SHOT_SEG_BYTES];
    VrShotRay r;
    double od[3], dd[3], n = 0;
    for (int i = 0; i < 3; i++) { od[i] = o[i]; dd[i] = d[i]; n += dd[i] * dd[i]; }
    for (int i = 0; i < 3; i++) dd[i] /= sqrt(n);
    *want = scene_hit(s, od, dd);
    if (!vr_shot_plan_ray(e->vp, k, EYE, EYE, o, d, &r))
        return -2;
    CHECK(r.segs >= 1 && r.segs <= VR_SHOT_SEGS && r.line.steps <= VR_SHOT_SEG * VR_SHOT_SEGS);
    draw_ray(e, s, &r, k, d24, buf);
    if (out)
        *out = r;
    return vr_shot_march_ray(&r, buf, d24);
}

/* HOTD3's scale (7.5 units a metre): the muzzle 30 cm right of and 35 cm
 * below the eye, 60 cm ahead, its ray to a wall 3 m out -- and from the
 * game camera (o 0) as the rifle's walk finds it */
static void ray_from_muzzle(void)
{
    const float u = 7.5f, p[3] = { 0.032f, 0, 0 };
    const float o[3] = { 0.3f * u, -0.35f * u, -0.6f * u }, d[3] = { -0.08f, 0.1f, -1.0f };
    const float cam[3] = { 0, 0, 0 };
    const Scene s = { 3.0 * u, 0, 0, 0 };
    Eye e;
    double want, got;
    eye_make(&e, ident, p, u);
    for (int d24 = 0; d24 < 2; d24++) {
        got = ray(&e, &s, 1.0f, d24, o, d, &want, NULL);
        CHECK(want > 0);
        NEAR(got, want, want * 0.003);
        got = ray(&e, &s, 0.8f, d24, o, d, &want, NULL);
        NEAR(got, want, want * 0.004);
        got = ray(&e, &s, 1.0f, d24, cam, d, &want, NULL);
        NEAR(got, want, want * 0.002);
    }
}

/* The muzzle half a metre off the eye: the ray's line hundreds of texels
 * long, read in many segments -- a zombie 1.4 m out (a post across the
 * view) met where the rifle's span-limited walk would only guess it */
static void ray_long_line(void)
{
    const float u = 7.5f, p[3] = { -0.032f, 0, 0 };
    const float o[3] = { 0.45f * u, -0.3f * u, -0.5f * u }, d[3] = { -0.25f, 0.18f, -1.0f };
    const Scene s = { 6.0 * u, 1.4 * u, -100.0, 0 };
    Eye e;
    VrShotRay r;
    double want, got;
    eye_make(&e, ident, p, u);
    got = ray(&e, &s, 1.0f, 1, o, d, &want, &r);
    CHECK(r.segs >= 6);
    NEAR(want, (1.4 * u - 0.5 * u) / (1.0 / sqrt(0.25 * 0.25 + 0.18 * 0.18 + 1)), 1e-4);
    NEAR(got, want, want * 0.005);
}

/* The post's edge (x 0, 3 m out; the wall at 8 m) from a muzzle 30 cm to
 * the left: its ray crossing the post's plane 5 cm left of the edge meets
 * the wall, 5 cm right of it the post -- the walk follows the muzzle's ray,
 * whose meeting the eye sees from where it is */
static void ray_post_edge(void)
{
    const float u = 7.5f, p[3] = { 0.032f, 0, 0 };
    const float o[3] = { -0.3f * u, -0.2f * u, -0.4f * u };
    const Scene s = { 8.0 * u, 3.0 * u, 0.0, 0 };
    Eye e;
    double want, got;
    eye_make(&e, ident, p, u);
    {   /* left of the edge and on, to the wall 0.5 m left of it */
        const float d[3] = { (-0.5f * u - o[0]) / (8.0f * u + o[2]), 0, -1 };
        got = ray(&e, &s, 1.0f, 0, o, d, &want, NULL);
        CHECK(want > 3.5 * u);      /* the wall */
        NEAR(got, want, want * 0.003);
    }
    {
        const float d[3] = { (0.05f * u - o[0]) / (3.0f * u + o[2]), 0, -1 };
        got = ray(&e, &s, 1.0f, 0, o, d, &want, NULL);
        CHECK(want < 3.5 * u);      /* the post */
        NEAR(got, want, want * 0.006);
    }
}

/* A zombie's arm (a strip 8 cm wide, 1.2 m out) the ray passes beside but,
 * as the eye sees it, behind: the walk goes on to the wall it does meet
 * (5 m out); the rifle's walk -- from the camera, no such parallax -- would
 * stop at the arm. A ray that passes beside the post (x 0 on, 3 m out)
 * and then goes behind it for good (the wall it meets hidden by it): met
 * where it went out of sight, at the post's edge as the eye sees it. */
static void ray_behind_arm(void)
{
    const float u = 7.5f, p[3] = { 0.032f, 0, 0 };
    const float o[3] = { 0.3f * u, -0.35f * u, -0.5f * u };
    const float d[3] = { (-0.4f - 0.3f) * u, (0.3f + 0.35f) * u, (-5.0f + 0.5f) * u };
    const Scene arm = { 5.0 * u, 1.2 * u, 0.02 * u, 0, 0.08 * u };
    Eye e;
    double want, got;
    eye_make(&e, ident, p, u);
    for (int d24 = 0; d24 < 2; d24++) {
        got = ray(&e, &arm, 1.0f, d24, o, d, &want, NULL);
        CHECK(want > 4.0 * u);      /* it meets the wall */
        NEAR(got, want, want * 0.003);
    }
    {   /* across the post's plane 5 cm left of its edge */
        const float m[3] = { -0.3f * u, -0.2f * u, -0.4f * u }, s = 0.25f / 2.6f, h[3] = { s, 0, -1 };
        const Scene post = { 8.0 * u, 3.0 * u, 0.0, 0, 0 };
        got = ray(&e, &post, 1.0f, 0, m, h, &want, NULL);
        CHECK(want > 3.5 * u);      /* the wall, out of the eye's sight */
        CHECK(got > 0 && got < want);
        {   /* the point beyond the post (out of sight a step on), at its edge as the eye sees it */
            const double n = sqrt(s * s + 1.0), q[3] = { m[0] + got * s / n, m[1], m[2] - got / n };
            CHECK(-q[2] > 3.0 * u + VR_SHOT_THICK * u);
            NEAR((q[0] - p[0] * u) / -q[2], (0.0 - p[0] * u) / (3.0 * u), 0.003);
        }
    }
}

/* A picture over the world 27 cm from the eye (House of the Dead III's
 * shot flashes, drawn into the eyes) across the near part of the ray's
 * line, within VR_SHOT_THICK of the walk's start: seen through -- the wall
 * beyond is met; over the whole line: not known (-1) */
static void ray_near_picture(void)
{
    const float u = 7.5f, p[3] = { 0.032f, 0, 0 };
    const float o[3] = { 0.3f * u, -0.35f * u, -0.5f * u };
    const float d[3] = { (-0.4f - 0.3f) * u, (0.3f + 0.35f) * u, (-5.0f + 0.5f) * u };
    const Scene flash = { 5.0 * u, 0.27 * u, 0.15 * u, 0, 0.2 * u }, cover = { 5.0 * u, 0, 0, 0.0035, 0 };
    Eye e;
    double want, got;
    eye_make(&e, ident, p, u);
    for (int d24 = 0; d24 < 2; d24++) {
        got = ray(&e, &flash, 1.0f, d24, o, d, &want, NULL);
        CHECK(want > 4.0 * u);
        NEAR(got, want, want * 0.003);
    }
    got = ray(&e, &cover, 1.0f, 1, o, d, &want, NULL);
    NEAR(got, -1, 0);           /* not known: the last answer stands */
}

/* the sky (nothing drawn): nothing met; a ray behind the eye, or with no
 * direction or a NaN: no walk */
static void ray_nothing(void)
{
    const float u = 7.5f, p[3] = { 0.032f, 0, 0 }, o[3] = { 0.2f * u, -0.3f * u, -0.5f * u };
    const float ahead[3] = { 0, 0.05f, -1 }, back[3] = { 0, 0, 1 }, none[3] = { 0, 0, 0 }, bad[3] = { NAN, 0, -1 };
    const Scene sky = { 0, 0, 0, 0 };
    Eye e;
    VrShotRay r;
    double want, got;
    eye_make(&e, ident, p, u);
    got = ray(&e, &sky, 1.0f, 1, o, ahead, &want, NULL);
    CHECK(want == 0);
    NEAR(got, 0, 0);
    CHECK(!vr_shot_plan_ray(e.vp, 1.0f, EYE, EYE, o, back, &r));
    CHECK(!vr_shot_plan_ray(e.vp, 1.0f, EYE, EYE, o, none, &r));
    CHECK(!vr_shot_plan_ray(e.vp, 1.0f, EYE, EYE, o, bad, &r));
}

int main(void)
{
    ray_from_muzzle();
    ray_long_line();
    ray_post_edge();
    ray_nothing();
    ray_behind_arm();
    ray_near_picture();
    wall_ahead();
    quest_scale();
    post_edge();
    nothing_met();
    long_line();
    near_layer();
    wall_before_walk();
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("all passed\n");
    return 0;
}
