/* vr_rifle_test.c - checks of runtime/src/host/vr_rifle.c, the VR rifle's
 * maths (scripts/vr-rifle-test.sh): aim angles, tremor steadying, zoom; and
 * of its model: the M700 and the scope lathed round the lens. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "vr_rifle.h"
#include "quest_overlay.h"
#include <string.h>
#include "quest_gun_geometry.inc"
#include "quest_rifle_geometry.inc"
#include "quest_overlay_math.inc"
#include "quest_rifle_model.inc"
#include "quest_rifle_scope.inc"
#include "../runtime/src/gpu/vulkan/vk_jpeg.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b, e) do { double _a = (a), _b = (b); if (!(fabs(_a - _b) <= (e))) { \
    printf("FAIL %s:%d: %s = %g, want %g (+-%g)\n", __FILE__, __LINE__, #a, _a, _b, (double)(e)); fails++; } } while (0)

#define PI 3.14159265358979323846
#define DT (1.0f / 72.0f)
#define DEG (PI / 180.0)

static void aim_angles_basic(void)
{
    float y, p;
    const float fwd[3] = {0, 0, -1}, r[3] = {1, 0, -1}, u[3] = {0, 1, -1};
    vr_aim_angles(fwd, &y, &p); NEAR(y, 0, 1e-5); NEAR(p, 0, 1e-5);
    vr_aim_angles(r, &y, &p);   NEAR(y, PI / 4, 1e-5); NEAR(p, 0, 1e-5);
    vr_aim_angles(u, &y, &p);   NEAR(p, PI / 4, 1e-5); NEAR(y, 0, 1e-5);
}

static void aim_angles_poles_and_behind(void)
{
    float y, p;
    const float up[3] = {0, 1, 0}, dn[3] = {0, -1, 0}, bk[3] = {0, 0, 1}, z[3] = {0, 0, 0};
    vr_aim_angles(up, &y, &p); NEAR(p, PI / 2, 1e-5); CHECK(isfinite(y));
    vr_aim_angles(dn, &y, &p); NEAR(p, -PI / 2, 1e-5); CHECK(isfinite(y));
    vr_aim_angles(bk, &y, &p); NEAR(fabs(y), PI, 1e-5); CHECK(isfinite(p));
    vr_aim_angles(z, &y, &p);  NEAR(y, 0, 0); NEAR(p, 0, 0);
    CHECK(isfinite(y) && isfinite(p));
}

static void yaw_dir(double a, float d[3]) { d[0] = (float)sin(a); d[1] = 0; d[2] = (float)-cos(a); }

static void steady_passes_first_and_still(void)
{
    VrSteady s = {{0}, {0}, {0}, 0};
    const float d[3] = {0.6f, 0.0f, -0.8f};
    float o[3];
    vr_steady(&s, d, DT, 1.0f, o);
    for (int i = 0; i < 3; i++) NEAR(o[i], d[i], 1e-6);
    for (int n = 0; n < 100; n++) vr_steady(&s, d, DT, 1.0f, o);
    for (int i = 0; i < 3; i++) NEAR(o[i], d[i], 1e-5);
}

/* peak |yaw| of the output over the last second of a 0.2 deg, 8 Hz wobble */
static double wobble_out(float zoom, double *in_amp)
{
    VrSteady s = {{0}, {0}, {0}, 0};
    double peak = 0, ipeak = 0;
    float d[3], o[3];
    for (int n = 0; n < 144; n++) {
        double a = 0.2 * DEG * sin(2 * PI * 8.0 * n * DT);
        yaw_dir(a, d);
        vr_steady(&s, d, DT, zoom, o);
        if (n >= 72) {
            float y, p;
            vr_aim_angles(o, &y, &p);
            if (fabs(y) > peak) peak = fabs(y);
            if (fabs(a) > ipeak) ipeak = fabs(a);
        }
    }
    *in_amp = ipeak;
    return peak;
}

static void steady_smooths_tremor_more_at_zoom(void)
{
    double in, w1 = wobble_out(1.0f, &in), w4 = wobble_out(4.0f, &in);
    printf("wobble: in %.4f deg, zoom1 %.4f deg, zoom4 %.4f deg\n", in / DEG, w1 / DEG, w4 / DEG);
    CHECK(w1 < in);
    CHECK(w4 < w1);
}

static void steady_follows_fast_swing(void)
{
    VrSteady s = {{0}, {0}, {0}, 0};
    float d[3], o[3], y, p;
    double a = 0;
    for (int n = 0; n < 36; n++) {   /* 0.5 s */
        a = 90.0 * DEG * n * DT;
        yaw_dir(a, d);
        vr_steady(&s, d, DT, 1.0f, o);
    }
    vr_aim_angles(o, &y, &p);
    printf("sweep lag: %.3f deg\n", (a - y) / DEG);
    CHECK((a - y) / DEG < 3.0);
}

static void steady_ignores_bad_dt(void)
{
    VrSteady a = {{0}, {0}, {0}, 0}, b = {{0}, {0}, {0}, 0};
    float d[3], o1[3], o2[3], held[3];
    const float bad[2] = {0.0f, NAN};
    for (int n = 0; n < 10; n++) {
        yaw_dir(n * 0.01, d);
        vr_steady(&a, d, DT, 1.0f, o1);
        vr_steady(&b, d, DT, 1.0f, o2);
    }
    yaw_dir(0.5, d);
    for (int k = 0; k < 2; k++) {
        vr_steady(&a, d, bad[k], 1.0f, held);
        for (int i = 0; i < 3; i++) NEAR(held[i], o1[i], 1e-6);
    }
    vr_steady(&a, d, -1.0f, 1.0f, held);
    for (int i = 0; i < 3; i++) NEAR(held[i], o1[i], 1e-6);
    vr_steady(&a, d, DT, 1.0f, o1);
    vr_steady(&b, d, DT, 1.0f, o2);
    for (int i = 0; i < 3; i++) { CHECK(isfinite(o1[i])); NEAR(o1[i], o2[i], 0); }
}

static void steady_ignores_nonfinite_dir(void)
{
    VrSteady a = {{0}, {0}, {0}, 0}, b = {{0}, {0}, {0}, 0};
    float d[3], o1[3], o2[3];
    const float nan3[3] = {NAN, 0, -1}, inf3[3] = {0, INFINITY, -1};
    vr_steady(&a, nan3, DT, 1.0f, o1);   /* first call, bad: (0,0,-1) */
    NEAR(o1[0], 0, 0); NEAR(o1[1], 0, 0); NEAR(o1[2], -1, 0);
    for (int n = 0; n < 10; n++) {
        yaw_dir(n * 0.01, d);
        vr_steady(&a, d, DT, 1.0f, o1);
        vr_steady(&b, d, DT, 1.0f, o2);
    }
    vr_steady(&a, nan3, DT, 1.0f, d);
    for (int i = 0; i < 3; i++) NEAR(d[i], o1[i], 1e-6);
    vr_steady(&a, inf3, DT, 1.0f, d);
    for (int i = 0; i < 3; i++) NEAR(d[i], o1[i], 1e-6);
    yaw_dir(0.2, d);
    vr_steady(&a, d, DT, 1.0f, o1);
    vr_steady(&b, d, DT, 1.0f, o2);
    for (int i = 0; i < 3; i++) { CHECK(isfinite(o1[i])); NEAR(o1[i], o2[i], 0); }
}

static void zoom_step_range(void)
{
    float z = 1;
    for (int n = 0; n < 72; n++) z = vr_zoom_step(z, 1.0f, DT);
    NEAR(z, 2.0, 0.02);
    for (int n = 0; n < 720; n++) z = vr_zoom_step(z, 1.0f, DT);
    NEAR(z, 4.0, 1e-6);
    for (int n = 0; n < 720; n++) z = vr_zoom_step(z, -1.0f, DT);
    NEAR(z, 0.5, 1e-6);
    NEAR(vr_zoom_step(1.7f, 0.1f, DT), 1.7f, 0);
}

/* Silent Scope's scope at zoom 1 drawn 3 times (its default guard) wider
 * than the lens shows: focal 296.8 * 45.583 px of its 256 / 3, in picture
 * widths (games/ss1/src/vr_rifle.c SCOPE_PROJ) */
#define LENS_G 3.0f
#define LENS_F (296.8f * 45.583f / 256.0f / LENS_G)

static void lens_crop_still(void)
{
    const VrLensAim a = {0.3f, -0.1f, LENS_F, LENS_G, 1};
    float c[3];
    CHECK(vr_lens_crop(&a, 0.3f, -0.1f, c) == 0);
    NEAR(c[0], 0.5, 1e-6); NEAR(c[1], 0.5, 1e-6); NEAR(c[2], 0.5 / LENS_G, 1e-6);
}

/* Turned by half the guard band (1 - 1/G) / 2 since the picture: the crop
 * at that edge, not past it; right moves it right, up moves it up (v down). */
