/*
 * vr_gun.h - a light gun's aim from where its laser meets the world (the
 * profile's vr.aim_world), header-only as vr_shot.h: the Quest host, the
 * desktop stand-in's test gun (vr_view_desktop.c) and tests/vr_gun_test.c.
 *
 * The laser leaves the muzzle along the barrel, and the renderer walks it
 * through an eye's depth (vr_shot.h vr_shot_plan_ray): t game units along
 * it, it meets the world. The game shoots from its camera -- the origin of
 * the GL view space the eyes are drawn in -- through a point of its screen;
 * that point is the hit put through the game's own projection, so the
 * game's shot passes through the very surface point the laser ends at,
 * whatever the parallax between the hand and the camera. Nothing met: the
 * laser's direction, the screen point a ray from the camera parallel to
 * the barrel goes through (a point at infinity along the laser).
 */
#ifndef VC3_VR_GUN_H
#define VC3_VR_GUN_H

#include <math.h>

#include "host_input.h"
#include "vr_math.h"

/* The aim handed to the game: its screen's pixels from the centre, y up
 * (the game's own 640 x 480 whatever its surface's size, as the profile's
 * vr.aim_screen takes it). */
#define VR_GUN_SCREEN_W 640.0
#define VR_GUN_SCREEN_H 480.0
/* How far from the screen's centre the gun still shoots, in screens each
 * way (1.5: 3 screens wide and high -- at VC3's usual lens 51 degrees
 * either side, 43 up and down); beyond, the gun is off the screen (a pull
 * reloads). */
#define VR_GUN_WIDE 1.5

typedef struct {
    float p[3];         /* the point aimed at, GL view space (game units); nothing met: the laser's direction */
    double gx, gy;      /* it on the game's screen, 0..1 across and down (beyond: outside it) */
    double ux, uy;      /* the gun's reading: the screen's point nearest it, 0..1 */
    int x, y;           /* the aim, pixels from the screen's centre, y up */
} VrGunAim;

/* The aim from the laser -- o + d t (GL view space of the game camera, game
 * units; d unit), t where it meets the world (0: nothing met) -- through
 * the game's projection g of a gw x gh screen. 1: the gun is on the screen,
 * the point within wide screens of its centre each way; 0: off it (behind
 * the camera, or farther out) -- a pull there is a reload. */
static inline int vr_gun_aim(const VrGameProj *g, int gw, int gh, const float o[3], const float d[3], float t,
                             double wide, VrGunAim *a)
{
    float sx, sy;
    int i;
    a->gx = a->gy = a->ux = a->uy = -1.0;
    a->x = a->y = 0;
    for (i = 0; i < 3; i++)
        a->p[i] = t > 0.0f && isfinite(t) ? o[i] + d[i] * t : d[i];
    if (gw <= 0 || gh <= 0 || !(fabsf(g->kx) > 1e-3f) || !(fabsf(g->ky) > 1e-3f) || !(fabsf(g->inv_pz) > 1e-9f)
        || !vr_project_gl(g, a->p, &sx, &sy))
        return 0;
    a->gx = sx / gw;
    a->gy = sy / gh;
    if (!(fabs(a->gx - 0.5) <= wide && fabs(a->gy - 0.5) <= wide))
        return 0;
    a->ux = a->gx < 0 ? 0 : a->gx > 1 ? 1 : a->gx;
    a->uy = a->gy < 0 ? 0 : a->gy > 1 ? 1 : a->gy;
    a->x = (int)lround((a->gx - 0.5) * VR_GUN_SCREEN_W);
    a->y = (int)lround((0.5 - a->gy) * VR_GUN_SCREEN_H);
    return 1;
}

/* The gun's buttons as the game gets them (host_input.h VC3_VR_*): the
 * trigger a shot, or with the gun off the screen a reload; the grip a
 * reload. */
static inline int vr_gun_buttons(int trigger, int grip, int on_screen)
{
    return (trigger ? on_screen ? VC3_VR_FIRE : VC3_VR_RELOAD : 0) | (grip ? VC3_VR_RELOAD : 0);
}

#endif
