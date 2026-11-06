/* vr_rifle.c - the VR rifle's maths: aim angles, one-euro steadying, zoom,
 * the lens's crop, scope sensitivity, the gun drawn along the aim. */
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
    zoom *= powf(2.0f, VR_ZOOM_RATE * stick_y * dt);
    return zoom < 0.5f ? 0.5f : zoom > 4.0f ? 4.0f : zoom;
}

static int lens_crop(const VrLensAim *a, float yaw, float pitch, double scale, float out[3]);
int vr_lens_crop(const VrLensAim *a, float yaw, float pitch, float out[3])
{
    return lens_crop(a, yaw, pitch, 1.0, out);
}

int vr_lens_crop_zoom(const VrLensAim *a, float yaw, float pitch, float zoom_now, float out[3])
{
    return vr_lens_crop_view(a, yaw, pitch, zoom_now, 1.0f, out);
}

int vr_lens_crop_view(const VrLensAim *a, float yaw, float pitch, float zoom_now, float view, float out[3])
{
    double s = isfinite(view) && view > 0 ? view : 1.0;
    if (a && a->follow && isfinite(a->zoom) && a->zoom > 0 && isfinite(zoom_now) && zoom_now > 0)
        s *= (double)a->zoom / zoom_now;
    return lens_crop(a, yaw, pitch, s, out);
}

/* The rifle's direction now, seen from the picture's camera: its tangent
 * plane coordinates (right, up) through the camera's axes -- forward f, right
 * r = (cos y, 0, sin y), up u = r x f (x right, y up, -z ahead) -- times the
 * focal length, about the picture's centre. A direction 90 degrees or more
 * off the camera's axis lands far out that way (and is clamped). */
static int lens_crop(const VrLensAim *a, float yaw, float pitch, double scale, float out[3])
{
    out[0] = out[1] = out[2] = 0.5f;
    if (!a || !isfinite(a->yaw) || !isfinite(a->pitch) || !isfinite(a->focal) || !isfinite(a->guard)
        || !(a->focal > 0) || !(a->guard >= 1))
        return 0;
    double h = 0.5 / a->guard * scale;     /* scale: the zoom drawn at over the zoom now */
    if (h > 0.5)
        h = 0.5;                            /* zoomed out past the band: the whole picture, till the next */
    out[2] = (float)h;
    if (!a->follow || !isfinite(yaw) || !isfinite(pitch))
        return 0;
    const double sy = sin(a->yaw), cy = cos(a->yaw), sp = sin(a->pitch), cp = cos(a->pitch);
    const double d[3] = { sin(yaw) * cos(pitch), sin(pitch), -cos(yaw) * cos(pitch) };
    const double f[3] = { sy * cp, sp, -cy * cp }, r[3] = { cy, 0, sy }, u[3] = { -sy * sp, cp, cy * sp };
    double z = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
    double x = d[0] * r[0] + d[1] * r[1] + d[2] * r[2], y = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
    if (z < 1e-3) {     /* at or past a right angle: far out in that direction */
        x *= 1e3;
        y *= 1e3;
        z = 1;
    }
    const double cu = 0.5 + a->focal * x / z, cv = 0.5 - a->focal * y / z;
    out[0] = (float)(cu < h ? h : cu > 1 - h ? 1 - h : cu);
    out[1] = (float)(cv < h ? h : cv > 1 - h ? 1 - h : cv);
    return cu < h - 1e-6 || cu > 1 - h + 1e-6 || cv < h - 1e-6 || cv > 1 - h + 1e-6;
}

/* ── Scope sensitivity ────────────────────────────────────────────────── */

#define SENS_TAU 0.08                           /* s: the offset's decay, disengaged */
#define SENS_RATE (2.5 * 0.0174532925199433)    /* rad/s on top of it: 4 degrees gone in 0.24 s */

/* v as a unit vector (double); 0 if it is not finite or about zero */
static int unit3(const float v[3], double o[3])
{
    double n;
    if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2]))
        return 0;
    n = sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]);
    if (!(n > 1e-6))
        return 0;
    for (int i = 0; i < 3; i++) o[i] = v[i] / n;
    return 1;
}

/* The shortest turn taking unit a to unit b: its unit axis (ax) and angle.
 * The same: 0; opposite: pi -- about an axis perpendicular to a. */
