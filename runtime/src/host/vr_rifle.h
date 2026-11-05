/* vr_rifle.h - the VR rifle's state, handed from the XR host to a game (ChihiroProfile.vr.rifle). */
#ifndef CH_VR_RIFLE_H
#define CH_VR_RIFLE_H

typedef struct {
    int on;
    float yaw, pitch;   /* radians in the game camera's view space: yaw + right, pitch + up */
    float zoom;         /* 0.5..4 */
} ChVrRifle;

#endif