static void lens_crop_half_guard(void)
{
    const VrLensAim a = {0, 0, LENS_F, LENS_G, 1};
    const double h = 0.5 / LENS_G, band = 0.5 - h, d = atan(band / LENS_F);
    float c[3];
    vr_lens_crop(&a, (float)d, 0, c);
    NEAR(c[0], 1 - h, 1e-5); NEAR(c[1], 0.5, 1e-6); NEAR(c[2], h, 1e-6);
    vr_lens_crop(&a, (float)-d, 0, c);
    NEAR(c[0], h, 1e-5); NEAR(c[1], 0.5, 1e-6);
    vr_lens_crop(&a, 0, (float)d, c);
    NEAR(c[0], 0.5, 1e-6); NEAR(c[1], h, 1e-5);
    vr_lens_crop(&a, 0, (float)-d, c);
    NEAR(c[1], 1 - h, 1e-5);
    /* half of that: half way (tangents add exactly about the centre) */
    vr_lens_crop(&a, (float)atan(band / 2 / LENS_F), 0, c);
    NEAR(c[0], 0.5 + band / 2, 1e-5);
}

/* A fast swing past the band: clamped to the edge, never outside [0, 1] */
static void lens_crop_clamps(void)
{
    const VrLensAim a = {0.2f, 0.1f, LENS_F, LENS_G, 1};
    const float turns[6][2] = {{0.8f, 0.1f}, {-0.6f, 0.1f}, {0.2f, 0.9f}, {0.2f, -0.7f}, {1.2f, -0.9f}, {3.3f, 0.1f}};
    float c[3];
    for (int i = 0; i < 6; i++) {
        CHECK(vr_lens_crop(&a, turns[i][0], turns[i][1], c) == 1);
        for (int k = 0; k < 2; k++) {
            CHECK(isfinite(c[k]));
            CHECK(c[k] - c[2] >= -1e-6f && c[k] + c[2] <= 1 + 1e-6f);
        }
    }
    vr_lens_crop(&a, 0.8f, 0.1f, c);
    NEAR(c[0], 1 - 0.5 / LENS_G, 1e-6);
    vr_lens_crop(&a, 0.2f, 0.9f, c);
    NEAR(c[1], 0.5 / LENS_G, 1e-6);
}

/* Zoom: the focal (and so the pixels a radian) scales the offset */
static void lens_crop_zoom_scales(void)
{
    const VrLensAim z1 = {0, 0, LENS_F, LENS_G, 1}, z2 = {0, 0, 2 * LENS_F, LENS_G, 1};
    float c1[3], c2[3];
    vr_lens_crop(&z1, 0.002f, -0.004f, c1);
    vr_lens_crop(&z2, 0.002f, -0.004f, c2);
    NEAR(c2[0] - 0.5, 2 * (c1[0] - 0.5), 1e-6);
    NEAR(c2[1] - 0.5, 2 * (c1[1] - 0.5), 1e-6);
    NEAR(c1[0] - 0.5, LENS_F * tan(0.002), 1e-5);
    CHECK(fabs(c2[1] - 0.5) < 0.5 - c2[2]);   /* inside the band: not clamped */
    NEAR(c1[2], c2[2], 0);
}

/* Pitched: a yaw turn moves the picture by yaw * cos(pitch) across */
static void lens_crop_pitched(void)
{
    const VrLensAim a = {0, 0.5f, LENS_F, LENS_G, 1};
    float c[3];
    vr_lens_crop(&a, 0.004f, 0.5f, c);
    NEAR(c[0] - 0.5, LENS_F * 0.004 * cos(0.5), 1e-4);
}

/* A picture the game aimed (the rifle off, its reticle in the picture): its
 * centre at the guard its focal length has -- not following the rifle,
 * however far it points; with no guard in it (1), the whole picture */
static void lens_crop_not_followed(void)
{
    const VrLensAim g = {0.1f, 0.0f, LENS_F, LENS_G, 0}, one = {0.1f, 0.0f, 3 * LENS_F, 1.0f, 0};
    float c[3];
    CHECK(vr_lens_crop(&g, 0.1004f, 0.0f, c) == 0);
    NEAR(c[0], 0.5, 0); NEAR(c[1], 0.5, 0); NEAR(c[2], 0.5 / LENS_G, 1e-6);
    CHECK(vr_lens_crop(&g, 0.9f, -0.4f, c) == 0);
    NEAR(c[0], 0.5, 0); NEAR(c[1], 0.5, 0); NEAR(c[2], 0.5 / LENS_G, 1e-6);
    CHECK(vr_lens_crop(&one, 0.1004f, 0.0f, c) == 0);
    NEAR(c[0], 0.5, 0); NEAR(c[1], 0.5, 0); NEAR(c[2], 0.5, 0);
}

/* No aim known: the whole picture as drawn */
static void lens_crop_unknown(void)
{
    const VrLensAim none[4] = {{0, 0, 0, 0, 1}, {0, 0, LENS_F, 0.5f, 1}, {NAN, 0, LENS_F, LENS_G, 1}, {0, 0, -1, LENS_G, 1}};
    float c[3];
    for (int i = 0; i < 4; i++) {
        CHECK(vr_lens_crop(&none[i], 0.4f, 0.1f, c) == 0);
        NEAR(c[0], 0.5, 0); NEAR(c[1], 0.5, 0); NEAR(c[2], 0.5, 0);
    }
    const VrLensAim a = {0, 0, LENS_F, LENS_G, 1};
    CHECK(vr_lens_crop(&a, NAN, 0, c) == 0);
    NEAR(c[0], 0.5, 0); NEAR(c[2], 0.5 / LENS_G, 1e-6);
}

/* The scope's scales checked: the default and the play test's range */
static const float SCALES[] = {0.5f, 1.0f, 1.5f, 2.0f};
#define NSCALES (int)(sizeof SCALES / sizeof *SCALES)

static void rifle_lens_on_scope_line(void)
{
    for (int k = 0; k < NSCALES; k++) {
        const float s = SCALES[k];
        Box sc[RIFLE_SCOPE_N];
        float c[3], r;
        rifle_scope(s, sc);
        rifle_lens(s, c, &r);
        const Box *tube = &sc[0], *bell = &sc[2];
        /* on the tube's axis, the sight line's height above the bore */
        NEAR(c[0], tube->c[0], 1e-6);
        NEAR(c[1], tube->c[1], 1e-6);
        NEAR(c[1], rifle_scope_y(s), 1e-6);
        /* at the tube's rear end: past the tube, on the rear bell's back face */
        CHECK(c[2] >= tube->c[2] + tube->h[2]);
        NEAR(c[2], bell->c[2] + bell->h[2], 0.002);
        CHECK(c[2] >= bell->c[2] + bell->h[2]);
        /* behind the receiver, and the disc inside the bell */
        CHECK(c[2] >= RIFLE_RECEIVER.c[2] + RIFLE_RECEIVER.h[2]);
        CHECK(r <= bell->h[0] && r <= bell->h[1]);
        NEAR(r, RIFLE_LENS_R * s, 1e-6);
    }
    /* 2.5 times the first rifle's 0.024 m */
    float c[3], r;
    rifle_lens(1, c, &r);
    NEAR(r, 0.06, 1e-6);
}

/* The lens disc (plane z = c[2], radius r) must not touch any rifle box: no
 * box spans its plane within reach of the disc. */
static int lens_hits(const float c[3], float r, const Box *b)
{
    if (c[2] > b->c[2] + b->h[2] || c[2] < b->c[2] - b->h[2]) return 0;
    float dx = fmaxf(fabsf(c[0] - b->c[0]) - b->h[0], 0);
    float dy = fmaxf(fabsf(c[1] - b->c[1]) - b->h[1], 0);
    return dx * dx + dy * dy < r * r;
}

static void rifle_lens_unobstructed(void)
{
    for (int k = 0; k < NSCALES; k++) {
        Box sc[RIFLE_SCOPE_N];
        float c[3], r;
        rifle_scope(SCALES[k], sc);
        rifle_lens(SCALES[k], c, &r);
        for (unsigned i = 0; i < sizeof RIFLE_BODY / sizeof *RIFLE_BODY; i++) CHECK(!lens_hits(c, r, &RIFLE_BODY[i]));
        for (unsigned i = 0; i < RIFLE_SCOPE_N; i++) CHECK(!lens_hits(c, r, &sc[i]));
    }
}

static int boxes_overlap(const Box *a, const Box *b)
{
    for (int i = 0; i < 3; i++)
        if (fabsf(a->c[i] - b->c[i]) >= a->h[i] + b->h[i] - 1e-6f) return 0;
    return 1;
}

/* On the rifle's top, clear of the stock and everything else: no scope box
 * overlaps a body box; the mounts stand on the receiver and meet the tube. */
