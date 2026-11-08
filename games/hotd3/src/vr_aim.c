/*
 * vr_aim.c - HOTD3's gun aim beyond its screen, for VR.
 *
 * The light gun reports where it points as a fraction of the game's 640x480
 * screen (JVS analog channels), so a gun can never aim outside it; in VR the
 * player sees far more than the game's 54 x 42 degree view. The game does not
 * need the limit. Once a frame (from 0x84E0F) sub_00079E80 turns each
 * player's reading (player 1's words at 0x27711A/0x27711C) through the
 * EEPROM calibration (sub_00079DE0) into pixels from the screen's centre, y
 * up -- player 1's at 0x2CB5E0 +4 (x) and +6 (y), signed words; player 2's
 * 0x34 on; GUN MARK CHECK (sub_000AC8E0) draws it at 320 + x, 240 - y --
 * keeping it only inside the screen (x -320..319, y -240..239; else 0, and
 * +0x10, "on the screen", clear). sub_000961F0 hands +4/+6 to the player as
 * floats, and a shot (sub_000925D0: trigger +0x16, on the screen +0x10, a
 * bullet left) is a ray from the camera through (x, y, -focal)
 * (sub_000492F0; the focal [0x27A148] = 240 / tan(FOVY / 2)), normalised
 * (sub_000F93F0) and tested in the world (0x43DE0, 0x44340) -- no clamp on
 * the way. So after the game's own conversion, player 1's aim is replaced by
 * the host's (hotd3_vr_aim_set), which may lie beyond the screen, as the game
 * converts it: in modes 0xA and 0xC ([0x27A410]) with player 1 playing
 * (state [0x276708] 5, 8 or 0xB), unless the gun is off the screen (+0x11
 * clear: its SCREEN-IN switch off, JVS push 2, which a reload holds with
 * GRIP, push 3 -- the press HOTD3 reloads by, gun +0x18; game.c).
 * The shot's lens is not the world's: the player update (sub_00093790) sets
 * the camera's FOVY to 45 degrees (0x938A5) for the shot, the bullet holes
 * and the HUD, and puts the camera's back after (0x939EE) -- in chapter 1's
 * lab the world is drawn at focal 625.2, its shots at 579.4. The host's
 * point is where the gun points in the world's picture (the frame's
 * projection, which [0x27A148] holds again by the next frame), so it is
 * scaled by 579.4 / that focal: the shot then goes where the gun points.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "recomp_types.h"

#define GUN1 0x2CB5E0u              /* player 1's gun: +4 x, +6 y, +0x10 on the screen, +0x11 sees it */
#define MODE 0x27A410u              /* the game's mode */
#define STATE1 0x276708u            /* player 1's state (players 0x22C apart) */
#define FOCAL 0x27A148u             /* 240 / tan(FOVY / 2), pixels */
#define SHOT_FOCAL 579.41f          /* 240 / tan(22.5 degrees): the shot's 45-degree FOVY */
/* For the log (HOTD3_AIM_LOG=1): player 1's shells (+0x60, six a load; a
 * hit taken loads six too, sub_00092AB0) and shots fired (+0x70,
 * sub_000925D0); its switches held, as sub_00011950 decodes them (bit 0
 * TRIGGER, 1 the gun off the screen -- JVS push 2, INPUT TEST's SCREEN-IN
 * -- 3 GRIP, push 3, whose press is the reload: gun +0x18); its last
 * shot's ray, camera space, unit (sub_000492F0: (x, y, -[FOCAL])
 * normalised, the shot's lens in [FOCAL] then) */
#define SHELLS1 0x276760u
#define SHOTS1 0x276770u
#define HELD1 0x27A3ECu
#define SHOT_RAY1 (0x277138u + 0x54u)

/* bit 63: set; bits 0-19 x, 20-39 y (two's complement), pixels */
static uint64_t s_aim;

void hotd3_vr_aim_set(int on, int x, int y)
{
    uint64_t v = on ? (1ull << 63) | ((uint64_t)((uint32_t)x & 0xFFFFFu))
                      | ((uint64_t)((uint32_t)y & 0xFFFFFu) << 20) : 0;
    __atomic_store_n(&s_aim, v, __ATOMIC_RELAXED);
}

/* HOTD3_AIM_LOG=1: each change of player 1's switches, shells and reload,
 * and each shot -- its ray against the point the game was handed for it. */
