/*
 * Silent Scope 3 (Konami; Xbox retail, the third game of Silent Scope
 * Complete, ss3.xbe): what it tells the shared runtime about itself
 * (chihiro_profile.h). The same XDK 5558 build as ss1: retail, the
 * controller through USB, the Chihiro-only blocks 0. Its VR is ss1's --
 * the shared scope game (host/scope_game.h; its addresses in scope.c) and
 * ss1's settings.
 */
#include <stddef.h>

#include "chihiro/chihiro_profile.h"
#include "host/scope_game.h"

extern void sub_00175F30(void);     /* D3D: insert a fence, kick unless flags & 2; lifted */
extern const ChScopeGame ss3_scope; /* scope.c */

static const ChihiroProfile k_ss3 = {
    .id = "ss3",
    .platform = CHIHIRO_PLATFORM_XBOX,
    .title = "Silent Scope 3",
    .xbe = "ss3.xbe",
    .entry = 0x00161FC3,                /* retail-key decoded */
    .env_prefix = "SS3",
    .save_name = "ss3-recomp",
    .disc_launcher = "ssc",             /* Silent Scope Complete's menu (default.xbe) */
    /* XDK 5558 D3D, as ss1's (found by its byte pattern): BlockOnTime at
     * 0x175FE0, replaced by the runtime's (recomp_manual.c). */
    .d3d = {
        .block_on_time = 0x00175FE0,
        .device_ptr = 0x0017FFD8,       /* g_pDevice */
        .kickoff_ret = 0x0017600F,      /* where 0x175FE0's own call returns */
        .kickoff = sub_00175F30,
        .no_multisample = 1,            /* 640x480, not its 1280x480 quincunx buffer */
    },
    .vr = {
        .app_name = "Silent Scope 3 VR",
        .panel_title = "Silent Scope 3 - settings",
        .sdcard_dir = "/sdcard/SilentScope",    /* ss1's: the Silent Scope Complete folder */
        .weapon = CH_VR_RIFLE,
        /* Its 3D is all vertex programs, in the PS2's screen units round
         * 2048: screen = (x / w - 2048) * (1.25, 1.0714) + (320, 240), the
         * matrix in c88..c91. As 640x480 pixels: kx 554.29, ky -542.75 (the
         * main view's D 443.44 in its 512x384 units); it put the scope's
         * disc where the aim was within 2 pixels. */
        .lens_k = 554.29f,
        .render_scale = 1.0f,   /* native, always: no dynamic resolution (ss1's) */
        .program_proj = { 554.29f, -542.75f, 320.0f, 240.0f, 1.0f },
        .hud_tiles = {          /* cut round its layout (quest_vulkan.inc's spread) */
            {  0,  0,200,140, 0,-1},
            {200,  0,640,140, 1,-1},   /* mission time, score, life, rank */
            {  0,140,640,370, 0, 0},   /* the middle: the scope disc, lines */
            {  0,370,110,480,-1, 1},   /* ammunition */
            {110,370,640,480, 0, 0},
        },
        .hud_tile_count = 5,
        .rifle = ch_scope_vr_rifle,     /* the shared scope game */
        .cull = ch_scope_vr_cull,
        .lens_aim = ch_scope_lens_aim,
        .flat_now = ch_scope_flat_now,  /* its scope view is drawn into the screen, then copied */
    },
    .gpu = {
        /* ss1's scale for now (an estimate: <PREFIX>_IMM_UNITS tunes it on
         * the headset) */
        .units_per_m = 0.08f,
        /* Its scope picture is sampled on the GPU and published to the lens
         * from there, as ss1's; nothing reads a picture back from memory */
        .lazy_flip = 1,
    },
    .scope = &ss3_scope,
};

const ChihiroProfile *chihiro_game(void)
{
    return &k_ss3;
}