static void rifle_scope_on_top(void)
{
    for (int k = 0; k < NSCALES; k++) {
        Box sc[RIFLE_SCOPE_N];
        rifle_scope(SCALES[k], sc);
        for (unsigned i = 0; i < RIFLE_SCOPE_N; i++)
            for (unsigned j = 0; j < sizeof RIFLE_BODY / sizeof *RIFLE_BODY; j++) CHECK(!boxes_overlap(&sc[i], &RIFLE_BODY[j]));
        for (unsigned i = 0; i < RIFLE_SCOPE_N; i++) CHECK(sc[i].c[1] - sc[i].h[1] >= RIFLE_RECEIVER_TOP - 1e-6f);
        NEAR(RIFLE_RECEIVER.c[1] + RIFLE_RECEIVER.h[1], RIFLE_RECEIVER_TOP, 1e-6);
        for (int m = 4; m < 6; m++) {
            NEAR(sc[m].c[1] - sc[m].h[1], RIFLE_RECEIVER_TOP, 1e-6);
            NEAR(sc[m].c[1] + sc[m].h[1], sc[0].c[1] - sc[0].h[1], 1e-6);
            CHECK(sc[m].h[1] > 0);
        }
    }
}

static void rifle_fits_hand(void)
{
    float zmin = 1e9f, zmax = -1e9f;
    const Box *grip = &RIFLE_BODY[0];
    for (unsigned i = 0; i < sizeof RIFLE_BODY / sizeof *RIFLE_BODY; i++) {
        if (RIFLE_BODY[i].c[2] - RIFLE_BODY[i].h[2] < zmin) zmin = RIFLE_BODY[i].c[2] - RIFLE_BODY[i].h[2];
        if (RIFLE_BODY[i].c[2] + RIFLE_BODY[i].h[2] > zmax) zmax = RIFLE_BODY[i].c[2] + RIFLE_BODY[i].h[2];
    }
    CHECK(zmax - zmin >= 0.9 && zmax - zmin <= 1.15);
    NEAR(zmin, RIFLE_BOX_MUZZLE_Z, 1e-6);
    for (int a = 0; a < 3; a++) CHECK(fabsf(grip->c[a]) <= grip->h[a] + 0.03f);
    CHECK(sizeof RIFLE_BODY / sizeof *RIFLE_BODY + RIFLE_SCOPE_N <= 64);
}

/* ── The detailed rifle: the M700 (third_party/rifle, embedded) and its scope ── */

static RifleModel m700;
static int m700_ok = -1;
static RifleScopeMesh scope;            /* big: static */

static int m700_load(void)
{
    if (m700_ok < 0)
        m700_ok = rifle_model_unpack(rifle_m700_mesh, (size_t)(rifle_m700_mesh_end - rifle_m700_mesh), &m700);
    return m700_ok;
}

static float m700_top(void)
{
    float top = -1e9f;
    for (int i = 0; i < m700.nv; i++) top = fmaxf(top, m700.p[i * 3 + 1]);
    return top;
}

/* The nearest hit of a ray on triangle abc (Moller-Trumbore), or -1. */
static double ray_tri(const double o[3], const double d[3], const float *a, const float *b, const float *c)
{
    double e1[3], e2[3], p[3], s[3], q[3];
    for (int i = 0; i < 3; i++) { e1[i] = b[i] - a[i]; e2[i] = c[i] - a[i]; s[i] = o[i] - a[i]; }
    p[0] = d[1] * e2[2] - d[2] * e2[1]; p[1] = d[2] * e2[0] - d[0] * e2[2]; p[2] = d[0] * e2[1] - d[1] * e2[0];
    double det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (fabs(det) < 1e-15) return -1;
    double u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) / det;
    q[0] = s[1] * e1[2] - s[2] * e1[1]; q[1] = s[2] * e1[0] - s[0] * e1[2]; q[2] = s[0] * e1[1] - s[1] * e1[0];
    double v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) / det;
    double t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) / det;
    return u >= 0 && v >= 0 && u + v <= 1 && t > 0 ? t : -1;
}

/* Along a ray from o: how many of the M700's triangles it crosses, and the nearest. */
static int m700_ray(const double o[3], const double d[3], double *nearest)
{
    int hits = 0;
    *nearest = INFINITY;
    for (int t = 0; t < m700.ni / 3; t++) {
        const uint16_t *k = &m700.idx[t * 3];
        double h = ray_tri(o, d, &m700.p[k[0] * 3], &m700.p[k[1] * 3], &m700.p[k[2] * 3]);
        if (h > 0) { hits++; if (h < *nearest) *nearest = h; }
    }
    return hits;
}

static void m700_unpacks(void)
{
    CHECK(m700_load());
    CHECK(m700.nv == 11332 && m700.ni == 3 * 11582);
    CHECK(m700.nv <= 65535);                    /* uint16 indices */
    int bad = 0;
    for (int i = 0; i < m700.ni; i++) bad += m700.idx[i] >= m700.nv;
    for (int i = 0; i < m700.nv; i++) {
        const float *n = &m700.n[i * 3], *uv = &m700.uv[i * 2];
        bad += fabsf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2] - 1) > 1e-4f;
        bad += !(uv[0] >= 0 && uv[0] <= 1 && uv[1] >= 0 && uv[1] <= 1);
    }
    CHECK(bad == 0);
}

/* The fallback's trigger: data that is not the mesh unpacks to nothing. */
static void m700_rejects_bad_data(void)
{
    const size_t size = (size_t)(rifle_m700_mesh_end - rifle_m700_mesh);
    unsigned char *b = malloc(size + 16);
    RifleModel m;
    if (!b) { CHECK(0); return; }
    memcpy(b, rifle_m700_mesh, size);
    CHECK(rifle_model_unpack(b, size, &m) == 1);
    rifle_model_free(&m);
    CHECK(rifle_model_unpack(b, size - 1, &m) == 0 && !m.p);             /* truncated */
    CHECK(rifle_model_unpack(b, size + 1, &m) == 0 && !m.p);             /* trailing bytes */
    CHECK(rifle_model_unpack(b, 0, &m) == 0 && !m.p);
    CHECK(rifle_model_unpack(NULL, size, &m) == 0 && !m.p);
    b[0] = 'X';
    CHECK(rifle_model_unpack(b, size, &m) == 0 && !m.p);                 /* not its magic */
    b[0] = rifle_m700_mesh[0];
    const uint32_t nv = 11332;
    memcpy(b + size - 2, &nv, 2);                                       /* the last index past the vertices */
    CHECK(rifle_model_unpack(b, size, &m) == 0 && !m.p);
    memcpy(b, rifle_m700_mesh, size);
    b[8] = 1;                                                           /* indices not whole triangles */
    CHECK(rifle_model_unpack(b, size, &m) == 0 && !m.p);
    memcpy(b, rifle_m700_mesh, size);
    const float nan = NAN;
    memcpy(b + 12, &nan, 4);                                            /* a position scale not a number */
    CHECK(rifle_model_unpack(b, size, &m) == 0 && !m.p);
    free(b);
}

/* The laser leaves from the barrel's tip; the controller's grip point is in the pistol grip. */
static void m700_muzzle_and_grip(void)
{
    if (!m700_load()) { CHECK(0); return; }
    float zmin = 1e9f, lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
    for (int i = 0; i < m700.nv; i++) zmin = fminf(zmin, m700.p[i * 3 + 2]);
    for (int i = 0; i < m700.nv; i++)
        if (m700.p[i * 3 + 2] < zmin + 0.003f)
            for (int k = 0; k < 2; k++) { lo[k] = fminf(lo[k], m700.p[i * 3 + k]); hi[k] = fmaxf(hi[k], m700.p[i * 3 + k]); }
    NEAR(RIFLE_MUZZLE_Z, zmin, 0.002);
    NEAR(0, (lo[0] + hi[0]) / 2, 0.002);
    NEAR(RIFLE_MUZZLE_Y, (lo[1] + hi[1]) / 2, 0.002);
    /* inside the model (an odd number of crossings every way along the axes),
     * the grip's sides within 2 cm either side */
    static const double dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const double origin[3] = {0, 0, 0};
    double side[6];
    for (int k = 0; k < 6; k++) CHECK(m700_ray(origin, dirs[k], &side[k]) % 2 == 1);
    CHECK(side[0] <= 0.02 && side[1] <= 0.02);
}

/* The colour texture decodes (the fallback when it does not: the box rifle). */
static void m700_texture_decodes(void)
{
    uint32_t w = 0, h = 0, *px = ch_jpeg_argb(rifle_m700_jpg, (size_t)(rifle_m700_jpg_end - rifle_m700_jpg), &w, &h);
    CHECK(px && w == 1024 && h == 1024);
    if (px) {
        double sum[3] = {0, 0, 0};
        int opaque = 1;
        for (uint32_t i = 0; i < w * h; i++) {
            opaque &= px[i] >> 24 == 255;
            for (int k = 0; k < 3; k++) sum[k] += (px[i] >> (16 - 8 * k)) & 255;
        }
        CHECK(opaque);
        CHECK(sum[0] > sum[1] && sum[1] > sum[2]);  /* walnut and blued steel: red over green over blue */
        free(px);
    }
    unsigned char junk[4096];
    for (int i = 0; i < (int)sizeof junk; i++) junk[i] = (unsigned char)(i * 37 + 11);
    junk[0] = 0xff; junk[1] = 0xd8;                 /* a JPEG's start, then nonsense */
    CHECK(ch_jpeg_argb(junk, sizeof junk, &w, &h) == NULL);
    CHECK(ch_jpeg_argb(rifle_m700_jpg, 64, &w, &h) == NULL);
}