static double turn_axis(const double a[3], const double b[3], double ax[3])
{
    double c[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    double s = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]), d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    if (s > 1e-12) {
        for (int i = 0; i < 3; i++) ax[i] = c[i] / s;
        return atan2(s, d);
    }
    /* parallel: a crossed with the axis it leans on least */
    const int k = fabs(a[0]) <= fabs(a[1]) && fabs(a[0]) <= fabs(a[2]) ? 0 : fabs(a[1]) <= fabs(a[2]) ? 1 : 2;
    const double e[3] = { k == 0, k == 1, k == 2 };
    c[0] = a[1] * e[2] - a[2] * e[1];
    c[1] = a[2] * e[0] - a[0] * e[2];
    c[2] = a[0] * e[1] - a[1] * e[0];
    s = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    for (int i = 0; i < 3; i++) ax[i] = c[i] / s;
    return d > 0 ? 0 : 3.14159265358979323846;
}

/* v turned by ang about the unit axis ax (Rodrigues); o may be v */
static void turn(const double v[3], const double ax[3], double ang, double o[3])
{
    const double c = cos(ang), s = sin(ang), k = (ax[0] * v[0] + ax[1] * v[1] + ax[2] * v[2]) * (1 - c);
    const double x[3] = { ax[1] * v[2] - ax[2] * v[1], ax[2] * v[0] - ax[0] * v[2], ax[0] * v[1] - ax[1] * v[0] };
    double r[3];
    for (int i = 0; i < 3; i++) r[i] = v[i] * c + x[i] * s + ax[i] * k;
    for (int i = 0; i < 3; i++) o[i] = r[i];
}

/* The aim turns by the hand's turn since the last frame (gain of it while
 * engaged, all of it otherwise); then its offset from the hand -- an axis
 * and an angle -- decays (disengaged) and is held to the leash, the aim
 * rebuilt from the hand at that angle. */
void vr_sens_step(VrSens *s, const float hand[3], int engaged, float gain, float leash, float dt, float out[3])
{
    double h[3], a[3], l[3], ax[3], ang, off;
    if (!unit3(hand, h)) {      /* no hand: the held aim, else straight ahead */
        for (int i = 0; i < 3; i++) out[i] = s->n ? s->aim[i] : (i == 2 ? -1.0f : 0.0f);
        return;
    }
    if (!s->n) {                /* the first: the hand */
        for (int i = 0; i < 3; i++) s->aim[i] = s->last[i] = out[i] = (float)h[i];
        s->n = 1;
        return;
    }
    if (!(dt > 0) || !isfinite(dt)) {   /* hold, nothing learnt */
        for (int i = 0; i < 3; i++) out[i] = s->aim[i];
        return;
    }
    if (!(gain >= 0 && gain <= 1))
        gain = 1;
    if (!(leash >= 0))
        leash = 0;
    if (!unit3(s->aim, a) || !unit3(s->last, l))       /* (they were unit when stored) */
        for (int i = 0; i < 3; i++) a[i] = l[i] = h[i];
    ang = turn_axis(l, h, ax);
    turn(a, ax, engaged ? gain * ang : ang, a);
    off = turn_axis(h, a, ax);
    if (!engaged) {
        off = (off + SENS_RATE * SENS_TAU) * exp(-dt / SENS_TAU) - SENS_RATE * SENS_TAU;
        if (off < 0)
            off = 0;
    }
    if (off > leash)
        off = leash;
    turn(h, ax, off, a);
    for (int i = 0; i < 3; i++) {
        s->aim[i] = out[i] = (float)a[i];
        s->last[i] = (float)h[i];
    }
}

/* v turned about +y by the angle of cosine c, sine s (yaw_rot's sense) */
static void yaw_turn3(float v[3], double c, double s)
{
    const double x = v[0], z = v[2];
    v[0] = (float)(c * x + s * z);
    v[2] = (float)(-s * x + c * z);
}

/* Every vector of the state -- the steadying's filtered direction, its
 * rate and its last input; the sensitivity's aim and last hand -- turned
 * alike. The sensitivity's maths is all axis and angle, so it carries on
 * exactly as if it had run in the new frame; the steadying works per
 * component, so only nearly -- without a jump either way. */
