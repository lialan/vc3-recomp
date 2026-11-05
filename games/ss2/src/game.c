/*
 * Silent Scope 2 (Konami; Xbox retail, the second game of Silent Scope
 * Complete, ss2.xbe): what it tells the shared runtime about itself
 * (chihiro_profile.h). The same XDK 5558 build as ss1: retail, the
 * controller through USB, the Chihiro-only blocks 0.
 */
#include <stddef.h>

#include "chihiro/chihiro_profile.h"

extern void sub_0012CAA0(void);     /* D3D: insert a fence, kick unless flags & 2; lifted */

static const ChihiroProfile k_ss2 = {
    .id = "ss2",
    .platform = CHIHIRO_PLATFORM_XBOX,
    .title = "Silent Scope 2",
    .xbe = "ss2.xbe",
    .entry = 0x000B23EC,                /* retail-key decoded */
    .env_prefix = "SS2",
    .save_name = "ss2-recomp",
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
        .sdcard_dir = "/sdcard/SilentScope2",
    },
};

const ChihiroProfile *chihiro_game(void)
{
    return &k_ss2;
}
