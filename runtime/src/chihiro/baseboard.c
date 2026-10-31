/*
 * Chihiro baseboard firmware handshake.
 *
 * At boot the game (0xF4310) uploads d:\firmware.bin and firmware2.bin to the
 * baseboard's JVS and sound controllers and waits for them to start. With no
 * board, the SEGA library's transfers spin forever. The board is emulated at
 * the packet level (jvs.c), so there is no firmware to run: report it loaded.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "chihiro/ch_env.h"
#include "chihiro/chihiro_profile.h"

#define STDCALL_ARG(n)  MEM32(g_esp + 4 + 4 * (n))

/* JammaFirmwareLoad(u32 which), stdcall: nonzero = started. */
void chihiro_jamma_firmware_load(void)
{
    fprintf(stderr, "[CHIHIRO] JammaFirmwareLoad(%u): no board, reporting loaded\n",
            STDCALL_ARG(0));
    g_eax = 1;
    g_esp += 8;
}

/* The game's "is this a Chihiro" probe (profile xpp.devunit_probe): it sets
 * a flag (xpp.devunit_flag) when the boot memory probe finds more than 64 MB
 * of contiguous memory -- a Chihiro (128 MB) rather than an Xbox (64 MB).
 * VC3 (0x7D220, setting [0x36983C]) reads it in eight places: at 1 it sets
 * up the JVS I/O board and polls it every frame; at 0 it reads Xbox gamepads
 * and never touches JVS. Set it outright rather than trusting the probe
 * against the toolkit's contiguous window. cdecl. */
void chihiro_devunit_probe(void)
{
    MEM32(chihiro_profile()->xpp.devunit_flag) = 1;
    g_esp += 4;
}

/* 0xF5220: the baseboard DIP switches (via 0x104FB0), 0x80000000 if the board
 * does not answer. The boot (0x39530) picks the display mode from them:
 * index (~dip >> 1) & 0xB through the table at 0xF88CC -- 10 is 640x480
 * progressive, 9 800x600, 11 1024x768, 0 640x480 interlaced. With no board
 * the index is 11, but VC3 lays its 2D out at 640x480, so a 1024x768 frame
 * shows menus and text in its top-left corner. Default to a 31 kHz monitor at
 * 640x480. The modes come from the profile (modes[], the first the default:
 * VC3's 640x480p, 800x600, 1024x768); VC3_RES=800|1024 picks the others,
 * VC3_DIP sets the raw value. cdecl, no arguments. */
void chihiro_dip_switches(void)
{
    static int logged;
    const ChihiroProfile *gp = chihiro_profile();
    const char *raw = ch_env("DIP");
    const char *res = ch_env("RES");
    uint32_t dip = gp->modes[0].dip;
    int i;

    if (raw)
        dip = (uint32_t)strtoul(raw, NULL, 0);
    else
        for (i = 1; res && i < 4 && gp->modes[i].fb_size; i++)
            if (gp->modes[i].res && !strcmp(res, gp->modes[i].res))
                dip = gp->modes[i].dip;
    if (!logged) {
        logged = 1;
        fprintf(stderr, "[CHIHIRO] DIP switches 0x%08X (display index %u)\n",
                dip, (~dip >> 1) & 0xBu);
    }
    g_eax = dip;
    g_esp += 4;
}

/* JvsScFirmwareDownload(u32 off, u32 size, u8 *buf, u32 flags), stdcall: the
 * game reads back 4 bytes of sound-controller firmware status. */
void chihiro_sc_firmware_download(void)
{
    uint32_t size = STDCALL_ARG(1), buf = STDCALL_ARG(2), i;

    for (i = 0; buf && i < size && i < 64; i++)
        MEM8(buf + i) = 0;
    g_eax = 0;
    g_esp += 20;
}
