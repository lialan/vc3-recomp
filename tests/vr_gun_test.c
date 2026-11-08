/* vr_gun_test.c - checks of runtime/src/host/vr_gun.h, the gun's aim from
 * where its laser meets the world (scripts/vr-rifle-test.sh): the game's
 * shot -- from its camera through the screen point it is handed -- goes
 * through the laser's end, however far the muzzle is from the camera; the
 * laser's direction when nothing is met; off the screen beyond the wide
 * limit and behind the camera; and with House of the Dead III's own shot
 * lens (vr_aim.c scales the point from the world's focal to it). */
#include <math.h>
#include <stdio.h>

#include "vr_gun.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b, e) do { double _a = (a), _b = (b); if (!(fabs(_a - _b) <= (e))) { \
    printf("FAIL %s:%d: %s = %g, want %g (+-%g)\n", __FILE__, __LINE__, #a, _a, _b, (double)(e)); fails++; } } while (0)

/* HOTD3's world in chapter 1's lab: 625.2 pixels focal at 640x480 */
static const VrGameProj hotd3 = { 625.2f, -625.2f, 320.0f, 240.0f, 1.0f };
#define WIDE 1.5

static void unit(float v[3])
{
    const float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= n; v[1] /= n; v[2] /= n;
}

/* The game's shot through screen pixels x, y from the centre (y up), at a
 * lens of focal f: its direction from the camera, unit */
static void shot_dir(double x, double y, double f, double d[3])
{
    const double n = sqrt(x * x + y * y + f * f);
    d[0] = x / n; d[1] = y / n; d[2] = -f / n;
}

/* How far the ray from the camera along d passes from p (game units) */
static double miss(const double d[3], const float p[3])
{
    const double t = d[0] * p[0] + d[1] * p[1] + d[2] * p[2];
    const double e[3] = { p[0] - t * d[0], p[1] - t * d[1], p[2] - t * d[2] };
    return sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
}

/* The muzzle well off the camera (HOTD3: 7.5 units a metre; held at the
 * hip, 35 cm right, 40 cm down, 30 cm ahead), its laser meeting a zombie 2
 * m out: the shot through the handed point passes through the laser's end
 * (to the pixel's rounding) -- the screen-plane aim missed it by far */
static void through_the_hit(void)
{
    const float o[3] = { 2.6f, -3.0f, -2.2f };
    float d[3] = { -0.2f, 0.25f, -1.0f };
    VrGunAim a;
    double s[3];
    unit(d);
    const float t = (15.0f + o[2]) / -d[2];           /* the zombie 15 units (2 m) ahead of the camera */
    CHECK(vr_gun_aim(&hotd3, 640, 480, o, d, t, WIDE, &a));
    NEAR(a.p[2], -15.0, 1e-4);
    shot_dir(a.x, a.y, 625.2, s);
    CHECK(miss(s, a.p) < 15.0 * 0.6 / 625.2 * 1.5);    /* the rounding to whole pixels: under a pixel there */
    /* through the screen-plane point, as before: the camera's ray through
     * where the laser crosses the HUD panel (5 m, 37.5 units, out) */
    {
        const float tp = (37.5f + o[2]) / -d[2];
        const float q[3] = { o[0] + d[0] * tp, o[1] + d[1] * tp, o[2] + d[2] * tp };
        VrGunAim b;
        CHECK(vr_gun_aim(&hotd3, 640, 480, o, d, tp, WIDE, &b));
        CHECK(fabs(q[0] - b.p[0]) < 1e-4);
        shot_dir(b.x, b.y, 625.2, s);
        CHECK(miss(s, a.p) > 1.0);                     /* over 13 cm off the zombie */
    }
}

/* A 1280x960 surface (16:9 or 2x drawn): the same aim in the game's 640 */
static void surface_size(void)
{
    const VrGameProj big = { 1250.4f, -1250.4f, 640.0f, 480.0f, 1.0f };
    const float o[3] = { 1.0f, -1.0f, -1.0f }, d[3] = { 0.1f, 0.05f, -0.99373f };
    VrGunAim a, b;
    CHECK(vr_gun_aim(&hotd3, 640, 480, o, d, 20.0f, WIDE, &a));
    CHECK(vr_gun_aim(&big, 1280, 960, o, d, 20.0f, WIDE, &b));
    CHECK(a.x == b.x && a.y == b.y);
    NEAR(a.ux, b.ux, 1e-6);
}

/* Nothing met: the shot parallel to the laser */
static void nothing_met(void)
{
    const float o[3] = { 2.6f, -3.0f, -2.2f };
    float d[3] = { 0.3f, 0.1f, -1.0f };
    VrGunAim a;
    double s[3];
    unit(d);
    CHECK(vr_gun_aim(&hotd3, 640, 480, o, d, 0.0f, WIDE, &a));
    shot_dir(a.x, a.y, 625.2, s);
    NEAR(s[0] * d[0] + s[1] * d[1] + s[2] * d[2], 1.0, 2e-6);
    CHECK(vr_gun_aim(&hotd3, 640, 480, o, d, NAN, WIDE, &a));
}

/* Beyond the game's screen (its edge at 320 px): still on, the aim beyond
 * it, the reading at the edge; past WIDE screens, or behind the camera, off */
static void off_the_screen(void)
{
    const float o[3] = { 0, 0, 0 };
    VrGunAim a;
    {   /* 45 degrees right: 625 px from the centre */
        const float d[3] = { 0.70711f, 0, -0.70711f };
        CHECK(vr_gun_aim(&hotd3, 640, 480, o, d, 10.0f, WIDE, &a));
        CHECK(a.x > 600 && a.x < 650 && a.y == 0);
        NEAR(a.ux, 1.0, 0);
        NEAR(a.uy, 0.5, 1e-6);
    }
    {   /* 70 degrees right: 1718 px out, past 1.5 screens (960) */
        const float d[3] = { 0.93969f, 0, -0.34202f };
        CHECK(!vr_gun_aim(&hotd3, 640, 480, o, d, 10.0f, WIDE, &a));
        CHECK(a.ux < 0 && a.uy < 0);
    }
    {   /* 60 degrees down: 1083 px below, past 1.5 screens (720) */
        const float d[3] = { 0, -0.86603f, -0.5f };
        CHECK(!vr_gun_aim(&hotd3, 640, 480, o, d, 0.0f, WIDE, &a));
    }
    {   /* a hit behind the camera (the muzzle ahead of it, aimed back) */
        const float m[3] = { 0, 0, -3.0f }, d[3] = { 0, 0, 1 };
        CHECK(!vr_gun_aim(&hotd3, 640, 480, m, d, 5.0f, WIDE, &a));
        CHECK(!vr_gun_aim(&hotd3, 640, 480, m, d, 0.0f, WIDE, &a));
    }
    {   /* no projection */
        const VrGameProj none = { 0, 0, 0, 0, 0 };
        const float d[3] = { 0, 0, -1 };
        CHECK(!vr_gun_aim(&none, 640, 480, o, d, 5.0f, WIDE, &a));
        CHECK(!vr_gun_aim(&hotd3, 0, 480, o, d, 5.0f, WIDE, &a));
    }
}

/* House of the Dead III shoots with a 45-degree lens (579.41 px), not its
 * world's: vr_aim.c hands it the point times 579.41 / 625.2 -- the shot's
 * ray still goes through the laser's end */
static void hotd3_shot_lens(void)
{
    const float o[3] = { -2.0f, -2.5f, -3.0f };
    float d[3] = { 0.9f, 0.7f, -1.0f };
    VrGunAim a;
    double s[3];
    unit(d);
    CHECK(vr_gun_aim(&hotd3, 640, 480, o, d, 25.0f, WIDE, &a));
    CHECK(a.x > 320 && a.y > 240);                     /* beyond the top-right corner */
    {
        const double k = 579.41 / 625.2;
        shot_dir(lround(a.x * k), lround(a.y * k), 579.41, s);
        CHECK(miss(s, a.p) < -a.p[2] / 579.41 * 1.5);  /* within its pixel rounding */
    }
}

static void buttons(void)
{
    CHECK(vr_gun_buttons(1, 0, 1) == VC3_VR_FIRE);
    CHECK(vr_gun_buttons(1, 0, 0) == VC3_VR_RELOAD);
    CHECK(vr_gun_buttons(0, 1, 1) == VC3_VR_RELOAD);
    CHECK(vr_gun_buttons(1, 1, 1) == (VC3_VR_FIRE | VC3_VR_RELOAD));
    CHECK(vr_gun_buttons(0, 0, 1) == 0);
}

int main(void)
{
    through_the_hit();
    surface_size();
    nothing_met();
    off_the_screen();
    hotd3_shot_lens();
    buttons();
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("all passed\n");
    return 0;
}
