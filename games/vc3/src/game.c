/*
 * Virtua Cop 3 (Sega Chihiro, 2003): what it tells the Chihiro runtime about
 * itself (chihiro_profile.h). The numbers come from vc3.xbe as lifted
 * (docs/chihiro-symbols.md, scripts/find_chihiro_syms.py) and from
 * measurements on the game, each noted where the runtime uses it.
 */
#include <stddef.h>

#include "chihiro/chihiro_profile.h"
#include "chihiro/chihiro.h"

extern void vc3_vr_aim_set(int on, int x, int y);   /* vr_aim.c */
extern void vc3_vr_cull_set(float half_angle_rad);   /* vr_cull.c */
extern void sub_0010CA10(void);     /* D3D KickOff(0), lifted */

/* 16:9: only a widget against the screen's side (within 5% of it) that
 * keeps to that side's 3/8 is the side HUD -- the score panel and the gun
 * panel, at x 8. Anything else stays with the 4:3 picture: the Virtua City
 * map's labels and other things a player shoots at, whose hit areas the
 * game keeps in 4:3 coordinates, and the life bar, whose fill is 3D. */
static int vc3_hud_place(float x0, float x1, float w)
{
    if (x0 <= w * 0.05f && x1 <= w * 0.375f)
        return -1;
    if (x1 >= w * 0.95f && x0 >= w * 0.625f)
        return 1;
    return 0;
}

/* Launch data for a boot into the test menu (VC3_TEST_LAUNCH=1): type 0
 * with the first launch-data dword 1 means "started by the TestLaunch stub
 * (vc3_t.xbe)": 0x7FE70 then skips the I/O check ('IOER', 0x814F0) and
 * boots straight into the test menu (scene 7, 0x80CF0). XGetLaunchInfo
 * (0xB61AB) checks the title id against the certificate's (0). */
static void vc3_launch_data(uint8_t page[4096])
{
    *(uint32_t *)(page + 0x000) = 0;    /* LaunchDataType: title */
    *(uint32_t *)(page + 0x004) = 0;    /* title id, as in vc3.xbe's certificate */
    *(uint32_t *)(page + 0x400) = 1;    /* launched by TestLaunch */
}

static const ChihiroProfile k_vc3 = {
    .id = "vc3",
    .title = "Virtua Cop 3",
    .xbe = "vc3.xbe",
    .entry = 0x000B6407,                /* debug-key decoded */
    .env_prefix = "VC3",
    .save_name = "vc3-recomp",
    .xpp = {
        .backup_read = 0x0013DD90, .backup_write = 0x0013DEE0,
        .eeprom_read = 0x0013DA10, .eeprom_write = 0x0013DBD0, .rtc_read = 0x0013E5E0,
        .jvs_send = 0x0013E110, .jvs_receive = 0x0013E030, .jvs_link_up = 0x00104F80,
        .jamma_firmware_load = 0x0013D120, .sc_firmware_download = 0x0013D310,
        .dip_switches = 0x000F5220,
        /* 0x7D220 sets [0x36983C] when the boot memory probe (0x34F60) finds
         * more than 64 MB contiguous: at 1 the game sets up the JVS board */
        .devunit_probe = 0x0007D220, .devunit_flag = 0x0036983C,
    },
    .d3d = {
        .block_on_time = 0x0010CAC0, .device_ptr = 0x001178F8,
        .kickoff_ret = 0x0010CAEF, .kickoff = sub_0010CA10,
        .xlaunch = 0x000B6256,
    },
    /* The boot (0x39530) picks the display mode from the switches: index
     * (~dip >> 1) & 0xB through the table at 0xF88CC -- 10 640x480p, 9
     * 800x600, 11 1024x768. VC3 lays its 2D out at 640x480: the default. */
    .modes = {
        { NULL,   0x2, "640x480",  "2" },
        { "800",  0x4, "800x600",  "1" },
        { "1024", 0x0, "1024x768", "1" },
    },
    .jvs = {
        .ident = "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10",
        .players = 2, .switch_bytes = 2, .coin_slots = 2,
        .analog_channels = 8, .screen_channels = 2,
        /* the input test names push 3 CHANGE and push 4 PEDAL; the gun's
         * "off screen" switch (push 2) is the reload */
        .trigger = JVS_SW_PUSH1, .offscreen = JVS_SW_PUSH2,
        .change = JVS_SW_PUSH3, .pedal = JVS_SW_PUSH4,
    },
    /* GUN ADJUSTMENT: x = (byte - 128) * 2.5, y = (128 - byte) * 1.875;
     * dots TOP, LEFT, CENTER, RIGHT, BOTTOM at x 16/320/624, y 16/240/464 */
    .aim = {
        .backup_offset = 0x476,
        .defaults = { 5, 233, -318, 17, -10, 15, 242, 34, -10, -204 },
        .dots_x = { 16.0f, 320.0f, 624.0f }, .dots_y = { 16.0f, 240.0f, 464.0f },
        .units_x = 2.5f, .units_y = 1.875f,
        .screen_w = 640.0f, .screen_h = 480.0f,
    },
    .gpu = {
        .hud_place = vc3_hud_place,
        .picture_min_w = 320,           /* the attract movie, menu backgrounds */
        .lazy_rt = 1,                   /* the scene copies into 80C28000, sampled only in E.S. mode */
        .stereo_depth = 8.0f,           /* clip w: about metres */
        .stereo_sep = 0.04f,
        .units_per_m = 1.6f,   /* the camera stands 2.44 units up: ~1.6 per metre */
        .buffers = 4,                   /* triple buffered, and one spare */
        .zoom_tolerance = 0.5f,
        /* E.S. mode: 63 distortion layers; on a Quest 3 at native only a
         * quarter each way held 60 (half: ~53) */
        .effect_scale = 4,
        .effect_min_layers = 8,
        .effect_min_px = 2000000,
    },
    .vr = {
        .app_name = "Virtua Cop 3 VR",
        .panel_title = "Virtua Cop 3 - settings",
        .sdcard_dir = "/sdcard/VirtuaCop3",
        .sight_height = { 0.0285f, 0.0335f, 0.0400f },  /* quest_overlay.c's SIGHT_Y */
        .weapon = CH_VR_HANDGUN,
        .lens_k = 772.5f,       /* its usual 45 x 34.5 degree view (the culling frustum) */
        .hud_tiles = {          /* cut round its layout (quest_vulkan.inc's spread) */
            {  0,  0,200, 64,-1,-1},   /* score */
            {200,  0,440, 64, 0, 0},
            {440,  0,640, 64, 1,-1},   /* player 2's score */
            {  0, 64,640,280, 0, 0},   /* the middle */
            {218,280,430,395, 0, 0},
            {  0,280,218,480,-1, 1},   /* magazine, weapon, lives */
            {430,280,640,428, 1, 1},   /* player 2's */
            {390,428,640,480, 1, 1},   /* ... and PRESS START BUTTON */
            {218,395,430,428, 0, 1},   /* the gauge */
            {218,428,390,480, 0, 1},   /* credits */
        },
        .hud_tile_count = 10,
        .aim_screen = vc3_vr_aim_set,
        .cull = vc3_vr_cull_set,
    },
    .hooks = {
        .launch_data = vc3_launch_data,
    },
};

const ChihiroProfile *chihiro_game(void)
{
    return &k_vc3;
}