/* The lens stays as approved (z, radius); the scope's eyepiece is round it,
 * on its axis, and nothing of the scope is behind it. */
static void scope_round_the_lens(void)
{
    for (int k = 0; k < NSCALES; k++) {
        const float s = SCALES[k];
        float c[3], r;
        rifle_lens(s, c, &r);
        NEAR(c[0], 0, 0); NEAR(c[2], 0.071, 1e-6); NEAR(r, RIFLE_LENS_R * s, 1e-6);
        CHECK(rifle_scope_mesh(s, &scope));
        CHECK(scope.nv > 0 && scope.nv <= 65535 && scope.ni % 3 == 0 && scope.ni > 0);
        int bad = 0, n = 0;
        double rim = 0, cx = 0, cy = 0, zmax = -1;
        for (int i = 0; i < scope.ni; i++) bad += scope.idx[i] >= scope.nv;
        for (int i = 0; i < scope.nv; i++) {
            const float *p = scope.p[i];
            zmax = fmax(zmax, p[2]);
            if (p[2] < 0.030f) continue;            /* the eyepiece's housing */
            rim = fmax(rim, hypot(p[0], p[1] - c[1]));
            cx += p[0]; cy += p[1]; n++;
        }
        CHECK(bad == 0);
        CHECK(n > 0);
        CHECK(rim >= r);
        NEAR(rim, RIFLE_REAR_R * s, 1e-4);
        NEAR(cx / n, 0, 1e-4); NEAR(cy / n, rifle_scope_y(s), 1e-4);
        CHECK(zmax < c[2]);
    }
}

/* Does triangle t reach into the cylinder of radius r round the axis (cx, cy)
 * from z0 to z1? Clipped to the slab, then the polygon against the circle. */
static int tri_in_cylinder(const float *t0, const float *t1, const float *t2, double cx, double cy, double r,
                           double z0, double z1)
{
    double poly[2][9][3];
    int n = 3, m;
    const float *t[3] = {t0, t1, t2};
    for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) poly[0][i][k] = t[i][k];
    for (int side = 0; side < 2; side++) {      /* keep z >= z0, then z <= z1 */
        double (*in)[3] = poly[side], (*out)[3] = poly[!side];
        m = 0;
        for (int i = 0; i < n; i++) {
            const double *a = in[i], *b = in[(i + 1) % n];
            double da = side ? z1 - a[2] : a[2] - z0, db = side ? z1 - b[2] : b[2] - z0;
            if (da >= 0) memcpy(out[m++], a, sizeof out[0]);
            if ((da >= 0) != (db >= 0)) {
                double f = da / (da - db);
                for (int k = 0; k < 3; k++) out[m][k] = a[k] + f * (b[k] - a[k]);
                m++;
            }
        }
        n = m;
        if (!n) return 0;
    }
    double (*q)[3] = poly[0];                   /* the second pass's output */
    int inside = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        double ax = q[i][0] - cx, ay = q[i][1] - cy, bx = q[j][0] - cx, by = q[j][1] - cy;
        double ex = bx - ax, ey = by - ay, len = ex * ex + ey * ey;
        double f = len > 0 ? fmax(0, fmin(1, -(ax * ex + ay * ey) / len)) : 0;
        if (hypot(ax + f * ex, ay + f * ey) < r) return 1;      /* an edge within r of the axis */
        if ((ay > 0) != (by > 0) && 0 < ax + (bx - ax) * (0 - ay) / (by - ay)) inside = !inside;
    }
    return inside;                                              /* the axis inside the polygon */
}

/* Nothing of the M700 or the scope between the lens and a point 30 cm behind it on its axis. */
static void rifle_model_lens_unobstructed(void)
{
    if (!m700_load()) { CHECK(0); return; }
    /* the check itself: a triangle across the axis, one beside it, one in front */
    const float across[3][3] = {{-1, 0.10f, 0.2f}, {1, 0.10f, 0.2f}, {0, 0.13f, 0.25f}};
    const float beside[3][3] = {{0.07f, 0, 0.1f}, {0.08f, 0.2f, 0.1f}, {0.07f, 0.2f, 0.3f}};
    const float ahead[3][3] = {{-1, 0.1f, 0.0f}, {1, 0.1f, 0.0f}, {0, 0.13f, 0.06f}};
    CHECK(tri_in_cylinder(across[0], across[1], across[2], 0, 0.12, 0.06, 0.071, 0.371));
    CHECK(!tri_in_cylinder(beside[0], beside[1], beside[2], 0, 0.12, 0.06, 0.071, 0.371));
    CHECK(!tri_in_cylinder(ahead[0], ahead[1], ahead[2], 0, 0.12, 0.06, 0.071, 0.371));
    for (int k = 0; k < NSCALES; k++) {
        float c[3], r;
        int hits = 0;
        rifle_lens(SCALES[k], c, &r);
        for (int t = 0; t < m700.ni / 3; t++) {
            const uint16_t *i = &m700.idx[t * 3];
            hits += tri_in_cylinder(&m700.p[i[0] * 3], &m700.p[i[1] * 3], &m700.p[i[2] * 3], c[0], c[1], r, c[2], c[2] + 0.30);
        }
        CHECK(rifle_scope_mesh(SCALES[k], &scope));
        for (int t = 0; t < scope.ni / 3; t++) {
            const uint16_t *i = &scope.idx[t * 3];
            hits += tri_in_cylinder(scope.p[i[0]], scope.p[i[1]], scope.p[i[2]], c[0], c[1], r, c[2], c[2] + 0.30);
        }
        CHECK(hits == 0);
    }
}

/* Do triangles a and b cross (an edge of either through the other)? */
static int tri_tri(const float *a[3], const float *b[3])
{
    for (int pass = 0; pass < 2; pass++) {
        const float **e = pass ? b : a, **t = pass ? a : b;
        for (int k = 0; k < 3; k++) {
            const float *p = e[k], *q = e[(k + 1) % 3];
            const double o[3] = {p[0], p[1], p[2]}, d[3] = {q[0] - p[0], q[1] - p[1], q[2] - p[2]};
            const double h = ray_tri(o, d, t[0], t[1], t[2]);
            if (h > 0 && h <= 1) return 1;
        }
    }
    return 0;
}

static void tri_box(const float *t[3], float lo[3], float hi[3])
{
    for (int k = 0; k < 3; k++) {
        lo[k] = fminf(t[0][k], fminf(t[1][k], t[2][k]));
        hi[k] = fmaxf(t[0][k], fmaxf(t[1][k], t[2][k]));
    }
}

/* The scope stands on the rail: no triangle of it crosses one of the M700's
 * (at the scales checked), and at every scale it can have, the rings' feet
 * are on the rail's top -- a hair above it, over it, not past its ends --
 * and nothing else of the scope is down there. */
