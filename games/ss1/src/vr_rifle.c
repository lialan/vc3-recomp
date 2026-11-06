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
 * The lens at the display's rate (the host reprojects the picture: vr_rifle.h
 * vr_lens_crop). With the rifle on, the scope is drawn guard() times wider
 * than the lens shows (0x1ADFC8 divided by it: the magnification the lens
 * shows stays the player's), so the host can shift its crop by how far the
 * rifle has turned since. Each scope pass's aim (the angle words it drew
 * with), focal length (from 0x1CEA0D4, which sub_000744B0 hands its camera:
 * SCOPE_PROJ) and guard go to the renderer with the picture
 * (ss1_vr_lens_aim, the profile's vr.lens_aim).
 * The renderer runs behind the game -- the push buffer is drawn on threads
 * of its own, often while the game is in its next frame -- so each aim is
 * tagged with a D3D fence inserted right after its pass (0xFC4D0, no kick):
 * the renderer's flip comes after that fence's release in the push buffer
 * and picks the newest aim whose fence the GPU's semaphore has passed.
 * The game's reticle (sub_00035A20: model [0x1DB24F0 + 2 type] 570 units
 * ahead, drawn in the scope pass) would move with the shifted picture: with
 * the rifle on it is not drawn, and the host draws a crosshair at the lens's
 * centre. Left as they are, moving by the small aim delta only: the scope
 * pass's text (sub_000359B0: WAIT, NOW RELOADING) and sub_00035BD0's arrow
 * (model [0x1DB251A] 850 units ahead, turned from the aim toward the target
 * object [0x1DB394C]; not an aim mark).
 *
 * The guard published is the one in the pass's focal length: sub_00074610
 * makes that from 0x1ADFC8 as last written, so on the frame the rifle
 * switches on (the panel closes, the gun is located again) the scope is
 * still drawn without the guard, and on the frame it switches off with it
 * (ss1_vr_focal follows it through).
 *
 * SS1_VR_RIFLE_TEST=yaw_deg,pitch_deg,zoom[,frames]: a rifle held still, set
 * once at boot; with frames, switched off and on every that many game
 * frames (test only). SS1_AIM_LOG=1: the aim and the shot direction relative
 * to the main camera, once a second.
 */
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
#define SCOPE_FOCAL 0x1CEA0D4u      /* the scope camera block's focal length */
#define SCOPE_W     256.0f          /* its target's width (sub_000743D0) */
/* The scope's projection, in its target's pixels, is SCOPE_PROJ times its
 * camera block's focal length (which only the culling reads as pixels):
 * every 3D draw into the target has position rows (c188, c191) with
 * |x row| / |w row| = |y row| / |w row| = 4509.7, 9019.3, 18038.6, 36077.3
 * at zoom 0.5, 1, 2, 4 (guard 1.5) against 98.93, 197.87, 395.73, 791.47
 * there -- 45.583 each time, and through stage 1 (measured on the Mac). At
 * zoom 1 the scope sees 1.1 degrees across: 13529 pixels a radian. */
#define SCOPE_PROJ  45.583f
#define DEVICE      0x105D28u       /* g_pDevice (game.c's d3d block) */
#define DEV_TIME    0x2Cu           /* the next fence's time */
#define DEV_SEM     0x30u           /* -> the GPU's progress: the last fence time released */

/* The scope drawn this many times wider than the lens shows (above): 3, or
 * SS1_VR_LENS_GUARD=g (1..4, the play test's tuning). With it the lens can
 * follow the rifle (g - 1) * 0.54 / zoom degrees ahead of the newest
 * picture: 1.08 at zoom 1. The picture reaches the headset ~110 ms after
 * the game read the aim, so at 1.5 (0.27 degrees) a 9 degree/s swing had
 * the lens at its edge in 78% of the frames on the Quest; at 3, 9% (at
 * the cost of the lens's detail: 170 of the 512 pixels across). */
#define GUARD_DEFAULT 3.0f
static float guard(void)
{
    static float g;
    if (!g) {
        const char *e = ch_env("VR_LENS_GUARD");
        const float v = e ? (float)atof(e) : GUARD_DEFAULT;
        g = v >= 1.0f && v <= 4.0f ? v : GUARD_DEFAULT;
    }
    return g;
}

/* The game's yaw and pitch for the rifle's right and up (measured, above). */
#define YAW_RIGHT   (-1)
#define PITCH_UP    1

