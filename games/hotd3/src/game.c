/*
 * The House of the Dead III (Sega / Wow Entertainment; Sega Chihiro, 2002):
 * what it tells the shared Chihiro runtime about itself (chihiro_profile.h).
 * Everything not set here is the runtime's default -- the same one VC3 and
 * Silent Scope run with. The addresses are hod3xb.xbe's as lifted (see
 * recomp_manual.c for how each was found).
 */
#include <stddef.h>
#include <stdint.h>

#include "chihiro/chihiro_profile.h"
#include "chihiro/chihiro.h"

extern void sub_0013BB70(void);     /* D3D KickOff(flags), lifted (VC3's 0x10CA10) */
extern void hotd3_vr_aim_set(int on, int x, int y);    /* vr_aim.c */
extern void hotd3_vr_cull_set(float half_angle_rad);   /* vr_cull.c */

/* Launch data for a boot into the test menu (HOTD3_TEST_LAUNCH=1), as the
 * TestLaunch stub acdemoT.xbe (Sega's sample, VC3's vc3_t.xbe's twin) hands
 * it: type 0, title id 0, the first launch-data dword 1. */
static void hotd3_launch_data(uint8_t page[4096])
{
    *(uint32_t *)(page + 0x000) = 0;    /* LaunchDataType: title */
    *(uint32_t *)(page + 0x004) = 0;    /* title id, as in the certificate */
    *(uint32_t *)(page + 0x400) = 1;    /* launched by TestLaunch */
}

