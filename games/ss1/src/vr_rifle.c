/*
 * vr_rifle.c - Silent Scope's side of the VR rifle (ChihiroProfile.vr.rifle,
 * .cull; host/vr_rifle.h): the rifle aims the scope, zooms it, and the
 * scope's own picture goes to the lens instead of the circle on the screen.
 *
 * Aim. sub_000588C0 steps the aim -- pitch 0x1CEB75C, yaw 0x1CEB75E, int16,
 * 65536 a turn -- by the stick and clamps it. After it (and the 16:9 step,
 * wide.c) the rifle's angles replace both, unclamped. The scope camera
 * (sub_00059F80) turns the main camera (matrix 0x1CEB084) by them, and a
 * shot flies from it along minus the scope matrix's third row (0x1CEB210,
 * sub_0003EB30). Measured in stage 1 with the stick held (SS1_AIM_LOG=1):
 * right takes the yaw negative (0 to -4800 in 4 s; the circle moves right,
 * the shot 26.4 degrees right of the main camera's ahead), up takes the
 * pitch positive (0 to 3600 in 3 s; the circle moves up, the shot 19.8
 * degrees up). The scope turns by the yaw, then by the pitch about its
 * turned right axis -- the rifle's own convention -- so the angles go
 * across as they are, the yaw negated.
 *
 * Zoom. The scope's focal length (sub_00074610, for the next frame) is
 * multiplied by 0x1ADFC8, which only sub_00075F40 writes: 1.0, or 1/s for
 * the type-3 scope (sub_000393F0() == 3; s = sub_00039420()). With the rifle
 * on, that value times the rifle's zoom.
 *
 * Not drawing the circle. sub_00075F40 draws the scope circle (the scope
 * target as a texture, its frame); sub_00076490 draws it at the aim and two
 * aim marks (models 0x1DB2482 / 0x1DB2480, calls at 0x766E4 / 0x7670A).
 * Their writes besides drawing, which the replacements keep (read from the
 * lifted bodies and everything they call):
 *   sub_00076490: 0x1CEA0A8 = [0x15B588] when (int8)[0x1CEBEF0] >= 0;
 *                 0x18F6E24 = 1 when [0x1D4AC4C] == 0 and [0x1ED224] == 0;
 *                 sub_00075F40's, when it calls it ([0x1CEA0A0] != 1 and
 *                 [0x1CEA0A4] or [0x1CEA0B0] set).
 *   sub_00075F40: 0x1ADFC8 (above); 0x1CEA13C, left 1 (a render-state
 *                 flag, also set 1 by the draw helpers); 0x1D4AC68 bit 3
 *                 cleared and set back (no net change).
 * The rest is drawing: the draw list behind [0x1D67004] (sub_000443D0,
 * sub_00044550), the matrix stack 0x1175998 / 0x11759DC (pushes and pops
 * balance; the current matrix is left as the marks' transform), the 2D
 * projection state 0x1D1F99C, 0x1D0BF80-0x1D0BFDC, 0x1D0C030-38 (set again
 * by every 2D draw), render-state shadows 0x1CEA0B8-0x1CEA0CC, 0x1185A60/64,
 * 0x1175A58-60, 0x1D0BF60, 0x1A5440, 0x1D1FAE4, 0x18F6E20, the draw batches
 * 0x1D1F960, 0x1D0C0BC.., 0x1D15D20.., shader constants 0x1CE4E5C-80,
 * 0x118DC88, 0x1CE4D84, and D3D's own state (0x105xxx, 0x162xxx, 0x1ED230/34).
 * With the rifle on, sub_00075F40 draws nothing; sub_00076490 draws nothing
 * when the scope pass (sub_000744B0) ran this frame, so the lens is the only
 * aim, and otherwise is the game's (its marks are the aim when there is no
 * scope picture).
 *
 * The lens surface. The scope pass renders into a 256x256 target (D3D
 * surface object at [0x1ED214]); its first run publishes the surface's
 * address for the renderer (vc3_vr_lens_surface_set).
 *
 * SS1_VR_RIFLE_TEST=yaw_deg,pitch_deg,zoom: a rifle held still, set once at
 * boot (test only). SS1_AIM_LOG=1: the aim and the shot direction relative
 * to the main camera, once a second.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "recomp_types.h"
#include "chihiro/ch_env.h"
#include "host/vr_rifle.h"
#include "host/vr_view.h"

#define AIM_PITCH   0x1CEB75Cu
#define AIM_YAW     0x1CEB75Eu
#define FRAME       0x1CEAF90u      /* the main loop's frame counter */
#define ZOOM_K      0x1ADFC8u       /* the scope focal length's factor */
#define ONE         0x14D304u       /* the game's 1.0 */
#define SCOPE_SURF  0x1ED214u       /* the scope target's D3D surface object */
#define MAIN_CAM    0x1CEB084u      /* 3x3 int16, rows the camera's axes */
#define SCOPE_CAM   0x1CEB204u

