/*
 * Silent Scope (Konami; Xbox retail, the first game of Silent Scope
 * Complete, ss1.xbe): what it tells the shared runtime about itself
 * (chihiro_profile.h). A retail title: no baseboard, its controller through
 * USB, so the Chihiro-only blocks stay 0. Addresses are filled in as the
 * port finds them (games/ss1/README.md).
 */
#include <stddef.h>

#include "chihiro/chihiro_profile.h"

extern void ss1_diag_start(void);   /* ss1_diag.c: SS1_DIAG=1 */
extern void sub_000FC4D0(void);     /* D3D: insert a fence, kick unless flags & 2; lifted */

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
    },
    .hooks = {
        .on_boot = ss1_diag_start,
    },
};

const ChihiroProfile *chihiro_game(void)
{
    return &k_ss1;
}