static void scope_clear_of_m700(void)
{
    if (!m700_load()) { CHECK(0); return; }
    const float top = m700_top();
    NEAR(top, M700_RAIL_TOP, 0.0002);
    /* the check itself: the scope at 1 dropped by 1 cm is in the rail (the last pass) */
    static const float CHECKED[] = {0.3f, 0.5f, 1.0f, 1.5f, 2.0f, 1.0f};
    const int NCHECKED = (int)(sizeof CHECKED / sizeof *CHECKED);
    static int near[11582];
    for (int k = 0; k < NCHECKED; k++) {
        if (!rifle_scope_mesh(CHECKED[k], &scope)) { CHECK(0); continue; }
        if (k == NCHECKED - 1)
            for (int i = 0; i < scope.nv; i++) scope.p[i][1] -= 0.01f;
        float slo[3] = {1e9f, 1e9f, 1e9f}, shi[3] = {-1e9f, -1e9f, -1e9f}, lo[3], hi[3];
        for (int i = 0; i < scope.nv; i++)
            for (int a = 0; a < 3; a++) { slo[a] = fminf(slo[a], scope.p[i][a]); shi[a] = fmaxf(shi[a], scope.p[i][a]); }
        int n = 0, crossed = 0;
        for (int t = 0; t < m700.ni / 3 && t < 11582; t++) {   /* the M700's triangles near the scope */
            const uint16_t *i = &m700.idx[t * 3];
            const float *v[3] = {&m700.p[i[0] * 3], &m700.p[i[1] * 3], &m700.p[i[2] * 3]};
            tri_box(v, lo, hi);
            if (hi[0] >= slo[0] && lo[0] <= shi[0] && hi[1] >= slo[1] && lo[1] <= shi[1] && hi[2] >= slo[2] && lo[2] <= shi[2])
                near[n++] = t;
        }
        for (int t = 0; t < scope.ni / 3; t++) {
            const float *a[3] = {scope.p[scope.idx[t * 3]], scope.p[scope.idx[t * 3 + 1]], scope.p[scope.idx[t * 3 + 2]]};
            float alo[3], ahi[3];
            tri_box(a, alo, ahi);
            for (int j = 0; j < n; j++) {
                const uint16_t *i = &m700.idx[near[j] * 3];
                const float *b[3] = {&m700.p[i[0] * 3], &m700.p[i[1] * 3], &m700.p[i[2] * 3]};
                tri_box(b, lo, hi);
                if (hi[0] < alo[0] || lo[0] > ahi[0] || hi[1] < alo[1] || lo[1] > ahi[1] || hi[2] < alo[2] || lo[2] > ahi[2])
                    continue;
                crossed += tri_tri(a, b);
            }
        }
        if (k == NCHECKED - 1) { CHECK(crossed > 0); break; }
        if (crossed) printf("scale %.2f: %d of the scope's triangles cross the M700's\n", CHECKED[k], crossed);
        CHECK(crossed == 0);
    }
    for (float s = 0.3f; s <= 2.0001f; s += 0.05f) {
        if (!rifle_scope_mesh(s, &scope)) { CHECK(0); continue; }
        double foot[2][3] = {{0, 0, 0}, {0, 0, 0}};
        int feet[2] = {0, 0}, off = 0;
        float low = 1e9f;
        for (int i = 0; i < scope.nv; i++) {
            const float *p = scope.p[i];
            if (p[1] > M700_RAIL_TOP + 0.001f || p[2] < M700_RAIL_Z0 || p[2] > M700_RAIL_Z1) continue;
            const int f = p[2] < -0.16f;            /* the front ring's foot, or the rear's */
            for (int k = 0; k < 3; k++) foot[f][k] += p[k];
            feet[f]++;
            low = fminf(low, p[1]);
            off += fabsf(p[0]) > 0.0105f || fabsf(p[2] - SCOPE_RING_Z[f]) > SCOPE_RING_HALF;
        }
        CHECK(off == 0);
        CHECK(low > top && low - M700_RAIL_TOP <= 0.0005f);
        for (int f = 0; f < 2; f++) {
            CHECK(feet[f] > 0);
            if (!feet[f]) continue;
            const double o[3] = {foot[f][0] / feet[f], foot[f][1] / feet[f], foot[f][2] / feet[f]}, down[3] = {0, -1, 0};
            double hit;
            m700_ray(o, down, &hit);
            CHECK(o[1] - hit >= 0.0645);            /* on the rail's top (its slots 0.0651), not past its end */
        }
    }
}

/* The scope's axis rises only for clearance: printed, and near where the user approved it. */
static void scope_axis_height(void)
{
    printf("scope axis (lens centre y): %.4f at 0.5, %.4f at 1, %.4f at 2 (the box rifle's: %.4f at 1)\n",
           rifle_scope_y(0.5f), rifle_scope_y(1), rifle_scope_y(2), 0.042 + 0.001 + 0.0675);
    CHECK(rifle_scope_y(1) - 0.1105f < 0.03f);
    for (float s = 0.3f; s <= 2.0001f; s += 0.05f) CHECK(rifle_scope_y(s) > rifle_scope_y(s - 0.05f));
}

/* rifle_shade lights as the overlay's gun (vk_overlay.c's box path): the
 * normal turned into the world, the same light, the same highlight. */
static void rifle_shade_matches_overlay_lighting(void)
{
    if (!m700_load()) { CHECK(0); return; }
    const float q0[4] = {0.3f, -0.5f, 0.2f, 0.7874f}, p0[3] = {0.1f, 1.2f, -0.4f};
    float q[4], model[16], vp[16] = {0}, mvp[16], out[3][8];
    float qn = sqrtf(q0[0] * q0[0] + q0[1] * q0[1] + q0[2] * q0[2] + q0[3] * q0[3]);
    for (int k = 0; k < 4; k++) q[k] = q0[k] / qn;
    pose_matrix(q, p0, model);
    for (int k = 0; k < 16; k++) vp[k] = (float)((k * 7) % 5) * 0.3f - 0.4f;
    vp[15] = 2;
    mul(mvp, vp, model);
    double worst = 0;
    int ok = 1;
    for (int i = 0; i < m700.nv; i += 97) {
        const int n = i + 3 <= m700.nv ? 3 : m700.nv - i;
        const float rgb[9] = {0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 1, 1, 1};
        ok &= rifle_shade(&m700.p[i * 3], &m700.n[i * 3], rgb, n, mvp, model, &out[0][0], 8, 5);
        for (int v = 0; v < n; v++) {
            const float *pv = &m700.p[(i + v) * 3], *nv = &m700.n[(i + v) * 3];
            float world[3], clip[4], nw[3], len = 0;
            apply(model, pv, world);
            for (int k = 0; k < 3; k++) { nw[k] = model[k] * nv[0] + model[4 + k] * nv[1] + model[8 + k] * nv[2]; len += nw[k] * nw[k]; }
            len = sqrtf(len);
            for (int k = 0; k < 3; k++) nw[k] /= len;
            float diffuse = .38f + .62f * fmaxf(0, (nw[0] * .3f + nw[1] + nw[2] * .4f) / sqrtf(1.25f));
            float spec = powf(fmaxf(0, (-nw[0] * .2f + nw[1] + nw[2] * .6f) / sqrtf(1.4f)), 24) * .18f;
            for (int k = 0; k < 4; k++) clip[k] = vp[k] * world[0] + vp[4 + k] * world[1] + vp[8 + k] * world[2] + vp[12 + k];
            const float want[4] = {clip[0], -clip[1], (clip[2] + clip[3]) * .5f, clip[3]};
            for (int k = 0; k < 4; k++) worst = fmax(worst, fabs(out[v][k] - want[k]) / fmax(1, fabs(want[k])));
            for (int k = 0; k < 3; k++) worst = fmax(worst, fabs(out[v][5 + k] - (rgb[v * 3 + k] * diffuse + spec)));
        }
    }
    CHECK(ok);
    CHECK(worst < 1e-4);
    /* NULL colours: white */
    ok = rifle_shade(m700.p, m700.n, NULL, 1, mvp, model, &out[0][0], 8, 5);
    CHECK(ok && out[0][5] == out[0][6] && out[0][6] == out[0][7] && out[0][5] > 0.37f);
    /* a position not finite, or at w = 0: refused */
    const float inf_p[3] = {INFINITY, 0, 0}, up[3] = {0, 1, 0};
    CHECK(!rifle_shade(inf_p, up, NULL, 1, mvp, model, &out[0][0], 8, 5));
    float flat[16] = {0};
    flat[0] = flat[5] = flat[10] = 1;               /* w always 0 */
    CHECK(!rifle_shade(m700.p, m700.n, NULL, 1, flat, model, &out[0][0], 8, 5));
}

/* The overlay's cost per eye: the M700's and the scope's vertices (printed, not checked). */
static void rifle_shade_timing(void)
{
    if (!m700_load() || !rifle_scope_mesh(1, &scope)) { CHECK(0); return; }
    enum { STRIDE = 31, COL = 27, RUNS = 200 };     /* ChVkColorVertex: 27 floats of position etc., then the colour */
    float *out = calloc((size_t)(m700.nv + scope.nv) * STRIDE, sizeof(float));
    float model[16], vp[16] = {0}, mvp[16];
    const float q[4] = {0, 0, 0, 1}, p[3] = {0, 1.2f, -0.4f};
    if (!out) { CHECK(0); return; }
    pose_matrix(q, p, model);
    vp[0] = vp[5] = 1; vp[10] = -1; vp[11] = -1; vp[14] = -0.1f;
    mul(mvp, vp, model);
    clock_t t0 = clock();
    int ok = 1;
    for (int r = 0; r < RUNS; r++) {
        ok &= rifle_shade(m700.p, m700.n, NULL, m700.nv, mvp, model, out, STRIDE, COL);
        ok &= rifle_shade(&scope.p[0][0], &scope.n[0][0], &scope.c[0][0], scope.nv, mvp, model,
                          out + (size_t)m700.nv * STRIDE, STRIDE, COL);
    }
    double us = (double)(clock() - t0) / CLOCKS_PER_SEC * 1e6 / RUNS;
    CHECK(ok);
    printf("rifle vertices per eye: %d (M700 %d, scope %d, %d + %d triangles): %.1f us (this machine, -O1)\n",
           m700.nv + scope.nv, m700.nv, scope.nv, m700.ni / 3, scope.ni / 3, us);
    free(out);
}

/* ── Scope sensitivity (vr_sens_step), the eye at the lens, the gun drawn along the aim ── */

#define LEASH (float)(4.0 * DEG)

