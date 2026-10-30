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

#include "recomp_types.h"

#define STDCALL_ARG(n)  MEM32(g_esp + 4 + 4 * (n))

/* JammaFirmwareLoad(u32 which), stdcall: nonzero = started. */
void chihiro_jamma_firmware_load(void)
{
    fprintf(stderr, "[CHIHIRO] JammaFirmwareLoad(%u): no board, reporting loaded\n",
            STDCALL_ARG(0));
    g_eax = 1;
    g_esp += 8;
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
