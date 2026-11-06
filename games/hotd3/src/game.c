/*
 * The House of the Dead III (Sega / Wow Entertainment; Sega Chihiro, 2002):
 * what it tells the shared Chihiro runtime about itself (chihiro_profile.h).
 * Everything not set here is the runtime's default -- the same one VC3 and
 * Silent Scope run with. The addresses are hod3xb.xbe's as lifted (see
 * recomp_manual.c for how each was found).
 */
#include <stddef.h>

#include "chihiro/chihiro_profile.h"
#include "chihiro/chihiro.h"

extern void sub_0013BB70(void);     /* D3D KickOff(flags), lifted (VC3's 0x10CA10) */

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
    /* PROVISIONAL, VC3's gun numbers until HOTD3's GUN ADJUSTMENT is read
     * (Phase 3). backup_offset 0: no saved calibration is read, so VC3's
     * backup layout is never assumed; the defaults apply. */
    .aim = {
        .backup_offset = 0,
        .defaults = { 5, 233, -318, 17, -10, 15, 242, 34, -10, -204 },
        .dots_x = { 16.0f, 320.0f, 624.0f }, .dots_y = { 16.0f, 240.0f, 464.0f },
        .units_x = 2.5f, .units_y = 1.875f,
        .screen_w = 640.0f, .screen_h = 480.0f,
    },
};

const ChihiroProfile *chihiro_game(void)
{
    return &k_hotd3;
}
