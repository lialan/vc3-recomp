/*
 * vr_view_desktop.c - the immersive view without a headset, for testing the
 * back end's immersive path on a desktop (vr_view.h).
 *
 * VC3_IMMERSIVE_TEST=yaw[,pitch[,fov]] (degrees): a head turned that far
 * (left and up positive), eyes 64 mm apart, each with a symmetric field of
 * view (default 100 degrees); the window's side-by-side stereo then shows
 * what each eye of a headset would get. Unset: immersive is off.
 */
#if !defined(__ANDROID__)

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vr_math.h"
#include "vr_view.h"
#include "chihiro/ch_env.h"

int vc3_vr_view_latch(Vc3VrView *v)
{
    static int parsed;
    static float yaw, pitch, fov = 100.0f;
    const char *e = ch_env("IMMERSIVE_TEST");
    float q[4], qy[4], qp[4], t, right[3];
    const float ex[3] = { 1, 0, 0 };
    int i, k;

    if (!e || !*e)
        return v->on = 0;
    if (!parsed) {
        parsed = 1;
        sscanf(e, "%f,%f,%f", &yaw, &pitch, &fov);
        fprintf(stderr, "[IMM] desktop test head: yaw %.0f, pitch %.0f, field of view %.0f\n",
                yaw, pitch, fov);
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
        vr_eye_matrix(v->pose_q[i], v->pose_p[i], v->tan_fov[i], 1.0f, 0.05f, 2000.0f, v->vp[i]);
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