/* the angle between two directions (any lengths), radians */
static double angle3(const float a[3], const float b[3])
{
    double c[3] = { (double)a[1] * b[2] - (double)a[2] * b[1], (double)a[2] * b[0] - (double)a[0] * b[2],
                    (double)a[0] * b[1] - (double)a[1] * b[0] };
    return atan2(sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]),
                 (double)a[0] * b[0] + (double)a[1] * b[1] + (double)a[2] * b[2]);
}

/* yaw y (+ right), pitch p (+ up), past +-90 degrees over the pole */
static void dir_yp(double y, double p, float d[3])
{
    d[0] = (float)(sin(y) * cos(p));
    d[1] = (float)sin(p);
    d[2] = (float)(-cos(y) * cos(p));
}

static void sens_gain_one_is_identity(void)
{
    VrSens a = {{0}, {0}, 0}, b = {{0}, {0}, 0};
    float h[3], oa[3], ob[3];
    double worst = 0;
    for (int n = 0; n < 288; n++) {     /* 4 s of a wide, quick wander */
        dir_yp(0.6 * sin(2 * PI * 0.7 * n * DT), 0.4 * sin(2 * PI * 0.45 * n * DT), h);
        vr_sens_step(&a, h, 1, 1.0f, LEASH, DT, oa);    /* engaged at 100% */
        vr_sens_step(&b, h, 0, 0.35f, LEASH, DT, ob);   /* not engaged: the gain does nothing */
        if (angle3(oa, h) > worst) worst = angle3(oa, h);
        if (angle3(ob, h) > worst) worst = angle3(ob, h);
        for (int i = 0; i < 3; i++) CHECK(isfinite(oa[i]) && isfinite(ob[i]));
    }
    printf("sens gain 1 / disengaged: worst %.2e deg off the hand\n", worst / DEG);
    CHECK(worst < 1e-5);
}

/* Gain 0.5: a 1 degree hand move gives 0.5 -- level, and across the zenith
 * (no yaw/pitch anywhere: the same along any great circle) */
static void sens_halves_small_moves(void)
{
    const double base[2][2] = {{0.2, 0}, {0, 89.5 * DEG}};     /* yaw, pitch to start at */
    const double step[2][2] = {{0.1 * DEG, 0}, {0, 0.1 * DEG}}; /* 10 frames of this each */
    for (int c = 0; c < 2; c++) {
        VrSens s = {{0}, {0}, 0};
        float h0[3], h[3], o0[3], o[3];
        dir_yp(base[c][0], base[c][1], h0);
        vr_sens_step(&s, h0, 1, 0.5f, LEASH, DT, o0);
        for (int i = 0; i < 3; i++) NEAR(o0[i], h0[i], 1e-6);
        for (int k = 1; k <= 10; k++) {
            dir_yp(base[c][0] + k * step[c][0], base[c][1] + k * step[c][1], h);
            vr_sens_step(&s, h, 1, 0.5f, LEASH, DT, o);
        }
        NEAR(angle3(h0, h) / DEG, 1.0, 1e-4);
        NEAR(angle3(o0, o) / DEG, 0.5, 1e-3);
        NEAR(angle3(o, h) / DEG, 0.5, 1e-3);
        NEAR(angle3(o0, o) + angle3(o, h), angle3(h0, h), 1e-6);   /* on the hand's path */
        if (c == 1) {   /* from 89.5 up, half a degree on: at the zenith, still on x = 0 */
            NEAR(o[0], 0, 1e-6);
            NEAR(o[1], 1, 1e-6);
        }
    }
}

/* Gain 0.35, a 20 degree sweep: the aim never more than the leash off the
 * hand, and dragged along by it once there */
static void sens_leash_caps_offset(void)
{
    VrSens s = {{0}, {0}, 0};
    float h[3], o[3], o0[3];
    double worst = 0;
    dir_yp(0, 0, h);
    vr_sens_step(&s, h, 1, 0.35f, LEASH, DT, o0);
    for (int n = 1; n <= 36; n++) {    /* 20 degrees in 0.5 s */
        dir_yp(20.0 * DEG * n / 36, 0, h);
        vr_sens_step(&s, h, 1, 0.35f, LEASH, DT, o);
        if (angle3(o, h) > worst) worst = angle3(o, h);
    }
    printf("sens leash: worst offset %.4f deg (leash %.1f), the aim moved %.3f deg\n", worst / DEG, LEASH / DEG,
           angle3(o0, o) / DEG);
    CHECK(worst <= LEASH + 1e-3);
    NEAR(angle3(o, h), LEASH, 1e-4);
    NEAR(angle3(o0, o) / DEG, 16.0, 1e-2);
    dir_yp(20.0 * DEG + PI / 2, 0.3, h);   /* a jump, a quarter turn in a frame */
    vr_sens_step(&s, h, 1, 0.35f, LEASH, DT, o);
    CHECK(angle3(o, h) <= LEASH + 1e-3);
}

/* the offset the leash test leaves, then not engaged for t seconds (the
 * hand still, or sweeping on at 40 deg/s): the offset after it, and after
 * the first frame */
static double sens_after(double t, int sweep, double *first)
{
    VrSens s = {{0}, {0}, 0};
    float h[3], o[3];
    double a = 0;
    for (int n = 0; n <= 36; n++) {
        a = 20.0 * DEG * n / 36;
        dir_yp(a, 0, h);
        vr_sens_step(&s, h, 1, 0.35f, LEASH, DT, o);
    }
    for (int n = 1; n * DT <= t + 1e-6; n++) {
        if (sweep) a += 40.0 * DEG * DT;
        dir_yp(a, 0, h);
        vr_sens_step(&s, h, 0, 0.35f, LEASH, DT, o);
        if (n == 1) *first = angle3(o, h);
    }
    return angle3(o, h);
}

static void sens_disengage_returns_to_hand(void)
{
    double first, still = sens_after(0.3, 0, &first), moving = sens_after(0.3, 1, &first);
    double at25 = sens_after(0.25, 0, &first);
    printf("sens disengaged: offset %.2f deg after one frame, %.5f deg at 0.25 s, %.5f at 0.3 s (%.5f sweeping)\n",
           first / DEG, at25 / DEG, still / DEG, moving / DEG);
    CHECK(still / DEG < 0.05);
    CHECK(moving / DEG < 0.05);
    CHECK(at25 / DEG < 0.05);
    /* no jump: about a time constant of 0.08 s, not a snap */
    CHECK(first > 0.6 * LEASH && first < 0.95 * LEASH);
}

static void sens_ignores_bad_input(void)
{
    VrSens a = {{0}, {0}, 0}, b = {{0}, {0}, 0}, c = {{0}, {0}, 0};
    float h[3], oa[3], ob[3], held[3];
    const float bad_dt[4] = {0.0f, -1.0f, NAN, INFINITY};
    const float bad_h[3][3] = {{NAN, 0, -1}, {0, INFINITY, -1}, {0, 0, 0}};
    vr_sens_step(&c, bad_h[0], 1, 0.5f, LEASH, DT, held);   /* nothing yet: straight ahead */
    NEAR(held[0], 0, 0); NEAR(held[1], 0, 0); NEAR(held[2], -1, 0);
    dir_yp(0.3, 0.1, h);
    vr_sens_step(&c, h, 1, 0.5f, LEASH, 0.0f, held);        /* a first hand, bad dt: the hand */
    for (int i = 0; i < 3; i++) NEAR(held[i], h[i], 1e-6);
    for (int n = 0; n < 20; n++) {
        dir_yp(0.01 * n, 0.005 * n, h);
        vr_sens_step(&a, h, 1, 0.5f, LEASH, DT, oa);
        vr_sens_step(&b, h, 1, 0.5f, LEASH, DT, ob);
    }
    dir_yp(0.5, 0.2, h);
    for (int k = 0; k < 4; k++) {       /* bad dt: the aim held, nothing learnt */
        vr_sens_step(&a, h, 1, 0.5f, LEASH, bad_dt[k], held);
        for (int i = 0; i < 3; i++) NEAR(held[i], oa[i], 0);
    }
    for (int k = 0; k < 3; k++) {       /* no hand: the same */
        vr_sens_step(&a, bad_h[k], 1, 0.5f, LEASH, DT, held);
        for (int i = 0; i < 3; i++) NEAR(held[i], oa[i], 0);
    }
    vr_sens_step(&a, h, 1, 0.5f, LEASH, DT, oa);
    vr_sens_step(&b, h, 1, 0.5f, LEASH, DT, ob);
    for (int i = 0; i < 3; i++) { CHECK(isfinite(oa[i])); NEAR(oa[i], ob[i], 0); }
    /* a gain or leash not a number: still a unit direction near the hand */
    vr_sens_step(&a, h, 1, NAN, NAN, DT, oa);
    for (int i = 0; i < 3; i++) CHECK(isfinite(oa[i]));
    NEAR(oa[0] * oa[0] + oa[1] * oa[1] + oa[2] * oa[2], 1, 1e-5);
    CHECK(angle3(oa, h) <= LEASH + 1e-3);
}