/* bits 0-15 yaw, 16-31 pitch (the game's units), 32-63 the zoom (float
 * bits); 0: off. One word, so a frame reads one rifle (as vc3_vr_aim_set). */
static uint64_t s_rifle;
static float s_cull_half;
static int64_t s_scope_frame = -1;  /* the frame the scope pass last ran */
static uint64_t s_frame_rifle;      /* the rifle the game's frame aims with (its aim step's) */
static int s_vr;                    /* a rifle has been handed over: a headset (or the test) */
/* The guard in 0x1ADFC8 as last written (1: the game's own value), and in
 * the scope's focal length 0x1CEA0D4 as sub_00074610 last made it from
 * 0x1ADFC8 -- a frame or so later: on the frames the rifle switches on or
 * off the scope is still drawn with the old one. */
static float s_zoomk_guard = 1.0f, s_focal_guard = 1.0f;
/* the rifle's zoom in ZOOM_K, then in the scope's focal length (0: the game's own) */
static float s_zoomk_zoom, s_focal_zoom;
/* SS1_VR_RIFLE_TEST's rifle, and its on/off period in game frames (0: on) */
static uint64_t s_test_rifle;
static uint32_t s_test_period;

/* The last few scope passes' aims, each with the fence after it (above) */
#define MARKS 8
static struct { uint32_t time; VrLensAim aim; } s_marks[MARKS];
static unsigned s_mark_n;
static pthread_mutex_t s_mark_lock = PTHREAD_MUTEX_INITIALIZER;

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
    if (r)
        __atomic_store_n(&s_vr, 1, __ATOMIC_RELAXED);
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
    const uint32_t frame = MEM32(FRAME);
    if (s_test_period)      /* SS1_VR_RIFLE_TEST's switching rifle */
        __atomic_store_n(&s_rifle, (frame / s_test_period) & 1 ? 0 : s_test_rifle, __ATOMIC_RELAXED);
    const uint64_t v = __atomic_load_n(&s_rifle, __ATOMIC_RELAXED);
    s_frame_rifle = v;
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

/* After a scope pass in VR: a fence (D3D's own fence insert, flags 2: no
 * kick) and the pass's aim, tagged with its time: the guard its focal length
 * was made with, and whether the rifle aimed it (follow; with the rifle off
 * -- the settings panel open, the gun not located: the game's own aim,
 * circle and reticle -- the lens shows its centre). */
extern void sub_000FC4D0(void);
static void lens_mark(float focal, float guard_in_focal, float zoom_in_focal, int rifle)
{
    const uint32_t dev = MEM32(DEVICE);
    if (!dev || !(focal > 0))
        return;
    const uint32_t time = MEM32(dev + DEV_TIME);    /* the time the fence releases */
    PUSH32(g_esp, 2);
    PUSH32(g_esp, 0x00059E0Au);                     /* (the scope pass's own return) */
    sub_000FC4D0();                                 /* ret 4 */
    const float unit = (float)(6.283185307179586 / 65536.0);
    const VrLensAim a = { YAW_RIGHT * (int16_t)MEM16(AIM_YAW) * unit, PITCH_UP * (int16_t)MEM16(AIM_PITCH) * unit,
                          focal * SCOPE_PROJ / SCOPE_W, guard_in_focal, rifle, zoom_in_focal };
    pthread_mutex_lock(&s_mark_lock);
    s_marks[s_mark_n % MARKS].time = time;
    s_marks[s_mark_n % MARKS].aim = a;
    s_mark_n++;
    pthread_mutex_unlock(&s_mark_lock);
}

/* The renderer, at the flip that hands a lens picture over (the profile's
 * vr.lens_aim): the newest marked pass the GPU's progress has passed. */
