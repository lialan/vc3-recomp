/*
 * Silent Scope 2 (Konami; Xbox retail, the second game of Silent Scope
 * Complete, ss2.xbe): what it tells the shared runtime about itself
 * (chihiro_profile.h). The same XDK 5558 build as ss1: retail, the
 * controller through USB, the Chihiro-only blocks 0. Its VR is ss1's --
 * the shared scope game (host/scope_game.h; its addresses in scope.c) and
 * ss1's settings.
 */
#include <stddef.h>

#include "chihiro/chihiro_profile.h"
#include "host/scope_game.h"

extern void sub_0012CAA0(void);     /* D3D: insert a fence, kick unless flags & 2; lifted */
extern const ChScopeGame ss2_scope; /* scope.c */

static const ChihiroProfile k_ss2 = {
    .id = "ss2",
    .platform = CHIHIRO_PLATFORM_XBOX,
    .title = "Silent Scope 2",
    .xbe = "ss2.xbe",
    .entry = 0x000B23EC,                /* retail-key decoded */
    .env_prefix = "SS2",
    .save_name = "ss2-recomp",
    .disc_launcher = "ssc",             /* Silent Scope Complete's menu (default.xbe) */
    /* XDK 5558 D3D, as ss1's (found by its byte pattern): BlockOnTime at
     * 0x12CB50, replaced by the runtime's (recomp_manual.c). */
    .d3d = {
        .block_on_time = 0x0012CB50,
        .device_ptr = 0x00136D68,       /* g_pDevice */
        .kickoff_ret = 0x0012CB7F,      /* where 0x12CB50's own call returns */
        .kickoff = sub_0012CAA0,
    },
    .vr = {
        .app_name = "Silent Scope 2 VR",
        .panel_title = "Silent Scope 2 - settings",
        .sdcard_dir = "/sdcard/SilentScope",    /* ss1's: the Silent Scope Complete folder */
        .weapon = CH_VR_RIFLE,
        /* Its 3D is all vertex programs, as ss1's: every batch's position
         * rows (c188..c191) give |x| / |w| = |y| / |w| = 553.78 pixels
         * (46.85 degrees high; the camera block's D 553.75); the offset is
         * the viewport's (640x480 at 0, 0, bias 0.53125). Eye depth = w, one
         * unit 553.75 of the game's (an unscaled model's w row is 1/553.75). */
        .lens_k = 553.78f,
        .render_scale = 1.0f,   /* native, always: no dynamic resolution (ss1's) */
        .program_proj = { 553.78f, -553.78f, 320.53125f, 240.53125f, 1.0f },
        .hud_tiles = {          /* cut round its layout (quest_vulkan.inc's spread) */
            {  0,  0,440,110, 0,-1},   /* total time, the timer */
            {440,  0,640,110, 1,-1},   /* score, player life */
            {  0,110,640,412, 0, 0},   /* the middle: pause menu, lines */
            {  0,412,490,480, 0, 0},
            {490,412,640,480, 1, 1},   /* credits */
        },
        .hud_tile_count = 5,
        .rifle = ch_scope_vr_rifle,     /* the shared scope game */
        .cull = ch_scope_vr_cull,
        .lens_aim = ch_scope_lens_aim,
    },
    .gpu = {
        /* ss1's scale (the same engine's units: its main view's w is the
         * same within 0.3%): ~0.08 w a metre */
        .units_per_m = 0.08f,
        /* Its scope view is sampled on the GPU and published to the lens
         * from there, as ss1's; nothing reads a picture back from memory */
        .lazy_flip = 1,
    },
    .scope = &ss2_scope,
};

const ChihiroProfile *chihiro_game(void)
{
    return &k_ss2;
}
