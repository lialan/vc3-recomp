/*
 * Chihiro hardware HLE for Virtua Cop 3.
 *
 * The game drives the Sega baseboard (JVS I/O, EEPROM, backup RAM, RTC) and
 * the media board through statically linked SEGA code; docs/chihiro-symbols.md
 * maps it. This layer replaces the game-side wrappers around that library so
 * no baseboard, media board or IDE channel is ever touched.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "xbox_memory_layout.h"
#include "chihiro.h"
#include "chihiro/ch_env.h"
#include "chihiro/chihiro_profile.h"
#include "host/scope_game.h"

static char s_save_dir[1024];

const char *chihiro_save_dir(void)
{
    return s_save_dir;
}

/* The launch data the Chihiro system software hands the game.
 *
 * A boot from the game's TestLaunch stub carries launch data that takes it
 * straight into its test menu (VC3: launch type 0, the first launch-data
 * dword 1). A normal boot has none, so the page is only installed when
 * VC3_TEST_LAUNCH=1, filled by the profile's launch_data hook.
 * XGetLaunchInfo reads the page from the kernel's LaunchDataPage, checks
 * the title id against the certificate's and copies 3 KB from +0x400. */
static void chihiro_set_launch_data(void)
{
    const char *test = ch_env("TEST_LAUNCH");
    const ChihiroProfile *gp = chihiro_profile();
    uint32_t page;

    if (!test || strcmp(test, "1") != 0 || !gp->hooks.launch_data)
        return;
    page = xbox_HeapAlloc(0x1000, 4096);
    if (!page) {
        fprintf(stderr, "[CHIHIRO] no memory for the launch data page\n");
        return;
    }
    memset((void *)XBOX_PTR(page), 0, 0x1000);
    gp->hooks.launch_data((uint8_t *)XBOX_PTR(page));
    MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = page;
    fprintf(stderr, "[CHIHIRO] launch data page at 0x%08X\n", page);
}

void chihiro_init(const char *save_dir)
{
    snprintf(s_save_dir, sizeof(s_save_dir), "%s", save_dir);
    fprintf(stderr, "[CHIHIRO] HLE init, save dir %s\n", s_save_dir);
    chihiro_set_launch_data();
    if (chihiro_profile()->scope)
        ch_scope_boot();     /* host/scope_game.h: the test rifle */
    if (chihiro_profile()->hooks.on_boot)
        chihiro_profile()->hooks.on_boot();
}
