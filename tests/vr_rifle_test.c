/* vr_rifle_test.c - checks of runtime/src/host/vr_rifle.c, the VR rifle's
 * maths (scripts/vr-rifle-test.sh): aim angles, tremor steadying, zoom. */
#include <math.h>
#include <stdio.h>

#include "vr_rifle.h"
#include "quest_overlay.h"
#include <string.h>
#include "quest_gun_geometry.inc"
#include "quest_rifle_geometry.inc"

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
    NEAR(zmin, RIFLE_MUZZLE_Z, 1e-6);
    for (int a = 0; a < 3; a++) CHECK(fabsf(grip->c[a]) <= grip->h[a] + 0.03f);
    CHECK(sizeof RIFLE_BODY / sizeof *RIFLE_BODY + RIFLE_SCOPE_N <= 64);
}

int main(void)
{
    rifle_lens_on_scope_line();
    rifle_lens_unobstructed();
    rifle_scope_on_top();
    rifle_fits_hand();
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
