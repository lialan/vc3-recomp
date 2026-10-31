/*
 * vr_view.h - the immersive view, between the GL back end and the host that
 * owns the headset (src/android/quest_host.c; on a desktop,
 * src/vr_view_desktop.c, a test stand-in).
 *
 * Immersive: the game's 3D fills each eye's whole field of view. The back
 * end draws every 3D vertex -- its game eye position recovered from its
 * screen position and w through the game's own projection (vr_math.h) --
 * through each eye's matrix: the head's pose relative to the game's camera
 * and the eye's own projection. The game's 2D that is not full-screen (the
 * HUD, the lock-on marks, the comm window) goes to a separate HUD picture
 * that the host shows on a panel the size of the game's own view, so marks
 * stay over what they mark. Frames with no 3D (movies, menus) are shown on
 * the virtual screen as before.
 */
#ifndef VC3_VR_VIEW_H
#define VC3_VR_VIEW_H

#include <stdint.h>

typedef struct {
    int on;                   /* immersive wanted, and a head pose to draw with */
    float vp[2][16];          /* per eye: GL view space of the game camera (game
                                 units) to clip space; column-major */
    float pose_q[2][4];       /* each eye in the anchor's frame: orientation, */
    float pose_p[2][3];       /* position (metres) -- for the headset's layer */
    float tan_fov[2][4];      /* left, right, up, down */
    float fov[2][4];          /* the same as angles (radians), as OpenXR gives them */
    int scale;                /* the game's surfaces drawn at this scale while
                                 immersive (each eye spans the whole field of
                                 view); 0: eye_w x eye_h when given (native),
                                 else the display setting's */
    int eye_w, eye_h;         /* native: each eye's picture this size, in
                                 display pixels -- the game's screen-shaped
                                 surfaces drawn exactly this big per eye,
                                 the rest at the same (non-integer) scale */
    uint64_t id;              /* which head pose (for the log) */
} Vc3VrView;

/* Host: the newest head pose as a view to draw a frame with. 0 when the
 * immersive view is off (or there is no headset pose yet). Called by the
 * back end once a frame, at the flip, for the next frame. */
int vc3_vr_view_latch(Vc3VrView *v);

/* Host: the virtual screen's size on the display (pixels of one eye's
 * picture), for drawing the game at that size when it is shown on the
 * screen rather than immersive; 0 when there is none (no headset, or a
 * fixed scale chosen). */
int vc3_vr_screen_px(int *w, int *h);

/* What the back end hands the host with each frame (vc3_quest_frame). */
typedef struct {
    int imm;                  /* drawn immersive: each eye through view, HUD apart */
    Vc3VrView view;           /* the pose it was drawn with */
    unsigned hud_fbo;         /* the HUD picture (0: none this frame) */
    int hud_w, hud_h;
    float gp[5];              /* the game's projection: kx, ky, cx, cy, 1/pz */
    int gp_ok;
    int gw, gh;               /* the game's screen, guest pixels */
} Vc3VrFrame;

#endif
