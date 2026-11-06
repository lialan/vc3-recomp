/*
 * Silent Scope (Konami; Xbox retail, the first game of Silent Scope
 * Complete, ss1.xbe): what it tells the shared runtime about itself
 * (chihiro_profile.h). A retail title: no baseboard, its controller through
 * USB, so the Chihiro-only blocks stay 0. Addresses are filled in as the
 * port finds them (games/ss1/README.md).
 */
#include <stddef.h>

#include "chihiro/chihiro_profile.h"
#include "host/scope_game.h"

extern void ss1_diag_start(void);   /* ss1_diag.c: SS1_DIAG=1 */
extern void sub_000FC4D0(void);     /* D3D: insert a fence, kick unless flags & 2; lifted */
extern const ChScopeGame ss1_scope;             /* scope.c */

static const ChihiroProfile k_ss1 = {
    .id = "ss1",
    .platform = CHIHIRO_PLATFORM_XBOX,
    .title = "Silent Scope",
    .xbe = "ss1.xbe",
    .entry = 0x00081362,                /* retail-key decoded */
    .env_prefix = "SS1",
    .save_name = "ss1-recomp",
    /* XDK 5558 D3D. BlockOnTime(time, flags) at 0xFC580 (same device
     * layout as VC3's: newest time at +0x2C, semaphore pointer at +0x30),
     * replaced by the runtime's (recomp_manual.c). Its "newest fence" kick
     * is 0xFC4D0(0): insert the fence and kick, ret 4. */
    .d3d = {
        .block_on_time = 0x000FC580,
        .device_ptr = 0x00105D28,       /* g_pDevice */
        .kickoff_ret = 0x000FC5AF,      /* where 0xFC580's own call returns */
        .kickoff = sub_000FC4D0,
    },
    .vr = {
        .app_name = "Silent Scope VR",
        .panel_title = "Silent Scope - settings",
        .sdcard_dir = "/sdcard/SilentScope",
        .weapon = CH_VR_RIFLE,
        /* Its 3D is all vertex programs (one program, its position matrix
         * in c188..c191); every batch's rows give |x| / |w| = |y| / |w| =
         * 555.0 pixels: square, 46.8 degrees high (240 / tan 23.4), as its
         * camera block's focal length (0x1D1F980) says. The offset is the
         * viewport's (c59). Eye depth = w: one unit is 555 of the game's
         * (an unscaled model's w row is -1/555); the scene lies at w 0.5..10. */
        .lens_k = 555.0f,
        .render_scale = 1.0f,   /* native, always: no dynamic resolution */
        .program_proj = { 555.0f, -555.0f, 320.53125f, 240.53125f, 1.0f },
        .hud_tiles = {          /* cut round its layout (quest_vulkan.inc's spread) */
            {  0,  0,440,110, 0,-1},   /* total time (221-409 x 35-55) */
            {440,  0,640,110, 1,-1},   /* score, player life (459-617 x 35-99) */
            {  0,110,640,412, 0, 0},   /* the middle: pause menu, Press START, lines (to y 408) */
            {  0,412,490,480, 0, 0},
            {490,412,640,480, 1, 1},   /* credits (508-607 x 418-435) */
        },
        .hud_tile_count = 5,
        .rifle = ch_scope_vr_rifle,     /* the shared scope game (host/scope_game.h) */
        .cull = ch_scope_vr_cull,
        .lens_aim = ch_scope_lens_aim,
    },
    .gpu = {
        /* Game units per metre in the immersive view. The scene's clip w
         * is in units where the street crossing the sniper looks down on
         * lies at w 10 and a car (4.5 m) is 45 px long at the 555 px lens:
         * about 55 m for the car, ~130 m for the crossing, so ~0.08 w per
         * metre. An estimate from the picture (cars, lanes), not a game
         * constant; unset it was 1, which put the whole city a few
         * metres away and exaggerated the stereo. */
        .units_per_m = 0.08f,
        /* It reads none of its pictures back from memory: the scope view
         * is sampled on the GPU (and published to the lens from there).
         * Flips that waited for the GPU to finish the frame kept the Quest
         * at 16-18 fps in stage 1 with the GPU half idle. */
        .lazy_flip = 1,
    },
    .scope = &ss1_scope,
    .hooks = {
        .on_boot = ss1_diag_start,
    },
};

const ChihiroProfile *chihiro_game(void)
{
    return &k_ss1;
}