static const ChihiroProfile k_hotd3 = {
    .id = "hotd3",
    .title = "The House of the Dead III",
    .xbe = "hod3xb.xbe",
    .entry = 0x000B8BAC,                /* debug-key decoded */
    .env_prefix = "HOTD3",
    .save_name = "hotd3-recomp",
    /* Sega's I/O library, the same build as VC3's (VC3's VA + 0x2D560) */
    .xpp = {
        .backup_read = 0x0016B2F0, .backup_write = 0x0016B440,
        .eeprom_read = 0x0016AF70, .eeprom_write = 0x0016B130, .rtc_read = 0x0016BB40,
        .jvs_send = 0x0016B670, .jvs_receive = 0x0016B590, .jvs_link_up = 0x00121FF0,
        .jamma_firmware_load = 0x0016A680, .sc_firmware_download = 0x0016A870,
        .dip_switches = 0x0011CD10,
        /* no "is a Chihiro" memory probe (VC3's 0x7D220): HOTD3 always
         * drives the JVS board */
    },
    /* XDK 4831's D3D: g_pDevice is the global its D3D loads most (107
     * times); KickOff writes the semaphore release (0x41D70) with the time
     * at [device+0x30]; BlockOnTime (310 bytes, ret 8, as VC3's) compares
     * it with *[device+0x34] and kicks off from 0x13BC4A */
    .d3d = {
        .block_on_time = 0x0013BC20, .device_ptr = 0x00147800,
        .kickoff_ret = 0x0013BC4F, .kickoff = sub_0013BB70,
        .xlaunch = 0x000B88D1,
        .dev_time = 0x30, .dev_semaphore = 0x34,
    },
    /* The boot (0xB1210) picks the display mode as VC3's does: index
     * (~dip >> 1) & 0xB; 10 is 640x480 progressive */
    .modes = {
        { NULL,   0x2, "640x480",  "2" },
    },
    /* VC3's I/O board, to start with: the same Sega library asks for it
     * (Phase 2 checks what HOTD3's I/O check wants) */
    .jvs = {
        .ident = "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10",
        .players = 2, .switch_bytes = 2, .coin_slots = 2,
        .analog_channels = 8, .screen_channels = 2,
        .trigger = JVS_SW_PUSH1, .offscreen = JVS_SW_PUSH2,
    },
    /* GUN SETTING > PLAYER1 GUN ADJUSTMENT: shoot targets centred at
     * (75.5, 53.5), (563.5, 419.5) and (319.5, 236.5) of 640x480; the readings
     * (the analog channels' top bytes, which INPUT TEST shows as GUN-X/Y) go
     * to EEPROM at 0x90 as dwords, x then y, top-left/centre/bottom-right,
     * player 2's after them, in a "HOD4EEP0" record at 0x80. A blank EEPROM:
     * the game's own player-1 defaults, as PLAYER1 GUN DEFAULT ADJUSTMENT
     * stores them (x 0x02/0x76/0xDC, y 0x1B/0x88/0xE3; player 2's differ),
     * here in the runtime's units. */
    .aim = {
        .eeprom_offset = 0x90,
        .defaults = { 0, 101, -126, 0, -10, -8, 92, 0, 0, -99 },
        .dots_x = { 75.5f, 319.5f, 563.5f }, .dots_y = { 53.5f, 236.5f, 419.5f },
        .units_x = 1.0f, .units_y = 1.0f,
        .screen_w = 640.0f, .screen_h = 480.0f,
    },
    .gpu = {
        /* the lab's walkway is ~10.8 units under the camera, a hunched
         * zombie on it ~12.7 tall (eye-space boxes of chapter 1's batches):
         * ~7.5 a metre */
        .units_per_m = 7.5f,
        /* its eyes are fragment-bound on the Quest and ~46% of their draws
         * alpha-test: the rest get a discard-free shader (5-9% less GPU
         * work in headset A/Bs, the same frame rate) */
        .split_alpha = 1,
    },
    .vr = {
        .app_name = "House of the Dead III VR",
        .panel_title = "House of the Dead III - settings",
        .sdcard_dir = "/sdcard/HouseOfTheDead3",
        /* a pump-action shotgun, as the cabinet's guns are shaped: the
         * handgun's controls, its sights the shotgun's own (the overlay's) */
        .weapon = CH_VR_SHOTGUN,
        .lens_k = 625.2f,       /* its world's projection, as the renderer reads it */
        /* Quest GPU (headset A/Bs, scripted chapter 1): the gun layer at 0.6
         * of the eye costs 41-47 ms of GPU a second against 70 at 0.75 (0.5:
         * 33-40, the panel's text softer); full-rate shading within 22
         * degrees, 2x1 to 35, 2x2 to 50, 4x4 beyond: 70-72% GPU against 74%
         * at 30, 45, 60 (18, 30, 45 no better) */
        .overlay_scale = 0.6f,
        .foveation = { 22.0f, 35.0f, 50.0f },
        /* All its program batches are one program, D3D's pass-through for
         * pre-transformed vertices: the HUD (rhw 10), text and sprites at or
         * in front of the near plane (w 168, 560.5), full-screen copies (1)
         * -- 2D; the world is fixed-function */
        .program_flat_w = 1000.0f,
        /* player 1's life, RELOAD and CONTINUE go into the scene buffer
         * (0x80001000) before it is copied onto the screen */
        .scene_hud = 1,
        .aim_screen = hotd3_vr_aim_set,
        .cull = hotd3_vr_cull_set,
        /* its stage parts are drawn whole, all round the camera (vr_cull.c):
         * turning the head shows them, but for the room behind a door the
         * player has gone through, which it does not draw at all */
        .world_behind = 1,
        /* it clears its scene to the stage's fog colour, red in the lab (fog
         * from 150 units, past its rooms): where it draws nothing -- that
         * doorway -- black reads as distance, red as a hole */
        .eye_clear_black = 1,
        /* its HUD: player 1's P1 and life at the bottom left, player 2's (or
         * PRESS START BUTTON, CREDIT(S)) at the bottom right, RELOAD left of
         * the middle over its player's side, CONTINUE? in the middle */
        .hud_tiles = {
            {  0,  0,240,200,-1,-1},
            {240,  0,400,200, 0,-1},
            {400,  0,640,200, 1,-1},
            {  0,200,240,380,-1, 0},   /* player 1's RELOAD */
            {240,200,400,380, 0, 0},   /* CONTINUE? */
            {400,200,640,380, 1, 0},   /* player 2's */
            {  0,380,240,480,-1, 1},   /* P1, life */
            {240,380,400,480, 0, 1},
            {400,380,640,480, 1, 1},   /* player 2, credits */
        },
        .hud_tile_count = 9,
    },
    .hooks = {
        .launch_data = hotd3_launch_data,
    },
};

const ChihiroProfile *chihiro_game(void)
{
    return &k_hotd3;
}
