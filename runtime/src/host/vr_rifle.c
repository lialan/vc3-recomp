/* vr_rifle.c - the VR rifle's maths: aim angles, one-euro steadying, zoom. */
#include "vr_rifle.h"

#include <math.h>

void vr_aim_angles(const float dir[3], float *yaw, float *pitch)
{
    if (dir[0] == 0 && dir[1] == 0 && dir[2] == 0) { *yaw = 0; *pitch = 0; return; }
    *yaw = atan2f(dir[0], -dir[2]);
    *pitch = atan2f(dir[1], hypotf(dir[0], dir[2]));
}

/* smoothing factor of a low-pass with the given cutoff (Hz) at step dt */
static float lp_alpha(float cutoff, float dt)
{
    float tau = 1.0f / (2.0f * 3.14159265358979f * cutoff);
    return 1.0f / (1.0f + tau / dt);
}

/* Constants: min_cutoff 1.2/sqrt(zoom) Hz, beta 3.8, d_cutoff 1 Hz. beta is 3.8,
 * not the plan's 0.6: with the standard derivative (new raw - previous raw) the
 * speed estimate is smaller than the plan's variant, and 0.6 let a 90 deg/s
 * sweep lag 7.7 deg (bound 3). 3.8 keeps 0.07 deg headroom (3.7 passes by only 0.008). */
void vr_steady(VrSteady *s, const float dir[3], float dt, float zoom, float out[3])
{
    const float beta = 3.8f, d_cutoff = 1.0f;
    int finite = isfinite(dir[0]) && isfinite(dir[1]) && isfinite(dir[2]);
    if (!finite || !(dt > 0) || !isfinite(dt)) {   /* bad input: hold, state untouched */
        float m = s->init ? sqrtf(s->x[0] * s->x[0] + s->x[1] * s->x[1] + s->x[2] * s->x[2]) : 0;
        for (int i = 0; i < 3; i++)
            out[i] = s->init ? (m > 0 ? s->x[i] / m : s->x[i]) : finite ? dir[i] : (i == 2 ? -1.0f : 0.0f);
        return;
    }
    zoom = zoom >= 0.5f ? (zoom > 4.0f ? 4.0f : zoom) : 0.5f;
    float min_cutoff = 1.2f / sqrtf(zoom);
    float u[3] = {dir[0], dir[1], dir[2]};
    float n = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (n > 0) for (int i = 0; i < 3; i++) u[i] /= n;
    if (!s->init) {
        for (int i = 0; i < 3; i++) { s->x[i] = s->prev[i] = u[i]; s->dx[i] = 0; out[i] = dir[i]; }
        s->init = 1;
        return;
    }
    float ad = lp_alpha(d_cutoff, dt);
    for (int i = 0; i < 3; i++) {
        float raw = (u[i] - s->prev[i]) / dt;   /* standard one-euro: new raw - previous raw */
        s->prev[i] = u[i];
        s->dx[i] += ad * (raw - s->dx[i]);
        s->x[i] += lp_alpha(min_cutoff + beta * fabsf(s->dx[i]), dt) * (u[i] - s->x[i]);
    }
    float m = sqrtf(s->x[0] * s->x[0] + s->x[1] * s->x[1] + s->x[2] * s->x[2]);
    for (int i = 0; i < 3; i++) out[i] = m > 0 ? s->x[i] / m : s->x[i];
}

float vr_zoom_step(float zoom, float stick_y, float dt)
{
    if (fabsf(stick_y) < 0.2f) return zoom;
    zoom *= powf(2.0f, stick_y * dt);
    return zoom < 0.5f ? 0.5f : zoom > 4.0f ? 4.0f : zoom;
}
