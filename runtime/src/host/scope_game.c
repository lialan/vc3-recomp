/*
 * scope_game.c - a game aimed through a sniper scope: its VR rifle and its
 * 16:9 view (scope_game.h). The game's addresses come from its profile's
 * scope (games/<id>/src); what they mean, and how they were found, is
 * written beside them there.
 *
 * Aim. The game's aim step turns the aim words by the stick and clamps
 * them; after it, the rifle's angles replace both, unclamped, with the
 * game's signs. The scope camera turns the main camera by them and a shot
 * flies along the scope's forward axis, so the scope sees -- and the shot
 * goes -- where the rifle points.
 *
 * Zoom. The scope's focal length is made (the game's focal length function,
 * for the next frame) from a zoom factor only the circle writes. With the
 * rifle on, the circle is replaced: its other writes are kept
 * (ChScopeGame.circle_writes) and the factor is written times the rifle's
 * zoom over the guard band.
 *
 * The lens at the display's rate (the host reprojects the picture: vr_rifle.h
 * vr_lens_crop). With the rifle on, the scope is drawn guard() times wider
 * than the lens shows (the zoom factor divided by it: the magnification the
 * lens shows stays the player's), so the host can shift its crop by how far
 * the rifle has turned since. Each scope pass's aim (the angle words it drew
 * with), focal length (its camera block's, times the game's projection; or,
 * for a pass that only sets its target up, the projection the view is then
 * drawn with: ChScopeGame.scope_drawing) and guard go to the renderer with
 * the picture (ch_scope_lens_aim, the profile's vr.lens_aim). The renderer
 * runs behind the game -- the push
 * buffer is drawn on threads of its own, often while the game is in its
 * next frame -- so each aim is tagged with a D3D fence inserted right after
 * its pass (no kick): the renderer's flip comes after that fence's release
 * in the push buffer and picks the newest aim whose fence the GPU has
 * passed. The game's reticle would move with the shifted picture: with the
 * rifle on it is not drawn, and the host draws a crosshair at the lens's
 * centre.
 *
 * The guard published is the one in the pass's focal length: the focal
 * length function makes that from the zoom factor as last written, so on
 * the frame the rifle switches on (the panel closes, the gun is located
 * again) the scope is still drawn without the guard, and on the frame it
 * switches off with it (ch_scope_focal follows it through).
 *
 * 16:9. The renderer draws the game's 4:3 picture unchanged in the middle of
 * a 16:9 one (640 of 854 columns) and lets the 3D view carry on into the
 * sides (vk_wide.c). Two of the game's limits widen with it while the
 * display is set to 16:9 (display_settings.c):
 *   Culling. The camera block the bounding-sphere cull tests against: for
 *   the main view (its W; the scope's view is narrower) W and the half
 *   width grow by 854/640 and the secant follows, so objects at the new
 *   sides are drawn instead of culled. In VR the host sets a half-angle for
 *   the headset's view (ch_scope_vr_cull); then W is 2 D tan(half) instead,
 *   never narrower than the game's, and 16:9 does not apply.
 *   Aim. The aim step clamps the yaw to the 4:3 view's edge. It runs from a
 *   yaw of 0, where the clamp cannot be reached, and the step is added back
 *   here with the 16:9 view's edge as the limit (the game's stick paths only
 *   ever add a step or leave the yaw as it is).
 */
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "chihiro/ch_env.h"
#include "chihiro/chihiro_profile.h"
#include "host/scope_game.h"
#include "host/vr_view.h"

extern int vc3_wide_get(void);                  /* host/display_settings.c */
extern uint32_t chihiro_d3d_fence(void);        /* chihiro/d3d_fence.c */
extern uint32_t chihiro_d3d_progress(void);

/* The renderer's 16:9 width for a 480-line picture (vk_wide.c wide_size). */
#define WIDE_K (854.0f / 640.0f)

static const ChScopeGame *game(void)
{
    return chihiro_profile()->scope;
}

