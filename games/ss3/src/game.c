/*
 * Silent Scope 3 (Konami; Xbox retail, the third game of Silent Scope
 * Complete, ss3.xbe): what it tells the shared runtime about itself
 * (chihiro_profile.h). The same XDK 5558 build as ss1: retail, the
 * controller through USB, the Chihiro-only blocks 0.
 */
#include <stddef.h>

#include "chihiro/chihiro_profile.h"

extern void sub_00175F30(void);     /* D3D: insert a fence, kick unless flags & 2; lifted */

static const ChihiroProfile k_ss3 = {
    .id = "ss3",
    .platform = CHIHIRO_PLATFORM_XBOX,
    .title = "Silent Scope 3",
    .xbe = "ss3.xbe",
    .entry = 0x00161FC3,                /* retail-key decoded */
    .env_prefix = "SS3",
    .save_name = "ss3-recomp",
    /* XDK 5558 D3D, as ss1's (found by its byte pattern): BlockOnTime at
     * 0x175FE0, replaced by the runtime's (recomp_manual.c). */
    .d3d = {
        .block_on_time = 0x00175FE0,
        .device_ptr = 0x0017FFD8,       /* g_pDevice */
        .kickoff_ret = 0x0017600F,      /* where 0x175FE0's own call returns */
        .kickoff = sub_00175F30,
    },
    .vr = {
        .app_name = "Silent Scope 3 VR",
        .panel_title = "Silent Scope 3 - settings",
        .sdcard_dir = "/sdcard/SilentScope3",
    },
};

const ChihiroProfile *chihiro_game(void)
{
    return &k_ss3;
}
