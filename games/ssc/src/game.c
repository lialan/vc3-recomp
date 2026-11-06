/*
 * Silent Scope Complete's launcher (Konami; Xbox retail, default.xbe): the
 * menu that starts Silent Scope 1, 2 or 3. What it tells the shared runtime
 * about itself (chihiro_profile.h). The same XDK 5558 build as the games:
 * retail, the controller through USB, the Chihiro-only blocks 0. Starting a
 * game is the kernel's launch (XLaunchNewImage's HalReturnToFirmware with a
 * launch data page): the runtime runs that game's binary beside this one
 * (main.c chihiro_launch_image; ssN.xbe runs the ssN binary, and quitting a
 * game launches default.xbe, this one, again).
 *
 * The menu (sub_00015BA0, its state at 0xFF970): the title (Start), the game
 * selection (state 11, the three cards turning by themselves), then A for
 * its cursor (state 15, from 240, 400 on the 640x480 screen; the stick or
 * the held d-pad moves it about 7 pixels a frame, or a light gun places it
 * -- the input reader sub_00012950, [0x108DBC] set). A with the cursor on a
 * card (its hit test sub_00011F30) picks that game (0xFF974: 1 ss1, 2 ss2,
 * 3 ss3), and a few frames later it checks d:\ssN.xbe exists and calls
 * XLaunchNewImage (sub_00019171) with its launch data (0x108020). A game
 * launched this way starts at its own main menu; its quit ("Quit Silent
 * Scope 2, OK?") launches default.xbe, which comes back at its title.
 */
#include <stddef.h>

#include "chihiro/chihiro_profile.h"

extern void sub_00094E40(void);     /* D3D: insert a fence, kick unless flags & 2; lifted */

static const ChihiroProfile k_ssc = {
    .id = "ssc",
    .platform = CHIHIRO_PLATFORM_XBOX,
    .title = "Silent Scope Complete",
    .xbe = "default.xbe",
    .entry = 0x0001B2C1,                /* retail-key decoded */
    .env_prefix = "SSC",
    .save_name = "ssc-recomp",
    .disc_launcher = "ssc",             /* itself: d:\ssN.xbe runs build's ssN */
    /* XDK 5558 D3D, as the games' (found by its byte pattern): BlockOnTime
     * at 0x94EF0, replaced by the runtime's (recomp_manual.c). */
    .d3d = {
        .block_on_time = 0x00094EF0,
        .device_ptr = 0x0009F7C8,       /* g_pDevice */
        .kickoff_ret = 0x00094F1F,      /* where 0x94EF0's own call returns */
        .kickoff = sub_00094E40,
        .no_multisample = 1,            /* 640x480, not its 1280x480 quincunx buffer */
    },
    /* On the headset: the games' app folder and controls -- the gun hand's
     * thumbstick is the d-pad (the menu's cursor), A / B are A / B, a stick
     * click Start (quest_host.c rifle_frame; no rifle hooks: nothing to
     * aim). Its screens are 2D, shown on the flat screen. */
    .vr = {
        .app_name = "Silent Scope Complete VR",
        .panel_title = "Silent Scope Complete - settings",
        .sdcard_dir = "/sdcard/SilentScope",    /* the three games' folder */
        .weapon = CH_VR_RIFLE,
    },
};

const ChihiroProfile *chihiro_game(void)
{
    return &k_ssc;
}
