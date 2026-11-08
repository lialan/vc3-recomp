/*
 * vr_view_desktop.c - the immersive view without a headset, for testing the
 * back end's immersive path on a desktop (vr_view.h).
 *
 * VC3_IMMERSIVE_TEST=yaw[,pitch[,fov[,sway]]] (degrees): a head turned that
 * far (left and up positive), eyes 64 mm apart, each with a symmetric field
 * of view (default 100 degrees), swaying up to sway degrees either way over
 * time (0: still); the window's side-by-side stereo then shows
 * what each eye of a headset would get. Unset: immersive is off.
 *
 * The game's scale is the headset's (quest_host.c imm_units): the profile's
 * game units per metre, or <prefix>_IMM_UNITS. One unit a metre put Silent
 * Scope's eyes 12.5 times too far apart (its 0.08 units a metre): indoors
 * the left eye stood beyond the corridor's wall, in the next room.
 *
 * VC3_GUN_TEST (a profile with vr.aim_world, with the test head): a gun
 * held still, its laser walked through the eyes' depth and the game aimed
 * from where it meets the world, as the headset's (quest_host.c, vr_gun.h)
 * -- "x,y,z,yaw,pitch": the muzzle in the anchor's frame, metres (x right,
 * y up, z back), the barrel turned yaw left and pitch up, degrees; or
 * "@t:x,y,z,yaw,pitch;t:..." each from t seconds on the scripted keys'
 * clock (VC3_AUTOKEYS). Its trigger and grip are scripted keys "trigger"
 * and "grip": the trigger a shot, off the screen a reload; the grip a
 * reload. A line a second, at each change and every frame a button is
 * held says what it aimed at: the hit along the laser, the point, the aim
 * handed to the game.
 */
#if !defined(__ANDROID__)

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host_input.h"
#include "vr_gun.h"
#include "vr_math.h"
#include "vr_view.h"
#include "chihiro/ch_env.h"
#include "chihiro/chihiro_profile.h"

static float units(void)
{
    static float u = -1;
    if (u < 0) {
        const char *e = ch_env("IMM_UNITS");
        float d = chihiro_profile()->gpu.units_per_m;
        u = e && atof(e) > 0 ? (float)atof(e) : d > 0.0f ? d : 1.0f;
    }
    return u;
}

/* The test gun's pose now (VC3_GUN_TEST): its muzzle (metres) and barrel
 * (unit), the anchor's frame; which schedule entry (-1: none set) */
static int gun_test(float o[3], float d[3])
{
    static int parsed = -1;
    const char *e = ch_env("GUN_TEST");
    float p[5] = { 0 }, x[5];
    double at, now;
    int entry = -1, n, i;
    if (parsed < 0)
        parsed = e && *e && chihiro_profile()->vr.aim_world;
    if (!parsed)
        return -1;
    if (e[0] != '@') {
        if (sscanf(e, "%f,%f,%f,%f,%f", &p[0], &p[1], &p[2], &p[3], &p[4]) != 5)
            return -1;
        entry = 0;
    } else {
        const char *s = e + 1;
        now = vc3_input_script_s();
        for (i = 0; sscanf(s, "%lf:%f,%f,%f,%f,%f%n", &at, &x[0], &x[1], &x[2], &x[3], &x[4], &n) == 6; i++) {
            if (now >= at) {
                memcpy(p, x, sizeof p);
                entry = i;
            }
            s += n;
            if (*s != ';')
                break;
            s++;
        }
        if (entry < 0)
            return -1;
    }
    {
        const float yaw = p[3] * 3.14159265f / 180.0f, pitch = p[4] * 3.14159265f / 180.0f;
        o[0] = p[0];
        o[1] = p[1];
        o[2] = p[2];
        d[0] = -sinf(yaw) * cosf(pitch);
        d[1] = sinf(pitch);
        d[2] = -cosf(yaw) * cosf(pitch);
    }
    return entry;
}

/* An immersive frame as the headset gets it: the test gun aims from where
 * its laser met the world in it, as quest_host.c gun_frame does. */