/* The game's yaw and pitch for the rifle's right and up (measured, above). */
#define YAW_RIGHT   (-1)
#define PITCH_UP    1

/* bits 0-15 yaw, 16-31 pitch (the game's units), 32-63 the zoom (float
 * bits); 0: off. One word, so a frame reads one rifle (as vc3_vr_aim_set). */
static uint64_t s_rifle;
static float s_cull_half;
static int64_t s_scope_frame = -1;  /* the frame the scope pass last ran */

static int16_t game_angle(float rad)
{
    double u = isfinite(rad) ? rad * 65536.0 / 6.283185307179586 : 0.0;
    return (int16_t)lround(u < -32768.0 ? -32768.0 : u > 32767.0 ? 32767.0 : u);
}

void ss1_vr_rifle(const ChVrRifle *r)
{
    uint64_t v = 0;
    if (r && r->on) {
        float z = isfinite(r->zoom) ? r->zoom : 1.0f;
        uint32_t zb;
        z = z < 0.5f ? 0.5f : z > 4.0f ? 4.0f : z;
        memcpy(&zb, &z, sizeof zb);
        v = (uint64_t)(uint16_t)game_angle(YAW_RIGHT * r->yaw)
            | (uint64_t)(uint16_t)game_angle(PITCH_UP * r->pitch) << 16
            | (uint64_t)zb << 32;
    }
    __atomic_store_n(&s_rifle, v, __ATOMIC_RELAXED);
}

void ss1_vr_cull(float half_angle_rad)
{
    __atomic_store(&s_cull_half, &half_angle_rad, __ATOMIC_RELAXED);
}

/* wide.c: the main view's culling half-angle in VR; 0: the game's own */
float ss1_vr_cull_half(void)
{
    float h;
    __atomic_load(&s_cull_half, &h, __ATOMIC_RELAXED);
    return h;
}

static float rifle_zoom(uint64_t v)
{
    uint32_t zb = (uint32_t)(v >> 32);
    float z;
    memcpy(&z, &zb, sizeof z);
    return z;
}

static int aim_log(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = ch_env("AIM_LOG");
        on = e && *e == '1';
    }
    return on;
}

/* sub_000588C0, after the game's own step (recomp_manual.c) */
void ss1_vr_aim(void)
{
    static uint32_t logged = ~0u;
    const uint64_t v = __atomic_load_n(&s_rifle, __ATOMIC_RELAXED);
    const uint32_t frame = MEM32(FRAME);
    if (v) {
        MEM16(AIM_YAW) = (uint16_t)v;
        MEM16(AIM_PITCH) = (uint16_t)(v >> 16);
    }
    if (aim_log() && frame % 30 == 0 && frame != logged) {
        logged = frame;
        fprintf(stderr, "[ss1 aim] frame %u yaw %d pitch %d rifle %s\n", frame,
                (int16_t)MEM16(AIM_YAW), (int16_t)MEM16(AIM_PITCH), v ? "on" : "off");
    }
}

/* SS1_AIM_LOG: the shot's direction (minus the scope matrix's third row)
 * in the main camera's axes, as yaw and pitch */
