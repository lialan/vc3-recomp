/*
 * quest_overlay.h - what the headset draws around the game's screen: the
 * handgun in the player's hand with its sight, the laser, the muzzle flash
 * and the settings panel (quest_overlay.c). Drawn into a transparent layer
 * over the screen's quad layers, so the screen itself is untouched.
 */
#ifndef VC3_QUEST_OVERLAY_H
#define VC3_QUEST_OVERLAY_H

/* QoFrame.weapon: the profile's CH_VR_HANDGUN / CH_VR_RIFLE values */
enum { QO_WEAPON_HANDGUN, QO_WEAPON_RIFLE };
enum { QO_SIGHT_IRON, QO_SIGHT_RED_DOT, QO_SIGHT_HOLO, QO_SIGHT_COUNT };

/* One headset frame's worth, in metres in the LOCAL space. */
typedef struct {
    int   gun_valid;
    float gun_q[4], gun_p[3];   /* the controller's aim pose: x y z w, position */
    float kick;                 /* 0..1: the recoil, 1 the moment of the shot */
    float end[3];               /* where the aim meets the screen (or 5 m out) */
    int   beam, dot, flash;     /* what to draw */
    int   sight;                /* QO_SIGHT_* */
    int   show_gun;             /* 0: only the laser */
    int   weapon;               /* QO_WEAPON_* (the profile's CH_VR_*) */
    void *lens_tex;             /* the rifle's lens picture (ChVkTexture *; NULL: none) */
    int   lens_fresh;           /* the picture is the latest game frame's; else dark glass */
} QoFrame;

int  qo_init(void);
/* Gun space (x right, y up, -z the barrel; origin the aim pose): the line the
 * sight looks along lies this far above the bore, and the muzzle is here. */
float qo_sight_height(int sight);
void  qo_muzzle(float out[3]);

/* Into the bound framebuffer (cleared to transparent by the caller, with a
 * depth buffer), seen through viewproj from eye (LOCAL metres). The colours
 * come out premultiplied by their alpha. */
void qo_draw(const QoFrame *f, const float viewproj[16], const float eye[3]);

/* The settings panel: its picture, then drawn on the quad -1..1 through mvp. */
void qo_panel_upload(const unsigned char *rgba, int w, int h);
void qo_draw_panel(const float mvp[16]);

#endif