void vr_aim_turn(VrSteady *st, VrSens *se, float yaw)
{
    double c, s;
    if (!isfinite(yaw))
        return;
    c = cos(yaw);
    s = sin(yaw);
    if (st) {
        yaw_turn3(st->x, c, s);
        yaw_turn3(st->dx, c, s);
        yaw_turn3(st->prev, c, s);
    }
    if (se) {
        yaw_turn3(se->aim, c, s);
        yaw_turn3(se->last, c, s);
    }
}

void vr_aim_reseed(VrSteady *st, VrSens *se)
{
    if (st)
        st->init = 0;
    if (se)
        se->n = 0;
}

int vr_eye_on_lens(const float eye_p[3], const float head_fwd[3], const float lens_c[3], const float lens_n[3],
                   float max_angle, float max_dist, float m[3])
{
    double v[3], f[3], n[3], dist, c, depth;
    float sink[3];
    if (!m)
        m = sink;
    m[0] = m[1] = m[2] = NAN;
    for (int i = 0; i < 3; i++) {
        if (!isfinite(eye_p[i]) || !isfinite(lens_c[i]))
            return 0;
        v[i] = (double)lens_c[i] - eye_p[i];    /* from the eye to the lens */
    }
    if (!unit3(head_fwd, f) || !unit3(lens_n, n))
        return 0;
    dist = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    c = dist > 0 ? (v[0] * f[0] + v[1] * f[1] + v[2] * f[2]) / dist : 1;
    c = c > 1 ? 1 : c < -1 ? -1 : c;
    depth = -(v[0] * n[0] + v[1] * n[1] + v[2] * n[2]);
    m[0] = (float)dist;
    m[1] = (float)acos(c);
    m[2] = (float)depth;
    return depth > 0 && dist <= max_dist && c >= cos((double)max_angle);
}

/* r * q: q's turn, then r's (quaternions x y z w) */
static void qmul(const double r[4], const double q[4], double o[4])
{
    o[0] = r[3] * q[0] + r[0] * q[3] + r[1] * q[2] - r[2] * q[1];
    o[1] = r[3] * q[1] - r[0] * q[2] + r[1] * q[3] + r[2] * q[0];
    o[2] = r[3] * q[2] + r[0] * q[1] - r[1] * q[0] + r[2] * q[3];
    o[3] = r[3] * q[3] - r[0] * q[0] - r[1] * q[1] - r[2] * q[2];
}

/* The turn from the barrel b to d as a quaternion: (b x d, 1 + b.d)
 * normalised -- half their angle, about their common perpendicular. */
void vr_gun_along(const float q[4], const float dir[3], float out[4])
{
    double g[4], d[3], b[3], r[4], o[4], n = 0;
    for (int i = 0; i < 4; i++) {
        out[i] = q[i];
        if (!isfinite(q[i]))
            return;
        n += (double)q[i] * q[i];
    }
    n = sqrt(n);
    if (!(n > 1e-6))
        return;
    for (int i = 0; i < 4; i++) out[i] = (float)(g[i] = q[i] / n);
    if (!unit3(dir, d))
        return;
    /* the barrel: -z turned by g */
    b[0] = -2 * (g[0] * g[2] + g[1] * g[3]);
    b[1] = -2 * (g[1] * g[2] - g[0] * g[3]);
    b[2] = -(1 - 2 * (g[0] * g[0] + g[1] * g[1]));
    r[0] = b[1] * d[2] - b[2] * d[1];
    r[1] = b[2] * d[0] - b[0] * d[2];
    r[2] = b[0] * d[1] - b[1] * d[0];
    r[3] = 1 + b[0] * d[0] + b[1] * d[1] + b[2] * d[2];
    if (r[3] < 1e-9) {          /* opposite: a half turn about the gun's up, +y turned by g */
        r[0] = 2 * (g[0] * g[1] - g[2] * g[3]);
        r[1] = 1 - 2 * (g[0] * g[0] + g[2] * g[2]);
        r[2] = 2 * (g[1] * g[2] + g[0] * g[3]);
        r[3] = 0;
    }
    n = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
    for (int i = 0; i < 4; i++) r[i] /= n;
    qmul(r, g, o);
    n = sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2] + o[3] * o[3]);
    for (int i = 0; i < 4; i++) out[i] = (float)(o[i] / n);
}