static void shot_log(void)
{
    static uint32_t logged = ~0u;
    const uint32_t frame = MEM32(FRAME);
    double m[3][3], d[3], c[3];
    int i, k;
    if (frame % 30 != 0 || frame == logged)
        return;
    logged = frame;
    for (i = 0; i < 3; i++) {
        double n = 0;
        for (k = 0; k < 3; k++) {
            m[i][k] = (int16_t)MEM16(MAIN_CAM + 6 * i + 2 * k);
            n += m[i][k] * m[i][k];
        }
        for (k = 0; k < 3; k++)
            m[i][k] /= n > 0 ? sqrt(n) : 1;
        d[i] = -(double)(int16_t)MEM16(SCOPE_CAM + 12 + 2 * i);
    }
    for (i = 0; i < 3; i++)
        c[i] = d[0] * m[i][0] + d[1] * m[i][1] + d[2] * m[i][2];
    fprintf(stderr, "[ss1 shot] frame %u dir %d %d %d main rows %.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f"
            " -> x %.1f y %.1f z %.1f: yaw %.2f pitch %.2f deg (x right, y up, -z ahead); scope focal %.1f\n",
            frame, (int)d[0], (int)d[1], (int)d[2], m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2],
            m[2][0], m[2][1], m[2][2], c[0], c[1], c[2],
            atan2(c[0], -c[2]) * 57.29577951308232, atan2(c[1], hypot(c[0], c[2])) * 57.29577951308232,
            MEMF(0x1CEA0D4u));
}

/* sub_000744B0: the scope pass */
void ss1_vr_scope_pass(void (*generated)(void))
{
    static int published;
    generated();
    s_scope_frame = MEM32(FRAME);
    if (!published) {
        const uint32_t surf = MEM32(SCOPE_SURF);
        if (surf) {
            /* D3D surface: Common, Data (the bits' physical address), Lock,
             * Format, Size; the renderer names it 0x80000000 up */
            const uint32_t va = 0x80000000u | MEM32(surf + 4);
            vc3_vr_lens_surface_set(va);
            published = 1;
            fprintf(stderr, "[ss1 rifle] scope surface %08X: data %08X format %08X size %08X -> lens %08X\n",
                    surf, MEM32(surf + 4), MEM32(surf + 12), MEM32(surf + 16), va);
        }
    }
    if (aim_log())
        shot_log();
}

/* What sub_00075F40 writes besides drawing, the zoom factor times zoom */
static void circle_writes(float zoom)
{
    float k = MEMF(ONE);
    /* sub_000393F0: the scope type */
    const int16_t ax = (int16_t)MEM16(0x1DB206Eu);
    const int bit = MEM8(0x1DB2068u) & 0x10;
    const int type = ax > 1 ? (bit || ax < 4 ? ax : 0) : bit ? ax + 4 : ax < 4 ? ax : 0;
    if (type == 3) {
        /* sub_00039420: s */
        const int16_t n = (int16_t)MEM16(0x1DB215Cu);
        const double s = n ? (double)n * MEMF(0x156390u) : (double)MEMF(0x156390u);
        k = (float)(MEMF(ONE) / s);
    }
    MEMF(ZOOM_K) = k * zoom;
    MEM32(0x1CEA13Cu) = 1;
}

/* sub_00075F40 (cdecl, four floats): the scope circle */
void ss1_vr_circle(void (*generated)(void))
{
    const uint64_t v = __atomic_load_n(&s_rifle, __ATOMIC_RELAXED);
    if (!v) {
        generated();
        return;
    }
    circle_writes(rifle_zoom(v));
    g_esp += 4;   /* ret: the caller drops the arguments */
}

/* sub_00076490 (no arguments): the circle at the aim and the aim marks */
void ss1_vr_marks(void (*generated)(void))
{
    const uint64_t v = __atomic_load_n(&s_rifle, __ATOMIC_RELAXED);
    if (!v || s_scope_frame != (int64_t)MEM32(FRAME)) {
        generated();
        return;
    }
    if ((int8_t)MEM8(0x1CEBEF0u) >= 0)
        MEMF(0x1CEA0A8u) = MEMF(0x15B588u);
    if (!MEM32(0x1D4AC4Cu) && !MEM32(0x1ED224u))
        MEM32(0x18F6E24u) = 1;
    if (MEM32(0x1CEA0A0u) != 1 && (MEM32(0x1CEA0A4u) || MEM32(0x1CEA0B0u)))
        circle_writes(rifle_zoom(v));
    g_esp += 4;   /* ret */
}

void ss1_vr_rifle_test(void)
{
    const char *e = ch_env("VR_RIFLE_TEST");
    float y, p, z;
    if (!e || sscanf(e, "%f,%f,%f", &y, &p, &z) != 3)
        return;
    const ChVrRifle r = { 1, y * 0.017453292519943295f, p * 0.017453292519943295f, z };
    ss1_vr_rifle(&r);
    fprintf(stderr, "[ss1 rifle] test rifle: yaw %.2f pitch %.2f deg, zoom %.2f\n", y, p, z);
}