/* (lens - p) normalised: looking straight at the lens from p */
static void look_at(const float lens[3], const float p[3], float f[3])
{
    double n = sqrt((double)(lens[0] - p[0]) * (lens[0] - p[0]) + (double)(lens[1] - p[1]) * (lens[1] - p[1])
                    + (double)(lens[2] - p[2]) * (lens[2] - p[2]));
    for (int i = 0; i < 3; i++) f[i] = (float)((lens[i] - p[i]) / n);
}

/* The lens faces +z (its picture's side) unless said; 25 degrees, 0.75 m */
static void eye_on_lens_cases(void)
{
    const float lens[3] = {0.1f, 1.2f, -0.3f}, nz[3] = {0, 0, 1}, fwd[3] = {0, 0, -1}, fwd3[3] = {0, 0, -3};
    const float px[3] = {1, 0, 0}, mx[3] = {-1, 0, 0}, nz3[3] = {0, 0, 4};
    const float A = (float)(25 * DEG), D = 0.75f;
    float p[3], f[3], m[3];
#define AT(dx, dy, dz) (p[0] = lens[0] + (dx), p[1] = lens[1] + (dy), p[2] = lens[2] + (dz), p)
    CHECK(vr_eye_on_lens(AT(0, 0, 0.2f), fwd, lens, nz, A, D, m) == 1);         /* behind it, looking in */
    NEAR(m[0], 0.2, 1e-6); NEAR(m[1], 0, 1e-4); NEAR(m[2], 0.2, 1e-6);
    CHECK(vr_eye_on_lens(AT(0.032f, 0, 0.15f), fwd, lens, nz, A, D, m) == 1);   /* an eye beside the axis */
    NEAR(m[1], atan2(0.032, 0.15), 1e-4); NEAR(m[2], 0.15, 1e-6);
    CHECK(vr_eye_on_lens(AT(0, 0, 0.74f), fwd3, lens, nz3, A, D, NULL) == 1);   /* any lengths */
    CHECK(vr_eye_on_lens(AT(0, 0, 0.76f), fwd, lens, nz, A, D, m) == 0);        /* too far */
    NEAR(m[0], 0.76, 1e-6);
    /* the user's way: the eye on the target, a glance down at the lens held
     * low at arm's length -- 0.55 m back, 0.3 m up, 29 degrees off its axis */
    look_at(lens, AT(0, 0.3f, 0.55f), f);
    CHECK(vr_eye_on_lens(p, f, lens, nz, A, D, m) == 1);
    NEAR(m[0], sqrt(0.3 * 0.3 + 0.55 * 0.55), 1e-6); NEAR(m[1], 0, 1e-4); NEAR(m[2], 0.55, 1e-6);
    /* in front of it, facing it (from the muzzle's side): not behind */
    look_at(lens, AT(0, 0.05f, -0.3f), f);
    CHECK(vr_eye_on_lens(p, f, lens, nz, A, D, m) == 0);
    NEAR(m[1], 0, 1e-4); NEAR(m[2], -0.3, 1e-6);
    CHECK(vr_eye_on_lens(AT(0, 0, -0.2f), fwd, lens, nz, A, D, m) == 0);       /* past it, the lens behind the head */
    CHECK(vr_eye_on_lens(AT(0.2f, 0, 0), fwd, lens, nz, A, D, m) == 0);        /* on its plane: not behind */
    /* a rifle held across the body (the lens facing +x), glanced down at
     * from above and to its left: facing it, but on its front side; the
     * same eye with the rifle the other way round is behind it */
    look_at(lens, AT(-0.15f, 0.35f, 0.1f), f);
    CHECK(vr_eye_on_lens(p, f, lens, px, A, D, m) == 0);
    NEAR(m[1], 0, 1e-4); NEAR(m[2], -0.15, 1e-6);
    CHECK(vr_eye_on_lens(p, f, lens, mx, A, D, m) == 1);
    NEAR(m[2], 0.15, 1e-6);
    f[0] = 0; f[1] = 0; f[2] = 1;
    CHECK(vr_eye_on_lens(AT(0, 0, 0.2f), f, lens, nz, A, D, m) == 0);          /* turned away */
    NEAR(m[1], PI, 1e-4);
    dir_yp(40 * DEG, 0, f);
    CHECK(vr_eye_on_lens(AT(0, 0, 0.2f), f, lens, nz, A, D, m) == 0);          /* turned 40 degrees aside */
    NEAR(m[1], 40 * DEG, 1e-4);
    dir_yp(0, -20 * DEG, f);
    CHECK(vr_eye_on_lens(AT(0, 0, 0.2f), f, lens, nz, A, D, m) == 1);          /* looking 20 degrees down at it */
    CHECK(vr_eye_on_lens(AT(0.2f, 0, 0.1f), fwd, lens, nz, A, D, m) == 0);     /* beside it, looking ahead */
    /* nothing to go by: no, and no measures */
    const float nan3[3] = {NAN, 0, -1}, zero[3] = {0, 0, 0};
    CHECK(vr_eye_on_lens(AT(0, 0, 0.2f), nan3, lens, nz, A, D, m) == 0);
    CHECK(isnan(m[0]) && isnan(m[1]) && isnan(m[2]));
    CHECK(vr_eye_on_lens(AT(0, 0, 0.2f), zero, lens, nz, A, D, m) == 0);
    CHECK(vr_eye_on_lens(AT(0, 0, 0.2f), fwd, lens, nan3, A, D, m) == 0);
    CHECK(vr_eye_on_lens(AT(0, 0, 0.2f), fwd, lens, zero, A, D, m) == 0);
    CHECK(isnan(m[0]) && isnan(m[1]) && isnan(m[2]));
    CHECK(vr_eye_on_lens(AT(0, NAN, 0.2f), fwd, lens, nz, A, D, m) == 0);
#undef AT
}

/* turned about +y by a, as quest_host.c's yaw_rot (the anchor's yaw) */
static void yaw_turn(double a, const float v[3], float o[3])
{
    const double c = cos(a), s = sin(a), x = v[0], z = v[2];
    o[0] = (float)(c * x + s * z);
    o[1] = v[1];
    o[2] = (float)(-s * x + c * z);
}

/* A 30 degree recentre mid-sweep (the anchor's frame turned): with the
 * steadying's and the sensitivity's state turned with it (vr_aim_turn) the
 * aim carries on as it was -- each frame's step no bigger than the hand's,
 * and about where an untouched filter fed the old frame puts it. Without
 * the turn (the control: the reviewer's case) it whips far off. */
static void aim_turn_on_recentre(void)
{
    const double Y = 30 * DEG, hand_step = 60 * DEG * DT;
    VrSteady sa = {{0}, {0}, {0}, 0}, sb = sa, sc = sa, sd = sa;
    VrSens ea = {{0}, {0}, 0}, eb = ea, ec = ea, ed = ea, xa = ea, xb = ea;
    float h[3], hy[3], t[3], oa[3] = {0, 0, -1}, ob[3], oc[3], od[3], obt[3], prev[3], pa[3], pb[3];
    float bprev[3] = {0, 0, -1};
    double worst = 0, whip = 0, step = 0, exact = 0, frame = 0, bstep = 0;
    for (int n = 0; n < 108; n++) {     /* 60 deg/s, a little up and down; turned at 0.75 s */
        dir_yp(60 * DEG * n * DT, 0.1 * sin(2 * PI * 0.5 * n * DT), h);
        yaw_turn(Y, h, hy);
        if (n == 54) {
            vr_aim_turn(&sa, &ea, (float)Y);
            vr_aim_turn(NULL, &xa, (float)Y);
            yaw_turn(Y, oa, prev);      /* where the aim was, in the new frame */
        } else {
            memcpy(prev, oa, sizeof prev);
        }
        const float *in = n >= 54 ? hy : h;
        vr_steady(&sa, in, DT, 2.0f, t); vr_sens_step(&ea, t, 1, 0.5f, LEASH, DT, oa);
        vr_steady(&sb, h, DT, 2.0f, t);  vr_sens_step(&eb, t, 1, 0.5f, LEASH, DT, ob);
        vr_steady(&sc, in, DT, 2.0f, t); vr_sens_step(&ec, t, 1, 0.5f, LEASH, DT, oc);
        vr_steady(&sd, hy, DT, 2.0f, t); vr_sens_step(&ed, t, 1, 0.5f, LEASH, DT, od);  /* the new frame all along */
        vr_sens_step(&xa, in, 1, 0.5f, LEASH, DT, pa);  /* the sensitivity alone: exactly the same */
        vr_sens_step(&xb, h, 1, 0.5f, LEASH, DT, pb);
        if (n >= 54) {
            yaw_turn(Y, ob, obt);
            if (angle3(oa, obt) > worst) worst = angle3(oa, obt);
            if (angle3(oc, obt) > whip) whip = angle3(oc, obt);
            if (angle3(od, obt) > frame) frame = angle3(od, obt);
            if (angle3(oa, prev) > step) step = angle3(oa, prev);
            if (angle3(ob, bprev) > bstep) bstep = angle3(ob, bprev);
            yaw_turn(Y, pb, t);
            if (angle3(pa, t) > exact) exact = angle3(pa, t);
        }
        memcpy(bprev, ob, sizeof bprev);
    }
    /* The steadying works per component, so the same sweep steadied in two
     * frames differs a little anyway (frame): the turned filter may be that
     * far from the untouched one, never a whip. */
    printf("recentre 30 deg mid-sweep: biggest step %.3f deg (the untouched filter's %.3f, the hand's %.3f);"
           " %.3f deg off the untouched filter (steadied in the new frame all along: %.3f); without the turn"
           " %.2f deg off\n", step / DEG, bstep / DEG, hand_step / DEG, worst / DEG, frame / DEG, whip / DEG);
    CHECK(step <= bstep + 0.2 * DEG);
    CHECK(worst <= frame + 0.1 * DEG);
    CHECK(whip > 5 * DEG);
    CHECK(exact < 1e-5);
    /* a yaw not a number: nothing turned */
    VrSteady s1 = sa, s2 = sa;
    VrSens e1 = ea, e2 = ea;
    vr_aim_turn(&s1, &e1, NAN);
    vr_steady(&s1, hy, DT, 2.0f, oa); vr_sens_step(&e1, oa, 1, 0.5f, LEASH, DT, oa);
    vr_steady(&s2, hy, DT, 2.0f, ob); vr_sens_step(&e2, ob, 1, 0.5f, LEASH, DT, ob);
    for (int i = 0; i < 3; i++) NEAR(oa[i], ob[i], 0);
}

