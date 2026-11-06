/*
 * scope_game.h - a game aimed through a sniper scope (Konami's Silent Scope
 * games): its VR rifle and its 16:9 view, for any such game. The game
 * describes where its state lives (ChScopeGame, the profile's scope) and
 * wraps a handful of its own functions with the ch_scope_* replacements
 * below (games/<id>/src/recomp_manual.c); everything else is here.
 *
 * What it does, with the rifle on (ChVrRifle from the headset, or
 * <PREFIX>_VR_RIFLE_TEST on the desktop):
 *   - the rifle's yaw and pitch replace the game's aim words after its aim
 *     step, unclamped (ch_scope_aim_step);
 *   - the game's scope circle is not drawn; the zoom factor it writes is
 *     written times the rifle's zoom, over the guard band (ch_scope_circle);
 *     the aim marks are not drawn while there is a scope picture
 *     (ch_scope_marks), nor the scope's reticle (ch_scope_reticle);
 *   - every scope pass is followed by a D3D fence and the aim, focal length,
 *     guard and zoom it was drawn with (ch_scope_pass), which the renderer
 *     reads back at the flip that shows that picture (ch_scope_lens_aim);
 *     the pass's first run publishes its target as the lens picture;
 *   - the main view's CPU culling widens to the headset's view
 *     (ch_scope_camera, ch_scope_vr_cull).
 * At 16:9 (the display setting, no headset) the culling widens with the
 * picture and the aim's yaw limit moves to the 16:9 view's edge.
 *
 * Diagnostics, by the game's env prefix: <PREFIX>_VR_RIFLE_TEST=yaw_deg,
 * pitch_deg,zoom[,frames[,start]] (a rifle held still; with frames (0:
 * never), switched off and on every that many game frames; with start, off
 * before that game frame -- menus the aim steers, as ss2's and ss3's, then
 * pick as scripted); <PREFIX>_AIM_LOG=1 (the aim, the
 * shot's direction and the lens's aims); <PREFIX>_VR_LENS_GUARD=g (1..8).
 */
#ifndef CH_SCOPE_GAME_H
#define CH_SCOPE_GAME_H

#include <stdint.h>

#include "host/vr_rifle.h"

/* An enemy for the bot (ChScopeGame.bot_targets): the game's own id and
 * object address, where to aim, and how far round it a shot still lands */
typedef struct ChScopeTarget {
    uint32_t id, object;
    float pos[3], radius;
} ChScopeTarget;

typedef struct ChScopeGame {
    const char *tag;                /* log prefix: "ss1" */
    uint32_t frame;                 /* the main loop's frame counter (u32) */
    /* The aim: int16 words, 65536 a turn, stepped by the game's aim step
     * (ch_scope_aim_step wraps it); the game's sign of a turn to the right
     * and up; the step's yaw clamp, the 4:3 view's edge. With aim_base the
     * words are offsets into the record it points to (the active player's;
     * nothing while it is 0), else addresses. */
    uint32_t aim_base;
    uint32_t aim_pitch, aim_yaw;
    int yaw_right, pitch_up;
    int yaw_edge;
    /* The zoom: the scope focal length's factor (float), written by the
     * circle; the camera mode word the focal length function
     * (ch_scope_focal wraps it) reads, and its value for the scope */
    uint32_t zoom_k, cam_mode, cam_mode_scope;
    /* The scope pass: its camera block's focal length (float), as handed to
     * its camera; a pointer to its target's D3D surface object; the
     * target's width; its projection in target pixels per unit of that
     * focal length */
    uint32_t scope_focal, scope_surf;
    float scope_w, scope_proj;
    /* A pass that only sets its target up, the view drawn after it: a word
     * nonzero while the scope view is drawn. Its projection is then the
     * first focal length the scope's camera mode makes while that is set
     * (scope_focal, already in target pixels: scope_proj 1), and the pass's
     * lens mark is made there. 0: the pass draws the whole view, with
     * scope_focal at its entry times scope_proj. */
    uint32_t scope_drawing;
    /* The camera block the CPU cull tests against (floats): focal length D,
     * width W, half width, secant of the horizontal half angle; the game's
     * own 0.25; the main view's W (another W is the scope's) */
    uint32_t cam_d, cam_w, cam_half_w, cam_sec, quarter;
    float main_w;
    /* <PREFIX>_AIM_LOG's shot direction: the main camera's and the scope's
     * 3x3 int16 matrices (rows its axes; the shot along minus the scope's
     * third row). 0: not logged */
    uint32_t main_cam, scope_cam;
    /* What the circle writes besides drawing, done here while the rifle
     * aims; returns the zoom factor it would write (the game's own: 1, or
     * its scope type's) */
    float (*circle_writes)(void);
    /* What the circle-at-the-aim and aim marks function writes besides
     * drawing; returns 1 where it would have called the circle */
    int (*marks_writes)(void);
    /* Bytes each replaced function's ret pops after its return address
     * (cdecl: 0) */
    unsigned circle_ret, marks_ret, reticle_ret;

    /* Games built otherwise (Silent Scope 3); 0 where a game is as above.
     *
     * zoom_fov: a scope zoomed by its field of view, no zoom factor (zoom_k
     * 0): the int16 full angles (65536 a turn) its drawn projection is
     * remade from when they change -- the words ch_scope_fov's function
     * copies -- scaled by the guard over the rifle's zoom while it runs and
     * put back after, so the game keeps its own.
     * pass_ret: the scope pass is the copy of a view drawn elsewhere into the
     * scope's texture, by a function called for other copies too: only the
     * call returning here is the pass (ch_scope_pass).
     * scope_surf_off: the surface object pointer is at [scope_surf] + this.
     * aim_pre: the scope camera is built from the aim before the aim step
     * runs; ch_scope_aim_pre wraps that build and writes the rifle first.
     * cull_view, cull_tan, cull_sec: a cull reading the main view's tangent
     * and secant of its horizontal half angle (floats at those offsets in
     * the view record cull_view) rather than a camera block: ch_scope_cull
     * wraps it -- cdecl (centre, view, radius) -- widening them for the
     * main view's calls and putting them back after (they make its
     * projection too). */
    uint32_t zoom_fov[4];
    uint32_t pass_ret;
    uint32_t scope_surf_off;
    int aim_pre;
    uint32_t cull_view, cull_tan, cull_sec;
    /* screen_view: the scope view is drawn into the screen itself before
     * the copy (pass_ret) takes it into the scope's texture -- the draws
     * between ch_scope_screen_view's call whose first argument is this
     * (cdecl, the view's index) and the copy are a picture for the copy,
     * not the screen's: immersive, they go to the game's own surface
     * (ch_scope_flat_now, the profile's vr.flat_now). 0: none. */
    uint32_t screen_view;

    /* <PREFIX>_BOT (ch_scope_frame, below): the game's live enemies --
     * bot_targets fills up to max (world points to aim at, game units) and
     * returns how many; the shot's origin (int32 x, y, z at this offset in
     * the aim record); a word nonzero while the game is paused. 0: no bot. */
    int (*bot_targets)(ChScopeTarget *t, int max);
    uint32_t shot_origin, paused;
    float shot_speed;           /* game units a shot flies a frame (0: at once) */
    /* <PREFIX>_BOT_CHEAT=1: a shot landing on t, written as the game's own
     * shot test writes one (for a target behind cover the bot's shots
     * cannot reach); 0: none */
    void (*bot_hit)(const ChScopeTarget *t);
} ChScopeGame;