int ss1_vr_lens_aim(VrLensAim *out)
{
    static unsigned flips, ahead, logged;
    const uint32_t dev = MEM32(DEVICE);
    const uint32_t sem = dev ? MEM32(dev + DEV_SEM) : 0;
    int found = 0, newer = 0;
    if (!sem)
        return 0;
    const uint32_t done = MEM32(sem);
    pthread_mutex_lock(&s_mark_lock);
    for (unsigned k = 1; k <= MARKS && k <= s_mark_n; k++) {
        const unsigned i = (s_mark_n - k) % MARKS;
        if ((int32_t)(done - s_marks[i].time) >= 0) {
            *out = s_marks[i].aim;
            found = 1;
            break;
        }
        newer = 1;
    }
    pthread_mutex_unlock(&s_mark_lock);
    /* SS1_AIM_LOG: how often the game had queued a newer pass already */
    flips++;
    ahead += found && newer;
    if (aim_log() && flips % 300 == 0 && logged++ < 20)
        fprintf(stderr, "[ss1 lens] %u of %u flips: the game had queued a newer scope pass%s; this one's aim "
                "yaw %.3f pitch %.3f deg, focal %.3f, guard %.2f, follow %d\n", ahead, flips,
                found ? "" : " (none passed yet)", found ? out->yaw * 57.29578f : 0.0f,
                found ? out->pitch * 57.29578f : 0.0f, found ? out->focal : 0.0f, found ? out->guard : 0.0f,
                found ? out->follow : 0);
    return found;
}

/* sub_000744B0: the scope pass */
void ss1_vr_scope_pass(void (*generated)(void))
{
    static int published;
    const float focal = MEMF(SCOPE_FOCAL), g = s_focal_guard, z = s_focal_zoom;   /* what it hands its camera */
    generated();
    s_scope_frame = MEM32(FRAME);
    if (s_frame_rifle || __atomic_load_n(&s_vr, __ATOMIC_RELAXED))
        lens_mark(focal, g, z, s_frame_rifle != 0);
    {   /* SS1_AIM_LOG: the passes round a switch of the rifle -- the guard
         * published is the one in the focal length (focal * guard: the
         * game's own focal length at that zoom) */
        static int was = -1, left;
        const int on = s_frame_rifle != 0;
        if (on != was)
            left = 3;
        if (aim_log() && was >= 0 && left)
            fprintf(stderr, "[ss1 lens guard] frame %u rifle %s: focal %.2f, guard in it %.2f (published), focal * guard"
                    " %.2f; by the rifle alone %.2f\n", MEM32(FRAME), on ? "on" : "off", focal, g, focal * g,
                    on ? guard() : 1.0f);
        left -= left > 0;
        was = on;
    }
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

/* What sub_00075F40 writes besides drawing, the zoom factor times zoom
 * over the guard band (the scope's focal length from the next frame on) */
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
    s_zoomk_guard = guard();
    s_zoomk_zoom = zoom;
    MEMF(ZOOM_K) = k * zoom / s_zoomk_guard;
    MEM32(0x1CEA13Cu) = 1;
}

/* sub_00074610 (cdecl): a camera mode's focal length; the scope's (mode 1)
 * from 0x1ADFC8, and with it its guard */
void ss1_vr_focal(void (*generated)(void))
{
    const int scope = MEM32(0x1CEA0E0u) == 1;
    generated();
    if (scope) {
        s_focal_guard = s_zoomk_guard;
        s_focal_zoom = s_zoomk_zoom;
    }
}

/* sub_00075F40 (cdecl, four floats): the scope circle */
void ss1_vr_circle(void (*generated)(void))
{
    const uint64_t v = __atomic_load_n(&s_rifle, __ATOMIC_RELAXED);
    if (!v) {
        generated();        /* 0x1ADFC8 the game's own */
        s_zoomk_guard = 1.0f;
        s_zoomk_zoom = 0.0f;
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

/* sub_00035A20 (cdecl, the reticle's type): the scope's reticle, not drawn
 * while the rifle aims (above) */
void ss1_vr_reticle(void (*generated)(void))
{
    if (!s_frame_rifle) {
        generated();
        return;
    }
    g_esp += 4;   /* ret: the caller drops the argument */
}

void ss1_vr_rifle_test(void)
{
    const char *e = ch_env("VR_RIFLE_TEST");
    float y, p, z;
    unsigned period = 0;
    if (!e || sscanf(e, "%f,%f,%f,%u", &y, &p, &z, &period) < 3)
        return;
    const ChVrRifle r = { 1, y * 0.017453292519943295f, p * 0.017453292519943295f, z };
    ss1_vr_rifle(&r);
    s_test_rifle = __atomic_load_n(&s_rifle, __ATOMIC_RELAXED);
    s_test_period = period;
    fprintf(stderr, "[ss1 rifle] test rifle: yaw %.2f pitch %.2f deg, zoom %.2f", y, p, z);
    if (period)
        fprintf(stderr, ", on and off every %u frames", period);
    fputc('\n', stderr);
}