void vc3_vr_desktop_frame(const Vc3VrFrame *f)
{
    static int last_entry = -2, last_buttons = -1, last_on = -1;
    static double logged_s = -10.0;
    const ChihiroProfile *prof = chihiro_profile();
    const VrGameProj g = { f->gp[0], f->gp[1], f->gp[2], f->gp[3], f->gp[4] };
    float o[3], d[3];
    VrGunAim a;
    int entry, on, trigger, grip, buttons;
    double now;
    if (!f->imm || !f->view.ray || (entry = gun_test(o, d)) < 0)
        return;
    /* the frame's ray is the one the pose was latched with (the gun held
     * still), its hit along it */
    on = vr_gun_aim(&g, f->gw, f->gh, f->view.ray_o, f->view.ray_d, f->aim_hit, VR_GUN_WIDE, &a) && f->gp_ok;
    trigger = vc3_input_auto_held("trigger");
    grip = vc3_input_auto_held("grip");
    buttons = vr_gun_buttons(trigger, grip, on);
    if (prof->vr.aim_screen)
        prof->vr.aim_screen(on, a.x, a.y);
    vc3_input_vr(on ? a.ux : -1.0, on ? a.uy : -1.0, buttons);
    now = vc3_input_script_s();
    if (entry != last_entry || buttons != last_buttons || on != last_on || buttons || now - logged_s >= 1.0) {
        const float n = sqrtf(a.p[0] * a.p[0] + a.p[1] * a.p[1] + a.p[2] * a.p[2]);
        fprintf(stderr, "[gun test] %.2fs aim %d: ray from %.3f %.3f %.3f along %.4f %.4f %.4f, %s %.3f -> point"
                " %.3f %.3f %.3f (from the camera %.5f %.5f %.5f) -> %s %d,%d (screen %.3f,%.3f, reading %.3f,%.3f);"
                " trigger %d grip %d -> buttons %X\n", now, entry, f->view.ray_o[0], f->view.ray_o[1],
                f->view.ray_o[2], f->view.ray_d[0], f->view.ray_d[1], f->view.ray_d[2],
                f->aim_hit > 0 ? "met at" : "nothing met", f->aim_hit, a.p[0], a.p[1], a.p[2],
                n > 0 ? a.p[0] / n : 0, n > 0 ? a.p[1] / n : 0, n > 0 ? a.p[2] / n : 0,
                on ? "on the screen" : "off the screen", a.x, a.y, a.gx, a.gy, a.ux, a.uy, trigger, grip, buttons);
        last_entry = entry;
        last_buttons = buttons;
        last_on = on;
        logged_s = now;
    }
}

int vc3_vr_view_latch(Vc3VrView *v)
{
    static int parsed;
    static float yaw0, pitch0, fov = 100.0f, sway;
    const char *e = ch_env("IMMERSIVE_TEST");
    float q[4], qy[4], qp[4], t, right[3], yaw, pitch;
    const float ex[3] = { 1, 0, 0 };
    int i, k;

    if (!e || !*e)
        return v->on = 0;
    if (!parsed) {
        parsed = 1;
        sscanf(e, "%f,%f,%f,%f", &yaw0, &pitch0, &fov, &sway);
        fprintf(stderr, "[IMM] desktop test head: yaw %.0f, pitch %.0f, field of view %.0f,"
                " sway %.0f\n", yaw0, pitch0, fov, sway);
    }
    yaw = yaw0;
    pitch = pitch0;
    if (sway != 0.0f) {     /* the head moving, as a player's does: degrees either way */
        struct timespec ts;
        double s;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        s = (double)ts.tv_sec + ts.tv_nsec * 1e-9;
        yaw += sway * (float)sin(s * 0.9);
        pitch += 0.6f * sway * (float)sin(s * 1.3 + 0.5);
    }
    memset(v, 0, sizeof *v);
    t = fov * 0.5f * 3.14159265f / 180.0f;
    qy[0] = 0; qy[1] = sinf(yaw * 3.14159265f / 360.0f); qy[2] = 0; qy[3] = cosf(yaw * 3.14159265f / 360.0f);
    qp[0] = sinf(pitch * 3.14159265f / 360.0f); qp[1] = 0; qp[2] = 0; qp[3] = cosf(pitch * 3.14159265f / 360.0f);
    vr_qmul(qy, qp, q);
    vr_qrot(q, ex, right);
    for (i = 0; i < 2; i++) {
        float s = i ? 0.032f : -0.032f;
        memcpy(v->pose_q[i], q, sizeof q);
        for (k = 0; k < 3; k++)
            v->pose_p[i][k] = right[k] * s;
        v->tan_fov[i][0] = -tanf(t);
        v->tan_fov[i][1] = tanf(t);
        v->tan_fov[i][2] = tanf(t);
        v->tan_fov[i][3] = -tanf(t);
        v->fov[i][0] = -t;
        v->fov[i][1] = t;
        v->fov[i][2] = t;
        v->fov[i][3] = -t;
        vr_eye_matrix(v->pose_q[i], v->pose_p[i], v->tan_fov[i], units(), 0.05f, 2000.0f, v->vp[i]);
    }
    {   /* VC3_IMMERSIVE_TEST_SCALE=n: the immersive detail the headset would
         * ask; VC3_IMMERSIVE_TEST_SIZE=WxH: native, each eye that size */
        const char *sc = ch_env("IMMERSIVE_TEST_SCALE"), *sz = ch_env("IMMERSIVE_TEST_SIZE");
        v->scale = sc ? atoi(sc) : 0;
        if (sz && sscanf(sz, "%dx%d", &v->eye_w, &v->eye_h) == 2 && v->eye_w > 0 && v->eye_h > 0)
            v->scale = 0;
    }
    {   /* the test gun's laser, as the headset's: GL view space, game units */
        float o[3], d[3];
        if (gun_test(o, d) >= 0) {
            v->ray = 1;
            for (k = 0; k < 3; k++) {
                v->ray_o[k] = o[k] * units();
                v->ray_d[k] = d[k];
            }
        }
    }
    v->id++;
    return v->on = 1;
}

/* VC3_SCREEN_TEST_SIZE=WxH: a virtual screen that size on the display. */
int vc3_vr_screen_px(int *w, int *h)
{
    const char *sz = ch_env("SCREEN_TEST_SIZE");
    return sz && sscanf(sz, "%dx%d", w, h) == 2 && *w > 0 && *h > 0;
}

#endif