/* A new gun hand, or the controller found again: both start afresh, the
 * next hand the aim at once (however far from the last) */
static void aim_reseed_starts_afresh(void)
{
    VrSteady st = {{0}, {0}, {0}, 0};
    VrSens se = {{0}, {0}, 0};
    float h[3], t[3], o[3];
    for (int n = 0; n <= 36; n++) {    /* an offset at the leash */
        dir_yp(20.0 * DEG * n / 36, 0, h);
        vr_steady(&st, h, DT, 1.0f, t);
        vr_sens_step(&se, t, 1, 0.35f, LEASH, DT, o);
    }
    vr_aim_reseed(&st, &se);
    dir_yp(-1.2, 0.3, h);
    vr_steady(&st, h, DT, 1.0f, t);
    vr_sens_step(&se, t, 1, 0.35f, LEASH, DT, o);
    for (int i = 0; i < 3; i++) { NEAR(t[i], h[i], 1e-6); NEAR(o[i], h[i], 1e-6); }
    vr_aim_reseed(NULL, &se);   /* either may be left out */
    vr_aim_reseed(&st, NULL);
}

/* q (x y z w) applied to v */
static void qrot4(const float q[4], const float v[3], float o[3])
{
    double tx = 2 * ((double)q[1] * v[2] - (double)q[2] * v[1]);
    double ty = 2 * ((double)q[2] * v[0] - (double)q[0] * v[2]);
    double tz = 2 * ((double)q[0] * v[1] - (double)q[1] * v[0]);
    o[0] = (float)(v[0] + q[3] * tx + (q[1] * tz - q[2] * ty));
    o[1] = (float)(v[1] + q[3] * ty + (q[2] * tx - q[0] * tz));
    o[2] = (float)(v[2] + q[3] * tz + (q[0] * ty - q[1] * tx));
}

/* a gun rolled about its barrel, pitched, then turned: its barrel along
 * dir_yp(yaw, pitch) */
static void gun_q(double yaw, double pitch, double roll, float q[4])
{
    const double cy = cos(yaw / 2), sy = -sin(yaw / 2), cp = cos(pitch / 2), sp = sin(pitch / 2);
    const double cr = cos(roll / 2), sr = sin(roll / 2);
    /* q = Ry(-yaw) * Rx(pitch) * Rz(roll) (+ yaw to the right, as dir_yp) */
    double x = cy * sp, y = sy * cp, z = -sy * sp, w = cy * cp;    /* Ry * Rx */
    q[0] = (float)(x * cr + y * sr);
    q[1] = (float)(y * cr - x * sr);
    q[2] = (float)(z * cr + w * sr);
    q[3] = (float)(w * cr - z * sr);
}

/* The drawn gun along the final aim: its barrel exactly the aim, its up
 * turned no more than the barrel was (the shortest turn), at the poles too */
static void gun_along_cases(void)
{
    static const float barrel[3] = {0, 0, -1}, upv[3] = {0, 1, 0};
    const double guns[4][3] = {{0, 0, 0}, {0.4, -0.2, 0.5}, {-1.0, 89.9 * DEG, 0.3}, {2.5, -89.95 * DEG, -1.2}};
    const double aims[4][2] = {{0.03, 0.01}, {0.45, -0.24}, {-1.0 + PI, 89.9 * DEG}, {2.5, -89.0 * DEG}};
    for (int k = 0; k < 4; k++) {
        float q[4], o[4], b0[3], b1[3], u0[3], u1[3], aim[3];
        gun_q(guns[k][0], guns[k][1], guns[k][2], q);
        dir_yp(aims[k][0], aims[k][1], aim);
        vr_gun_along(q, aim, o);
        NEAR(o[0] * o[0] + o[1] * o[1] + o[2] * o[2] + o[3] * o[3], 1, 1e-6);
        qrot4(q, barrel, b0); qrot4(o, barrel, b1);
        qrot4(q, upv, u0); qrot4(o, upv, u1);
        CHECK(angle3(b1, aim) < 1e-5);
        CHECK(angle3(u0, u1) <= angle3(b0, aim) + 1e-5);
        NEAR(b1[0] * u1[0] + b1[1] * u1[1] + b1[2] * u1[2], 0, 1e-5);
    }
    {   /* the aim opposite the barrel: a half turn about the gun's own up */
        float q[4], o[4], b[3], u0[3], u1[3], aim[3];
        gun_q(0.3, 0.2, 0.4, q);
        qrot4(q, barrel, b);
        for (int i = 0; i < 3; i++) aim[i] = -b[i];
        vr_gun_along(q, aim, o);
        qrot4(o, barrel, b); qrot4(q, upv, u0); qrot4(o, upv, u1);
        CHECK(angle3(b, aim) < 1e-5);
        CHECK(angle3(u0, u1) < 1e-5);
        for (int i = 0; i < 4; i++) CHECK(isfinite(o[i]));
    }
    {   /* nothing to go by: q as it is (normalised) */
        float q[4], q2[4], o[4];
        const float nan3[3] = {NAN, 0, -1}, zero[3] = {0, 0, 0}, ahead[3] = {0, 0, -1};
        gun_q(0.3, 0.2, 0.4, q);
        vr_gun_along(q, nan3, o);
        for (int i = 0; i < 4; i++) NEAR(o[i], q[i], 1e-6);
        vr_gun_along(q, zero, o);
        for (int i = 0; i < 4; i++) NEAR(o[i], q[i], 1e-6);
        for (int i = 0; i < 4; i++) q2[i] = 2 * q[i];
        vr_gun_along(q2, zero, o);
        for (int i = 0; i < 4; i++) NEAR(o[i], q[i], 1e-6);
        q2[3] = NAN;                    /* not a rotation: as it is */
        vr_gun_along(q2, ahead, o);
        for (int i = 0; i < 3; i++) NEAR(o[i], q2[i], 0);
        CHECK(isnan(o[3]));
    }
}

int main(void)
{
    sens_gain_one_is_identity();
    sens_halves_small_moves();
    sens_leash_caps_offset();
    sens_disengage_returns_to_hand();
    sens_ignores_bad_input();
    eye_on_lens_cases();
    aim_turn_on_recentre();
    aim_reseed_starts_afresh();
    gun_along_cases();
    rifle_lens_on_scope_line();
    rifle_lens_unobstructed();
    rifle_scope_on_top();
    rifle_fits_hand();
    m700_unpacks();
    m700_rejects_bad_data();
    m700_muzzle_and_grip();
    m700_texture_decodes();
    scope_round_the_lens();
    rifle_model_lens_unobstructed();
    scope_clear_of_m700();
    scope_axis_height();
    rifle_shade_matches_overlay_lighting();
    rifle_shade_timing();
    aim_angles_basic();
    aim_angles_poles_and_behind();
    steady_passes_first_and_still();
    steady_smooths_tremor_more_at_zoom();
    steady_follows_fast_swing();
    steady_ignores_bad_dt();
    steady_ignores_nonfinite_dir();
    zoom_step_range();
    lens_crop_still();
    lens_crop_half_guard();
    lens_crop_clamps();
    lens_crop_zoom_scales();
    lens_crop_pitched();
    lens_crop_unknown();
    lens_crop_not_followed();
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("all passed\n");
    return 0;
}
