/* vr_rifle.h - the VR rifle's state, handed from the XR host to a game (ChihiroProfile.vr.rifle). */
#ifndef CH_VR_RIFLE_H
#define CH_VR_RIFLE_H

#include "vr_view.h"

typedef struct {
    int on;
    float yaw, pitch;   /* radians in the game camera's view space: yaw + right, pitch + up */
    float zoom;         /* 0.5..4 */
} ChVrRifle;

/* dir (GL view space of the game camera, any length) -> yaw/pitch; zero -> 0, 0. */
void vr_aim_angles(const float dir[3], float *yaw, float *pitch);

/* One-euro steadying of an aim direction; more smoothing at higher zoom. */
typedef struct { float x[3], dx[3], prev[3]; int init; } VrSteady;
void vr_steady(VrSteady *s, const float dir[3], float dt, float zoom, float out[3]);

/* Exponential zoom from the thumbstick (dead zone 0.2), clamped to 0.5..4. */
float vr_zoom_step(float zoom, float stick_y, float dt);

/* The lens's crop of its picture for a display frame (image-space
 * reprojection): the picture was drawn with a (vr_view.h); the rifle points
 * at yaw, pitch now (ChVrRifle's). out: the crop's centre u, v (0..1 of the
 * picture, v down) and half size h = 0.5 / guard -- where the aim is now in
 * the picture, so the lens's centre shows it -- kept inside the picture.
 * Returns 1 when that meant clamping it to the edge. No aim known (guard
 * < 1, focal <= 0, anything not finite): the whole picture, 0; a picture
 * the rifle did not aim (a->follow 0), or yaw or pitch not finite: the
 * centre crop, 0. */
int vr_lens_crop(const VrLensAim *a, float yaw, float pitch, float out[3]);

/* Scope sensitivity: the aim follows the hand's rotation scaled by gain while
 * engaged, never more than leash radians from the hand; disengaged, it turns
 * with the hand and the offset decays to zero (time constant 0.08 s, plus
 * 2.5 deg/s: a 4 degree offset is gone in 0.24 s). Rotations between
 * successive unit vectors (axis and angle), no yaw/pitch. Unit vectors in
 * (any length is normalised); the first hand starts it and is the output.
 * Bad dt or no hand (not finite, zero): the held aim (none yet: straight
 * ahead), the state untouched. A gain not in 0..1: 1; a leash not >= 0: 0. */
typedef struct { float aim[3], last[3]; int n; } VrSens;
void vr_sens_step(VrSens *s, const float hand[3], int engaged, float gain, float leash, float dt, float out[3]);
/* Is the head looking into the lens: within max_dist of its centre and facing it within max_angle (radians). */
int vr_eye_on_lens(const float head_p[3], const float head_fwd[3], const float lens_c[3], float max_angle, float max_dist);
/* The gun's orientation q (x y z w) turned by the shortest rotation taking
 * its barrel (-z) onto dir: the gun drawn along dir, its roll kept. dir
 * opposite the barrel: a half turn about the gun's own up (+y). dir not
 * finite or zero: q normalised; q not a rotation (not finite or zero): q. */
void vr_gun_along(const float q[4], const float dir[3], float out[4]);

#endif
