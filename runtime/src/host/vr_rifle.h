/* vr_rifle.h - the VR rifle's state, handed from the XR host to a game (ChihiroProfile.vr.rifle). */
#ifndef CH_VR_RIFLE_H
#define CH_VR_RIFLE_H

typedef struct {
    int on;
    float yaw, pitch;   /* radians in the game camera's view space: yaw + right, pitch + up */
    float zoom;         /* 0.5..4 */
} ChVrRifle;

/* dir (GL view space of the game camera, any length) -> yaw/pitch; zero -> 0, 0. */
void vr_aim_angles(const float dir[3], float *yaw, float *pitch);

/* One-euro steadying of an aim direction; more smoothing at higher zoom. */
typedef struct { float x[3], dx[3]; int init; } VrSteady;
void vr_steady(VrSteady *s, const float dir[3], float dt, float zoom, float out[3]);

/* Exponential zoom from the thumbstick (dead zone 0.2), clamped to 0.5..4. */
float vr_zoom_step(float zoom, float stick_y, float dt);

#endif