/* The scope drawn this many times wider than the lens shows (above): 3, or
 * <PREFIX>_VR_LENS_GUARD=g (1..8, the play test's tuning). With it the lens
 * can follow the rifle (g - 1) * half the lens's field ahead of the newest
 * picture. The picture reaches the headset ~110 ms after the game read the
 * aim, so at 1.5 a 9 degree/s swing had the lens at its edge in 78% of the
 * frames on the Quest; at 3, 9% (at the cost of the lens's detail). The
 * host's own (ChVrRifle.guard, 1..8) comes first: its 2D scope shows 3 times
 * the lens's field, so it has the scope drawn 6 times wider. */
#define GUARD_DEFAULT 3.0f
static float s_host_guard;      /* the host's guard; 0: none */
static float guard(void)
{
    static float g;
    float h;
    __atomic_load(&s_host_guard, &h, __ATOMIC_RELAXED);
    if (h >= 1.0f && h <= 8.0f)
        return h;
    if (!g) {
        const char *e = ch_env("VR_LENS_GUARD");
        const float v = e ? (float)atof(e) : GUARD_DEFAULT;
        g = v >= 1.0f && v <= 8.0f ? v : GUARD_DEFAULT;
    }
    return g;
}

/* bits 0-15 yaw, 16-31 pitch (the game's units), 32-63 the zoom (float
 * bits); 0: off. One word, so a frame reads one rifle (as vc3_vr_aim_set). */
static uint64_t s_rifle;
static float s_cull_half;
static int64_t s_scope_frame = -1;  /* the frame the scope pass last ran */
static int64_t s_pre_frame = -1;    /* the frame aim_pre last wrote the rifle (s_frame_rifle) */
static void screen_view_end(void);  /* screen_view: the copy closes its window (below) */
static uint64_t s_frame_rifle;      /* the rifle the game's frame aims with (its aim step's) */
static int s_vr;                    /* a rifle has been handed over: a headset (or the test) */
/* The guard in the zoom factor as last written (1: the game's own value),
 * and in the scope's focal length as the focal length function last made
 * it from the factor -- a frame or so later: on the frames the rifle
 * switches on or off the scope is still drawn with the old one. */
static float s_zoomk_guard = 1.0f, s_focal_guard = 1.0f;
/* the rifle's zoom in the zoom factor, then in the scope's focal length (0: the game's own) */
static float s_zoomk_zoom, s_focal_zoom;
/* <PREFIX>_VR_RIFLE_TEST's rifle, and its on/off period in game frames (0: on) */
static uint64_t s_test_rifle;
static uint32_t s_test_period, s_test_start;

/* The last few scope passes' aims, each with the fence after it (above) */
#define MARKS 8
static struct { uint32_t time; VrLensAim aim; } s_marks[MARKS];
static unsigned s_mark_n;
static pthread_mutex_t s_mark_lock = PTHREAD_MUTEX_INITIALIZER;

/* The aim words' addresses (with aim_base, in the record it points to);
 * 0: no record yet */
static int aim_words(const ChScopeGame *g, uint32_t *pitch, uint32_t *yaw)
{
    uint32_t base = 0;
    if (g->aim_base && !(base = MEM32(g->aim_base)))
        return 0;
    *pitch = base + g->aim_pitch;
    *yaw = base + g->aim_yaw;
    return 1;
}

static int16_t game_angle(float rad)
{
    double u = isfinite(rad) ? rad * 65536.0 / 6.283185307179586 : 0.0;
    return (int16_t)lround(u < -32768.0 ? -32768.0 : u > 32767.0 ? 32767.0 : u);
}