/* The game's own functions, wrapped (each runs the lifted body, `gen`,
 * itself where it needs to) */
void ch_scope_aim_step(void (*gen)(void));   /* steps and clamps the aim words */
void ch_scope_camera(void (*gen)(void));     /* loads the cull's camera block */
void ch_scope_pass(void (*gen)(void));       /* renders the scope view into its target */
void ch_scope_focal(void (*gen)(void));      /* a camera mode's focal length, from the zoom factor */
void ch_scope_circle(void (*gen)(void));     /* draws the scope circle, writes the zoom factor */
void ch_scope_marks(void (*gen)(void));      /* the circle at the aim and the aim marks */
void ch_scope_reticle(void (*gen)(void));    /* the reticle inside the scope view */
/* The other build (above) */
void ch_scope_fov(void (*gen)(void));        /* remakes the drawn scope projection from zoom_fov */
void ch_scope_aim_pre(void (*gen)(void));    /* builds the scope camera from the aim (aim_pre) */
void ch_scope_cull(void (*gen)(void));       /* the bounding-sphere cull (cull_view) */
void ch_scope_screen_view(void (*gen)(void));    /* draws a view into the screen (screen_view) */
int ch_scope_flat_now(uint32_t release);               /* the profile's vr.flat_now */

/* The profile's vr hooks (chihiro_profile.h vr.rifle, vr.cull, vr.lens_aim) */
void ch_scope_vr_rifle(const ChVrRifle *r);
void ch_scope_vr_cull(float half_angle_rad);
int ch_scope_lens_aim(VrLensAim *out);

/* At boot (chihiro.c, with a profile scope): <PREFIX>_VR_RIFLE_TEST */
void ch_scope_boot(void);

/* <PREFIX>_BOT=1: a player for test runs, here and on a headset alike
 * (env.txt), from the first frame of play (the menus before are the
 * player's or a pad script's). In play (the aim step) it holds the live
 * enemy the game lists nearest its aim, aims at it (led by its motion over
 * the shot's flight) and pulls the trigger (the pad's right trigger) while
 * the aim stays on it; a target down leaves the game's list. Out of play it
 * presses START to resume a pause, and with <PREFIX>_BOT=2 every 2 s
 * besides (past cutscenes, results and the continue screen).
 * <PREFIX>_BOT_CHEAT=1: a target fired at for 5 s and still up (behind
 * cover) is hit outright (ChScopeGame.bot_hit). <PREFIX>_BOT_LOG=1 logs its
 * targets and trigger pulls. ch_scope_frame wraps a function the game's main loop
 * calls once a frame, in every state. */
void ch_scope_frame(void (*gen)(void));

#endif
