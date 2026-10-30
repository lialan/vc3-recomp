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

static char s_save_dir[1024];

const char *chihiro_save_dir(void)
{
    return s_save_dir;
}

void chihiro_init(const char *save_dir)
{
    snprintf(s_save_dir, sizeof(s_save_dir), "%s", save_dir);
    fprintf(stderr, "[CHIHIRO] HLE init, save dir %s\n", s_save_dir);
}