static void aim_log(uint32_t mode, uint32_t state, int host, int host_x, int host_y)
{
    static uint32_t shells = 0xFFFFFFFFu, shots, held = 0xFFFFFFFFu, reload;
    static int handed_x, handed_y, handed_host, raw_x, raw_y;
    static float focal;
    const uint32_t now_shells = MEM32(SHELLS1), now_shots = MEM32(SHOTS1), now_held = MEM32(HELD1) & 0xFu;
    const uint32_t now_reload = MEM8(GUN1 + 0x18);
    if (now_held != held)
        fprintf(stderr, "[aim] switches %X -> %X: trigger %u off the screen %u grip %u; gun on the screen %u,"
                " sees it %u, trigger %u, reload %u; mode %X state %X shells %u\n", held & 0xFu, now_held,
                now_held & 1u, (now_held >> 1) & 1u, (now_held >> 3) & 1u, MEM8(GUN1 + 0x10), MEM8(GUN1 + 0x11),
                MEM8(GUN1 + 0x16), now_reload, mode, state, now_shells);
    if (now_reload && !reload)
        fprintf(stderr, "[aim] reload (GRIP pressed): mode %X state %X shells %u\n", mode, state, now_shells);
    if (now_shells != shells)
        fprintf(stderr, "[aim] shells %d -> %d: mode %X state %X switches %X\n", (int)shells, (int)now_shells,
                mode, state, now_held);
    if (now_shots != shots && shots) {   /* a shot since the last frame, with the aim it was handed then */
        const float dx = MEMF(SHOT_RAY1), dy = MEMF(SHOT_RAY1 + 4), dz = MEMF(SHOT_RAY1 + 8);
        fprintf(stderr, "[aim] shot %u: ray %.5f %.5f %.5f = %.2f,%.2f px at the shot's lens; handed %d,%d (%s",
                now_shots, dx, dy, dz, dz < 0 ? -SHOT_FOCAL * dx / dz : NAN, dz < 0 ? -SHOT_FOCAL * dy / dz : NAN,
                handed_x, handed_y, handed_host ? "the host's" : "the game's");
        if (handed_host)
            fprintf(stderr, " %d,%d at the world's focal %.1f", raw_x, raw_y, focal);
        fprintf(stderr, ")\n");
    }
    held = now_held;
    reload = now_reload;
    shells = now_shells;
    shots = now_shots;
    handed_x = (int16_t)MEM16(GUN1 + 4);
    handed_y = (int16_t)MEM16(GUN1 + 6);
    handed_host = host;
    raw_x = host_x;
    raw_y = host_y;
    focal = MEMF(FOCAL);
}

void hotd3_gun_aim(void (*generated)(void))
{
    static int log = -1;
    static unsigned calls;
    int host = 0;
    generated();
    const uint64_t v = __atomic_load_n(&s_aim, __ATOMIC_RELAXED);
    const int x = (int32_t)((uint32_t)v << 12) >> 12, y = (int32_t)((uint32_t)(v >> 20) << 12) >> 12;
    const uint32_t mode = MEM32(MODE), state = MEM32(STATE1);
    const int converted = (mode == 0xA || mode == 0xC) && (state == 5 || state == 8 || state == 0xB);
    if (log < 0) log = getenv("HOTD3_AIM_LOG") && atoi(getenv("HOTD3_AIM_LOG")) > 0;
    if (log && ++calls % 60 == 0)   /* HOTD3_AIM_LOG=1: every 60th, the game's and the host's */
        fprintf(stderr, "[aim] mode %X state %X on %u sees %u game %d,%d host %s %d,%d focal %.1f\n", mode, state,
                MEM8(GUN1 + 0x10), MEM8(GUN1 + 0x11), (int16_t)MEM16(GUN1 + 4), (int16_t)MEM16(GUN1 + 6),
                (v >> 63) ? "on" : "off", x, y, MEMF(FOCAL));
    if ((v >> 63) && converted && MEM8(GUN1 + 0x11)) {
        const float world = MEMF(FOCAL);
        const float k = isfinite(world) && world > 1.0f ? SHOT_FOCAL / world : 1.0f;
        ST16(GUN1 + 4, (int16_t)lroundf((float)x * k));
        ST16(GUN1 + 6, (int16_t)lroundf((float)y * k));
        ST8(GUN1 + 0x10, 1);
        host = 1;
    }
    if (log)
        aim_log(mode, state, host, x, y);
}
