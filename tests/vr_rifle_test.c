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

static void rifle_lens_on_scope_line(void)
{
    const Box *tube = &RIFLE_SCOPE[0], *bell = &RIFLE_SCOPE[2];
    /* on the tube's axis, above the bore by the tube's height */
    NEAR(RIFLE_LENS_C[0], tube->c[0], 1e-6);
    NEAR(RIFLE_LENS_C[1], tube->c[1], 1e-6);
    NEAR(RIFLE_LENS_C[1], RIFLE_SCOPE_Y, 1e-6);
    /* at the tube's rear end: past the tube, on the rear bell's back face */
    CHECK(RIFLE_LENS_C[2] >= tube->c[2] + tube->h[2]);
    NEAR(RIFLE_LENS_C[2], bell->c[2] + bell->h[2], 0.002);
    CHECK(RIFLE_LENS_C[2] >= bell->c[2] + bell->h[2]);
    /* behind the receiver, and the disc inside the bell */
    CHECK(RIFLE_LENS_C[2] >= RIFLE_RECEIVER.c[2] + RIFLE_RECEIVER.h[2]);
    CHECK(RIFLE_LENS_R > 0.02 && RIFLE_LENS_R < 0.03);
    CHECK(RIFLE_LENS_R <= bell->h[0]);
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
    CHECK(sizeof RIFLE_BODY / sizeof *RIFLE_BODY + sizeof RIFLE_SCOPE / sizeof *RIFLE_SCOPE <= 64);
}

int main(void)
{
    rifle_lens_on_scope_line();
    rifle_fits_hand();
    aim_angles_basic();
    aim_angles_poles_and_behind();
    steady_passes_first_and_still();
    steady_smooths_tremor_more_at_zoom();
    steady_follows_fast_swing();
    steady_ignores_bad_dt();
    steady_ignores_nonfinite_dir();
    zoom_step_range();
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("all passed\n");
    return 0;
}
