/*
 * vr_aim.c - VC3's gun aim beyond its screen, for VR.
 *
 * The light gun reports where it points as a fraction of the game's 640x480
 * screen (JVS analog channels, 16-bit full scale = the screen), so a gun can
 * never aim outside it; in VR the player sees far more than the game's
 * 45-degree view. The game does not need the limit: sub_0007A160 (esi: the
 * player) turns the reading into pixels from the screen's centre, y up, and
 * sub_00078240 calibrates them into player+0x0C/+0x10; a shot is a ray from
 * the camera through that point and its lens (sub_0001EE40, sub_000B21E0),
 * with no clamp on the way. So after the game's own conversion, player 1's
 * aim is replaced by the host's (vc3_vr_aim_set), which may lie beyond the
 * screen -- unless the gun is off the screen (flag 0x400, the reload switch).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "recomp_types.h"

#define PLAYER1 0x3678E8u           /* player structs: 0x3678E8, 0x3679C0 */
#define OFF_SCREEN 0x400u           /* player+4: the gun sees no screen */

/* bit 63: set; bits 0-19 x, 20-39 y (two's complement), pixels */
static uint64_t s_aim;

void vc3_vr_aim_set(int on, int x, int y)
{
    uint64_t v = on ? (1ull << 63) | ((uint64_t)((uint32_t)x & 0xFFFFFu))
                      | ((uint64_t)((uint32_t)y & 0xFFFFFu) << 20) : 0;
    __atomic_store_n(&s_aim, v, __ATOMIC_RELAXED);
}

void vc3_gun_aim(void (*generated)(void))
{
    static int log = -1;
    static unsigned calls;
    const uint32_t player = g_esi;
    generated();
    uint64_t v = __atomic_load_n(&s_aim, __ATOMIC_RELAXED);
    if (log < 0) log = getenv("VC3_AIM_LOG") && atoi(getenv("VC3_AIM_LOG")) > 0;
    if (log && player == PLAYER1 && ++calls % 60 == 0)   /* VC3_AIM_LOG=1: player 1's every 60th, the game's and the host's */
        fprintf(stderr, "[aim] player %08X flags %08X game %d,%d host %s %d,%d\n", player, MEM32(player + 4),
                (int32_t)MEM32(player + 0x0C), (int32_t)MEM32(player + 0x10), (v >> 63) ? "on" : "off",
                (int32_t)((uint32_t)v << 12) >> 12, (int32_t)((uint32_t)(v >> 20) << 12) >> 12);
    if (player == PLAYER1 && (v >> 63) && !(MEM32(player + 4) & OFF_SCREEN)) {
        MEM32(player + 0x0C) = (uint32_t)((int32_t)((uint32_t)v << 12) >> 12);
        MEM32(player + 0x10) = (uint32_t)((int32_t)((uint32_t)(v >> 20) << 12) >> 12);
    }
}
