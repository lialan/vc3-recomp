/*
 * scope.c - Silent Scope 3's side of the shared scope game (the runtime's
 * host/scope_game.h: the VR rifle and the 16:9 view): where its aim, zoom
 * and scope picture live. recomp_manual.c wraps the functions named here.
 * Found in Mac runs of Story stage 1 (2026-11-06); ss3 is built otherwise
 * than ss1 and ss2, which the header's "other build" fields describe.
 *
 * Aim. The active player's record is [0x4D4428] (0x4D3C40 + 0x3F4 i; the
 * first in Story); its aim words are pitch +0x38C, yaw +0x38E (int16, 65536
 * a turn), right taking the yaw negative and up the pitch positive.
 * sub_001065B0 (cdecl: the player's index, a scale; run for both records
 * every frame) makes them from the cursor (0x4AD6F2 x, 0x4AD70E y, clamped
 * to +-32767 by sub_00114530: at stage 1's zoom yaw +-5279, pitch +-4080)
 * scaled by the scope's field of view, plus an aim assist -- an absolute
 * aim, so no 16:9 yaw limit to widen (yaw_edge 0). The rifle's aim
 * replaces them after it. The scope camera is built from them before it
 * runs (sub_00105AE0: the main camera turned by the aim), so the rifle is
 * written first there too (aim_pre), or the picture would be a frame
 * behind. The shot is built from the player's angles and the aim by the
 * same turn: it leaves along the scope's axis. The frame counter is
 * 0x2DC708 (sub_000C7E10).
 *
 * Zoom. No zoom factor: the scope's field of view is a word, the full angle
 * (65536 a turn; 364, 2 degrees, in stage 1), which sub_00105830 sets in
 * views 1-3 (0x42ACBC + 0x17C n, the word at +0x13A; +0x138 the other
 * angle, made from it). sub_00105D20 copies view 2 or 3 -- both angles --
 * into view 4, the render copy 0x42B130, and remakes its projection
 * (sub_00105330) when +0x13A changed; the scope view is drawn as view 4,
 * its focal length at +0x158 (0x42B288: 7334.9 at 364, in the 256-unit
 * texture's pixels; made from +0x138). So the rifle's zoom and guard go
 * into views 2 and 3's angles while sub_00105D20 runs (zoom_fov), the
 * game's own put back after. (sub_000D3230 and a few others fill view 4
 * too, from the main view.)
 *
 * The scope picture. sub_000D1FB0 draws the scope view into the back buffer
 * (sub_000D25F0(4, 1), about 160..480 x 100..380), its reticle and target
 * arrow into it (sub_000D2CB0), then copies it into a 256x256 texture --
 * sub_00149340(0x1D8, 128, 96, 256, 256, ...), the game's screen copy, the
 * call at 0xD2175 (pass_ret 0xD217A) -- draws the main view over it, and
 * the HUD with the scope disc. sub_000D25F0 (cdecl: the view's index, a
 * mode) draws a view; view 4 is the scope's, so from its call to the copy
 * the screen carries a picture for the copy (screen_view): immersive, those
 * draws stay on the game's own surface. The texture's surface object is at
 * [[0x41BF20] + 0x14C] (data 0x01D84000 on the Mac). sub_00149340 scales
 * its 512x448 rectangle to the back buffer by a width the game set for its
 * 1280-wide quincunx buffer (recomp_manual.c corrects it).
 *
 * Not drawing the circle. sub_000C1FB0 (no arguments) draws the scope disc
 * and ring at the aim; besides drawing it sets 0x2DC3DC (a frame-end
 * effect's request, set by several others and cleared at the frame's end).
 * No aim marks. The reticle -- crosshair and red dot inside the scope
 * picture -- is sub_000C4FC0 (cdecl, two arguments), from sub_000D2CB0;
 * recomp_manual.c hides only those calls. The target arrow (sub_000C2D00)
 * stays, as ss1's.
 *
 * The cull. sub_000C13D0 (cdecl: centre, view, radius) tests a bounding
 * sphere against a view's own tan and sec of its half angles (+0x140 and
 * +0x150 across, +0x13C and +0x14C down), no camera block; the main view
 * 0x42AB40 (D 443.438, 512 units across): tan 0.5773, sec 1.1547. Its
 * projection is made from the same values, so the widening is round the
 * call (cull_view).
 *
 * Its matrices are floats: no shot log (main_cam, scope_cam 0).
 */
#include <stdint.h>

#include "recomp_types.h"
#include "host/scope_game.h"

/* sub_000C1FB0's write besides drawing */
static float circle_writes(void)
{
    MEM32(0x2DC3DCu) = 1;
    return 1.0f;            /* no zoom factor (zoom_fov) */
}

const ChScopeGame ss3_scope = {
    .tag = "ss3",
    .frame = 0x2DC708u,
    .aim_base = 0x4D4428u, .aim_pitch = 0x38Cu, .aim_yaw = 0x38Eu,
    .yaw_right = -1, .pitch_up = 1,
    .yaw_edge = 0,                          /* an absolute aim: nothing to widen */
    .scope_focal = 0x42B288u,
    .scope_surf = 0x41BF20u, .scope_surf_off = 0x14Cu,
    .scope_w = 256.0f, .scope_proj = 1.0f,
    .circle_writes = circle_writes,
    /* views 2 and 3's angles (+0x138, +0x13A: the projection's focal
     * length comes from the first, the change test from the second) */
    .zoom_fov = { 0x42AF70u, 0x42AF72u, 0x42B0ECu, 0x42B0EEu },
    .pass_ret = 0x000D217Au,
    .aim_pre = 1,
    .cull_view = 0x42AB40u, .cull_tan = 0x140u, .cull_sec = 0x150u,
    .screen_view = 4,                       /* sub_000D25F0(4, n): view 4 into the back buffer */
};
