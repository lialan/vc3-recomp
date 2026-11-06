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
 */
#if !defined(__ANDROID__)

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
