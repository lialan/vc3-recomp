/*
 * Chihiro hardware HLE for Virtua Cop 3.
 *
 * The game drives the Sega baseboard (JVS I/O, EEPROM, backup RAM, RTC) and
 * the media board through statically linked SEGA code; docs/chihiro-symbols.md
 * maps it. This layer replaces the game-side wrappers around that library so
 * no baseboard, media board or IDE channel is ever touched.
 */

#include <stdio.h>
#include <string.h>

#include "recomp_types.h"
#include "xbox_memory_layout.h"
#include "chihiro.h"

static char s_save_dir[1024];

const char *chihiro_save_dir(void)
{
    return s_save_dir;
}

/* The launch data the Chihiro system software hands the game.
 *
 * VC3 marks its I/O ready (0x7FE70, the flag 0x7FEE0 tests; without it the
 * game raises "JVS I/O board is not connected") only when XGetLaunchInfo
 * returns launch type 0 with the first launch-data dword 1: launched by the
 * boot software after its I/O check, as the TestLaunch stub (vc3_t.xbe) does.
 * XGetLaunchInfo (0xB61AB) reads the page from the kernel's LaunchDataPage,
 * checks the title id against the certificate's (0 here) and copies 3 KB from
 * +0x400. */
static void chihiro_set_launch_data(void)
{
    uint32_t page = xbox_HeapAlloc(0x1000, 4096);

    if (!page) {
        fprintf(stderr, "[CHIHIRO] no memory for the launch data page\n");
        return;
    }
    memset((void *)XBOX_PTR(page), 0, 0x1000);
    MEM32(page + 0x000) = 0;          /* LaunchDataType: title */
    MEM32(page + 0x004) = 0;          /* title id, as in vc3.xbe's certificate */
    MEM32(page + 0x400) = 1;          /* launched after the I/O check */
    MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = page;
    fprintf(stderr, "[CHIHIRO] launch data page at 0x%08X\n", page);
}

void chihiro_init(const char *save_dir)
{
    snprintf(s_save_dir, sizeof(s_save_dir), "%s", save_dir);
    fprintf(stderr, "[CHIHIRO] HLE init, save dir %s\n", s_save_dir);
    chihiro_set_launch_data();
}
