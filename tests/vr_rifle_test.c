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