void ch_scope_vr_rifle(const ChVrRifle *r)
{
    const ChScopeGame *g = game();
    uint64_t v = 0;
    if (!g)
        return;
    if (r && r->on) {
        float z = isfinite(r->zoom) ? r->zoom : 1.0f;
        uint32_t zb;
        z = z < 0.5f ? 0.5f : z > 4.0f ? 4.0f : z;
        memcpy(&zb, &z, sizeof zb);
        v = (uint64_t)(uint16_t)game_angle(g->yaw_right * r->yaw)
            | (uint64_t)(uint16_t)game_angle(g->pitch_up * r->pitch) << 16
            | (uint64_t)zb << 32;
        float gd = isfinite(r->guard) && r->guard >= 1.0f && r->guard <= 8.0f ? r->guard : 0.0f;
        __atomic_store(&s_host_guard, &gd, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&s_rifle, v, __ATOMIC_RELAXED);
    if (r)
        __atomic_store_n(&s_vr, 1, __ATOMIC_RELAXED);
}

void ch_scope_vr_cull(float half_angle_rad)
{
    __atomic_store(&s_cull_half, &half_angle_rad, __ATOMIC_RELAXED);
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

/* --- 16:9 and the VR cull ------------------------------------------------ */

void ch_scope_camera(void (*gen)(void))
{
    const ChScopeGame *g = game();
    gen();
    if (!g || !g->cam_w)
        return;
    float half;
    __atomic_load(&s_cull_half, &half, __ATOMIC_RELAXED);
    const int vr = half > 0.0f && half < 1.55f;
    if ((!vr && !vc3_wide_get()) || MEMF(g->cam_w) != g->main_w)
        return;
    const float d = MEMF(g->cam_d);
    float k = WIDE_K, w = MEMF(g->cam_w) * WIDE_K;
    if (!(d > 0.0f) || !isfinite(d))
        return;
    if (vr) {
        w = 2.0f * d * tanf(half);
        k = w / MEMF(g->cam_w);
        if (!(k > 1.0f) || !isfinite(k))
            return;
    }
    MEMF(g->cam_w) = w;
    MEMF(g->cam_half_w) = MEMF(g->cam_half_w) * k;
    MEMF(g->cam_sec) = sqrtf(w * w * MEMF(g->quarter) + d * d) / d;
}

static void wide_aim(const ChScopeGame *g, void (*gen)(void))
{
    static int16_t edge;
    uint32_t pa, ya;
    if (!vc3_wide_get() || !g->yaw_edge || !aim_words(g, &pa, &ya)) {
        gen();
        return;
    }
    if (!edge) {   /* the 4:3 edge's angle, its tangent widened as the picture is */
        const double turn = 6.283185307179586 / 65536.0;
        edge = (int16_t)(atan(tan(g->yaw_edge * turn) * WIDE_K) / turn);
    }
    const int32_t yaw = (int16_t)MEM16(ya);
    MEM16(ya) = 0;
    gen();
    int32_t step = (int16_t)MEM16(ya), y = yaw + step;
    if (y > edge) y = edge;
    if (y < -edge) y = -edge;
    MEM16(ya) = (uint16_t)(int16_t)y;
}

/* --- the rifle ------------------------------------------------------------ */

void ch_scope_aim_step(void (*gen)(void))
{
    static uint32_t logged = ~0u;
    const ChScopeGame *g = game();
    if (!g) {
        gen();
        return;
    }
    wide_aim(g, gen);
    const uint32_t frame = MEM32(g->frame);
    if (s_test_period || s_test_start)  /* the test's switching or late rifle */
        __atomic_store_n(&s_rifle, frame < s_test_start || (s_test_period && (frame / s_test_period) & 1)
                         ? 0 : s_test_rifle, __ATOMIC_RELAXED);
    /* the rifle the scope camera was built with this frame (aim_pre), else
     * the newest */
    const uint64_t v = s_pre_frame == (int64_t)frame ? s_frame_rifle : __atomic_load_n(&s_rifle, __ATOMIC_RELAXED);
    uint32_t pa, ya;
    s_frame_rifle = v;
    if (!aim_words(g, &pa, &ya))
        return;
    if (v) {
        MEM16(ya) = (uint16_t)v;
        MEM16(pa) = (uint16_t)(v >> 16);
    }
    if (aim_log() && frame % 30 == 0 && frame != logged) {
        logged = frame;
        fprintf(stderr, "[%s aim] frame %u yaw %d pitch %d rifle %s\n", g->tag, frame,
                (int16_t)MEM16(ya), (int16_t)MEM16(pa), v ? "on" : "off");
    }
}

/* <PREFIX>_AIM_LOG: the shot's direction (minus the scope matrix's third
 * row) in the main camera's axes, as yaw and pitch */
static void shot_log(const ChScopeGame *g)
{
    static uint32_t logged = ~0u;
    const uint32_t frame = MEM32(g->frame);
    double m[3][3], d[3], c[3];
    int i, k;
    if (!g->main_cam || !g->scope_cam || frame % 30 != 0 || frame == logged)
        return;
    logged = frame;
    for (i = 0; i < 3; i++) {
        double n = 0;
        for (k = 0; k < 3; k++) {
            m[i][k] = (int16_t)MEM16(g->main_cam + 6 * i + 2 * k);
            n += m[i][k] * m[i][k];
        }
        for (k = 0; k < 3; k++)
            m[i][k] /= n > 0 ? sqrt(n) : 1;
        d[i] = -(double)(int16_t)MEM16(g->scope_cam + 12 + 2 * i);
    }
    for (i = 0; i < 3; i++)
        c[i] = d[0] * m[i][0] + d[1] * m[i][1] + d[2] * m[i][2];
    fprintf(stderr, "[%s shot] frame %u dir %d %d %d main rows %.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f"
            " -> x %.1f y %.1f z %.1f: yaw %.2f pitch %.2f deg (x right, y up, -z ahead); scope focal %.1f\n",
            g->tag, frame, (int)d[0], (int)d[1], (int)d[2], m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2],
            m[2][0], m[2][1], m[2][2], c[0], c[1], c[2],
            atan2(c[0], -c[2]) * 57.29577951308232, atan2(c[1], hypot(c[0], c[2])) * 57.29577951308232,
            MEMF(g->scope_focal));
}

/* After a scope pass in VR: a fence and the pass's aim, tagged with its
 * time: the guard its focal length was made with, and whether the rifle
 * aimed it (follow; with the rifle off -- the settings panel open, the gun
 * not located: the game's own aim, circle and reticle -- the lens shows its
 * centre). */
static void lens_mark(const ChScopeGame *g, float focal, float guard_in_focal, float zoom_in_focal, int rifle)
{
    uint32_t pa, ya;
    if (!(focal > 0) || !aim_words(g, &pa, &ya))
        return;
    const uint32_t time = chihiro_d3d_fence();
    if (!time)
        return;
    const float unit = (float)(6.283185307179586 / 65536.0);
    const VrLensAim a = { g->yaw_right * (int16_t)MEM16(ya) * unit,
                          g->pitch_up * (int16_t)MEM16(pa) * unit,
                          focal * g->scope_proj / g->scope_w, guard_in_focal, rifle, zoom_in_focal };
    pthread_mutex_lock(&s_mark_lock);
    s_marks[s_mark_n % MARKS].time = time;
    s_marks[s_mark_n % MARKS].aim = a;
    s_mark_n++;
    pthread_mutex_unlock(&s_mark_lock);
}

/* The renderer, at the flip that hands a lens picture over (the profile's
 * vr.lens_aim): the newest marked pass the GPU's progress has passed. */
int ch_scope_lens_aim(VrLensAim *out)
{
    static unsigned flips, ahead, logged;
    const ChScopeGame *g = game();
    const uint32_t done = chihiro_d3d_progress();
    int found = 0, newer = 0;
    if (!g || !done)
        return 0;
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
    /* <PREFIX>_AIM_LOG: how often the game had queued a newer pass already */
    flips++;
    ahead += found && newer;
    if (aim_log() && flips % 300 == 0 && logged++ < 20)
        fprintf(stderr, "[%s lens] %u of %u flips: the game had queued a newer scope pass%s; this one's aim "
                "yaw %.3f pitch %.3f deg, focal %.3f, guard %.2f, follow %d\n", g->tag, ahead, flips,
                found ? "" : " (none passed yet)", found ? out->yaw * 57.29578f : 0.0f,
                found ? out->pitch * 57.29578f : 0.0f, found ? out->focal : 0.0f, found ? out->guard : 0.0f,
                found ? out->follow : 0);
    return found;
}

static int s_pass_open;     /* scope_drawing: a pass whose view's projection is still to come */

/* A scope view drawn with focal (scope_proj units) and the guard and zoom
 * in it: its lens mark */
static void pass_drawn(const ChScopeGame *g, float focal, float gd, float z)
{
    if (s_frame_rifle || __atomic_load_n(&s_vr, __ATOMIC_RELAXED))
        lens_mark(g, focal, gd, z, s_frame_rifle != 0);
    {   /* <PREFIX>_AIM_LOG: the passes round a switch of the rifle -- the
         * guard published is the one in the focal length (focal * guard:
         * the game's own focal length at that zoom) */
        static int was = -1, left;
        const int on = s_frame_rifle != 0;
        if (on != was)
            left = 3;
        if (aim_log() && was >= 0 && left)
            fprintf(stderr, "[%s lens guard] frame %u rifle %s: focal %.2f, guard in it %.2f (published), focal * guard"
                    " %.2f; by the rifle alone %.2f\n", g->tag, MEM32(g->frame), on ? "on" : "off", focal, gd,
                    focal * gd, on ? guard() : 1.0f);
        left -= left > 0;
        was = on;
    }
}

void ch_scope_pass(void (*gen)(void))
{
    static int published;
    const ChScopeGame *g = game();
    if (!g || (g->pass_ret && MEM32(g_esp) != g->pass_ret)) {
        gen();              /* (pass_ret) a copy of something else */
        return;
    }
    const float focal = MEMF(g->scope_focal), gd = s_focal_guard, z = s_focal_zoom;   /* what it hands its camera */
    gen();
    if (g->screen_view)
        screen_view_end();  /* the view drawn into the screen is copied: the screen's again */
    s_scope_frame = MEM32(g->frame);
    if (g->scope_drawing)
        s_pass_open = 1;    /* the view and its projection come after (ch_scope_focal) */
    else
        pass_drawn(g, focal, gd, z);
    if (!published) {
        const uint32_t holder = g->scope_surf_off ? MEM32(g->scope_surf) : 0;
        const uint32_t surf = !g->scope_surf_off ? MEM32(g->scope_surf)
                              : holder ? MEM32(holder + g->scope_surf_off) : 0;
        if (surf) {
            /* D3D surface: Common, Data (the bits' physical address), Lock,
             * Format, Size; the renderer names it 0x80000000 up */
            const uint32_t va = 0x80000000u | MEM32(surf + 4);
            vc3_vr_lens_surface_set(va);
            published = 1;
            fprintf(stderr, "[%s rifle] scope surface %08X: data %08X format %08X size %08X -> lens %08X\n",
                    g->tag, surf, MEM32(surf + 4), MEM32(surf + 12), MEM32(surf + 16), va);
        }
    }
    if (aim_log())
        shot_log(g);
}

/* What the circle writes besides drawing (the game's), and the zoom factor
 * times the zoom over the guard band (the scope's focal length from the
 * next frame on) */
static void circle_writes(const ChScopeGame *g, float zoom)
{
    const float k = g->circle_writes ? g->circle_writes() : 1.0f;
    if (!g->zoom_k)
        return;             /* zoomed by its field of view (ch_scope_fov) */
    s_zoomk_guard = guard();
    s_zoomk_zoom = zoom;
    MEMF(g->zoom_k) = k * zoom / s_zoomk_guard;
}

void ch_scope_focal(void (*gen)(void))
{
    const ChScopeGame *g = game();
    const int scope = g && MEM32(g->cam_mode) == g->cam_mode_scope;
    gen();
    if (!scope)
        return;
    s_focal_guard = s_zoomk_guard;
    s_focal_zoom = s_zoomk_zoom;
    if (g->scope_drawing && s_pass_open && MEM32(g->scope_drawing)) {
        s_pass_open = 0;    /* the first while the view is drawn: its projection */
        pass_drawn(g, MEMF(g->scope_focal), s_focal_guard, s_focal_zoom);
    }
}

void ch_scope_circle(void (*gen)(void))
{
    const ChScopeGame *g = game();
    const uint64_t v = __atomic_load_n(&s_rifle, __ATOMIC_RELAXED);
    if (!g || !v) {
        gen();              /* the zoom factor the game's own */
        if (g && g->zoom_k) {
            s_zoomk_guard = 1.0f;
            s_zoomk_zoom = 0.0f;
        }
        return;
    }
    circle_writes(g, rifle_zoom(v));
    g_esp += 4 + g->circle_ret;   /* ret */
}

void ch_scope_marks(void (*gen)(void))
{
    const ChScopeGame *g = game();
    const uint64_t v = __atomic_load_n(&s_rifle, __ATOMIC_RELAXED);
    if (!g || !v || s_scope_frame != (int64_t)MEM32(g->frame)) {
        gen();
        return;
    }
    if (g->marks_writes && g->marks_writes())
        circle_writes(g, rifle_zoom(v));
    g_esp += 4 + g->marks_ret;    /* ret */
}

void ch_scope_reticle(void (*gen)(void))
{
    const ChScopeGame *g = game();
    if (!g || !s_frame_rifle) {
        gen();
        return;
    }
    g_esp += 4 + g->reticle_ret;  /* ret */
}

/* --- the other build (scope_game.h: zoom_fov, aim_pre, cull_view) ------- */

void ch_scope_fov(void (*gen)(void))
{
    const ChScopeGame *g = game();
    const uint64_t v = __atomic_load_n(&s_rifle, __ATOMIC_RELAXED);
    int16_t keep[4] = { 0, 0, 0, 0 };
    int i;
    if (!g || !v) {
        gen();              /* the game's own field of view */
        s_focal_guard = 1.0f;
        s_focal_zoom = 0.0f;
        return;
    }
    const float gd = guard(), z = rifle_zoom(v);
    const double turn = 6.283185307179586 / 65536.0;
    for (i = 0; i < 4; i++) {
        if (!g->zoom_fov[i])
            continue;
        keep[i] = (int16_t)MEM16(g->zoom_fov[i]);
        if (keep[i] <= 0)
            continue;
        /* the full angle whose half's tangent is the game's times guard / zoom */
        double a = 2.0 * atan(tan(keep[i] * turn * 0.5) * gd / z) / turn;
        a = a < 1.0 ? 1.0 : a > 32767.0 ? 32767.0 : a;
        MEM16(g->zoom_fov[i]) = (uint16_t)(int16_t)lround(a);
    }
    gen();
    for (i = 0; i < 4; i++)
        if (g->zoom_fov[i] && keep[i] > 0)
            MEM16(g->zoom_fov[i]) = (uint16_t)keep[i];
    s_focal_guard = gd;     /* in the projection the scope is drawn with from now */
    s_focal_zoom = z;
}

void ch_scope_aim_pre(void (*gen)(void))
{
    const ChScopeGame *g = game();
    uint32_t pa, ya;
    if (g && g->aim_pre) {
        const uint64_t v = __atomic_load_n(&s_rifle, __ATOMIC_RELAXED);
        s_frame_rifle = v;
        s_pre_frame = MEM32(g->frame);
        if (v && aim_words(g, &pa, &ya)) {
            MEM16(ya) = (uint16_t)v;
            MEM16(pa) = (uint16_t)(v >> 16);
        }
    }
    gen();
}

void ch_scope_cull(void (*gen)(void))
{
    const ChScopeGame *g = game();
    float half;
    if (!g || !g->cull_view || MEM32(g_esp + 8) != g->cull_view) {
        gen();
        return;
    }
    __atomic_load(&s_cull_half, &half, __ATOMIC_RELAXED);
    const int vr = half > 0.0f && half < 1.55f;
    const uint32_t ta = g->cull_view + g->cull_tan, sa = g->cull_view + g->cull_sec;
    const float t0 = MEMF(ta), s0 = MEMF(sa);
    float t = vr ? tanf(half) : vc3_wide_get() ? t0 * WIDE_K : t0;
    if (!(t > t0) || !isfinite(t)) {
        gen();
        return;
    }
    MEMF(ta) = t;
    MEMF(sa) = sqrtf(1.0f + t * t);
    gen();
    MEMF(ta) = t0;
    MEMF(sa) = s0;
}

/* screen_view: the windows of the push buffer the scope view is drawn in,
 * as D3D fence times -- begin, the fence before its draws; end, the one
 * after the copy (0 while open). Each draw carries the last fence release
 * the decode passed before it (the toolkit's Nv2aRenderState.release), so
 * the renderer can tell a draw inside a window -- not the semaphore, written
 * only once the draws before it are submitted (the view's backdrop, first,
 * went to the HUD), nor the decode's own position, ahead of a deferred
 * renderer's draws. */
#define WINDOWS 8
static struct { uint32_t begin, end; } s_win[WINDOWS];
static unsigned s_win_n;
static int s_win_open;
static pthread_mutex_t s_win_lock = PTHREAD_MUTEX_INITIALIZER;

void ch_scope_screen_view(void (*gen)(void))
{
    const ChScopeGame *g = game();
    if (g && g->screen_view && MEM32(g_esp + 4) == g->screen_view) {
        const uint32_t t = chihiro_d3d_fence();
        if (t) {
            pthread_mutex_lock(&s_win_lock);
            if (s_win_open)     /* a view never copied: its window ends here */
                s_win[(s_win_n - 1) % WINDOWS].end = t;
            s_win[s_win_n % WINDOWS].begin = t;
            s_win[s_win_n % WINDOWS].end = 0;
            s_win_n++;
            s_win_open = 1;
            pthread_mutex_unlock(&s_win_lock);
        }
    }
    gen();
}

/* the copy that closes the open window (ch_scope_pass) */
static void screen_view_end(void)
{
    if (!s_win_open)
        return;
    const uint32_t t = chihiro_d3d_fence();
    pthread_mutex_lock(&s_win_lock);
    s_win[(s_win_n - 1) % WINDOWS].end = t ? t : 1;
    s_win_open = 0;
    pthread_mutex_unlock(&s_win_lock);
}

int ch_scope_flat_now(uint32_t release)
{
    const uint32_t p = release;
    int in = 0;
    if (!p)
        return 0;
    pthread_mutex_lock(&s_win_lock);
    for (unsigned k = 1; k <= WINDOWS && k <= s_win_n && !in; k++) {
        const unsigned i = (s_win_n - k) % WINDOWS;
        const uint32_t b = s_win[i].begin, e = s_win[i].end;
        in = (int32_t)(p - b) >= 0 && (!e || (int32_t)(p - e) < 0);
    }
    pthread_mutex_unlock(&s_win_lock);
    return in;
}

void ch_scope_boot(void)
{
    const ChScopeGame *g = game();
    const char *e = ch_env("VR_RIFLE_TEST");
    float y, p, z;
    unsigned period = 0, start = 0;
    if (!g || !e || sscanf(e, "%f,%f,%f,%u,%u", &y, &p, &z, &period, &start) < 3)
        return;
    const ChVrRifle r = { 1, y * 0.017453292519943295f, p * 0.017453292519943295f, z };
    ch_scope_vr_rifle(&r);
    s_test_rifle = __atomic_load_n(&s_rifle, __ATOMIC_RELAXED);
    s_test_period = period;
    s_test_start = start;
    if (start)                  /* off until then: menus the aim steers pick as scripted */
        __atomic_store_n(&s_rifle, 0, __ATOMIC_RELAXED);
    fprintf(stderr, "[%s rifle] test rifle: yaw %.2f pitch %.2f deg, zoom %.2f", g->tag, y, p, z);
    if (period)
        fprintf(stderr, ", on and off every %u frames", period);
    if (start)
        fprintf(stderr, ", from game frame %u", start);
    fputc('\n', stderr);
}
