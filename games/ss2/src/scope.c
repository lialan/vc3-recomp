/*
 * scope.c - Silent Scope 2's side of the shared scope game (the runtime's
 * host/scope_game.h: the VR rifle and the 16:9 view): where its aim, zoom
 * and scope view live, and what its circle and aim marks write besides
 * drawing. recomp_manual.c wraps the functions named here. Found as ss1's
 * were (games/ss1/src/scope.c), and checked in Mac runs of the shooting
 * range and Story stage 1.
 *
 * Aim. sub_0006D590 steps the aim by the stick and clamps it (yaw +-5500,
 * pitch +-4300). The words live in the active player's record: pitch at
 * [0xB3DA04] + 0x148, yaw + 0x14A (int16, 65536 a turn; the record is
 * 0xB3D6F4 + 0x188 * [0xB3D6E2], and the two players' records swap
 * contents, so always through the pointer). The second record is the AI
 * partner in Story, whose aim the game moves itself. Right takes the yaw
 * negative (0 to -4760 in 4 s), up the pitch positive (0 to 3600 in 3 s),
 * as in ss1. The stick paths only add a step (the light gun's path,
 * absolute, runs only while (int8)[0xB3E473] < 0, as ss1's). The scope
 * camera (sub_0006E120) turns the main camera (matrix 0xB3D1A4) by them into
 * 0xB3D2F4; the shot (sub_00056670) builds its direction from the player's
 * angles turned by the aim words, and matched the scope's in every test
 * (yaw -4760: 26.15 degrees right; pitch 3600: 19.78 up). No recoil or sway
 * moves the player's aim. The main loop's frame counter is 0xB3D034.
 *
 * Zoom. The zoom factor 0xB1AC1C (written by the circle sub_000768E0, and
 * 1.0 by its init) multiplies every focal length the scope's camera mode
 * ([0x2C4E58] == 1) makes in sub_000A4240 (cdecl, six arguments):
 * [0x2C4E64] = 256 / (2 a2) * a4 * the factor. Four a frame: the first ones
 * while the scope view is drawn give its projection in the target's pixels
 * (7382.79 in the shooting range, 13356.71 in Story stage 1 at zoom 1 --
 * per scene and animated, from the scope view's angle word [0xB3D406]); the
 * last (a4 677: 296.767) is the cull's for the next scope pass.
 *
 * The scope view. sub_0009D5F0 only sets it up -- the camera block {D =
 * [0x2C4E64], W = H = 256}, mode 1, the 256x256 target [0x3E8120] bound,
 * [0x201070] = 1 -- and sub_0006D250 then draws the view into it, ending in
 * sub_000A4420 (mode 0, the main target back, [0x201070] = 0). So the
 * projection the lens is published with is the first focal length made
 * while [0x201070] is set (scope_drawing). One scope target (X8R8G8B8,
 * data 0x805C6000 on the Mac).
 *
 * Not drawing the circle. sub_000768E0 (cdecl, four floats: x, y, radius,
 * alpha) draws the circle; sub_00076D90 (no arguments) draws it at the aim
 * and two aim marks (models [0xB3E604 + 2 idx] and [0xB3E600 + 2 idx]).
 * Their writes besides drawing (read from the lifted bodies and their
 * callees, and memory diffs round a call):
 *   sub_00076D90: 0x1F67F0 = 1 (the gun-flash request) on entry, then back
 *                 to 0 below; returns at once when byte [0xB3E46F] & 1;
 *                 0xB1AC28 = [0x19D1E0] (250.0) when (int8)[0xB3E473] >= 0,
 *                 else (float)(int)[0xA919CC] when [0xB195A0] == 5 -- both
 *                 clearing 0x1F67F0; the 2D scale 0x1D8748 / 0x1D874C set
 *                 to 1, 1 for its drawing and left as [0x1D8750] /
 *                 [0x1D8754] (sub_00097B50, sub_00097B70); sub_000768E0's
 *                 when it calls it ([0xB1AC20] != 1 and [0xB1AC24] or
 *                 [0xB1AC30] set).
 *   sub_000768E0: 0xB1AC1C = [0x18B734] (1.0), or 1 / s for the type-3
 *                 scope (sub_0005CF80() == 3; s = sub_0005D040()); the
 *                 render-state dirty flag 0x2C4E78, set by its state
 *                 setters and cleared by its flush -- left as it is here,
 *                 so state pending before it is flushed by the next draw
 *                 instead; game flag 7 cleared and set back (no net change).
 * The rest is drawing (the draw list behind [0xB1B49C], matrix stacks
 * 0x1F7310.., 0x222B08.., 2D and render-state copies, D3D's own state).
 * The reticle (sub_00066AA0, cdecl, the reticle's type: model [0xB3E680 + 2
 * type] 570 units ahead) writes nothing else.
 *
 * The cull. sub_00097C60 (cdecl, a pointer to {D, W, H, half W, half H})
 * loads the camera block at 0xA85220 the bounding-sphere cull
 * (sub_0009A1E0, ss1's 0x6EE40) tests against, its secant at 0xA85240
 * (+0x20; (W/2)^2 = W^2 * the game's 0.25 at 0x1A6340). The main view's W
 * is 640 (D 553.75), the scope's 256.
 */
#include <stdint.h>

#include "recomp_types.h"
#include "host/scope_game.h"

#define ONE         0x18B734u       /* the game's 1.0 */

/* sub_000768E0's writes besides drawing; the zoom factor it writes */
static float circle_writes(void)
{
    float k = MEMF(ONE);
    /* sub_0005CF80: the scope type (game flag n: [0xB14E00] bit n, sub_0007F270) */
    const int16_t ax = (int16_t)MEM16(0xB657AEu);
    const int bit = MEM8(0xB657A8u) & 0x10;
    int type;
    if (ax <= 1 && bit)
        type = ax + 5;
    else if (MEM32(0xB14E00u) & (1u << 12 | 1u << 13))
        type = 5;
    else if (!bit && ax >= 5)
        type = 0;
    else
        type = ax;
    if (type == 3) {
        /* sub_0005D040: s */
        const int16_t n = (int16_t)MEM16(0xB65898u);
        const double s = n ? (double)n * MEMF(0x1965F4u) : (double)MEMF(0x1965F4u);
        k = (float)(MEMF(ONE) / s);
    }
    return k;
}

/* sub_00076D90's writes besides drawing; 1 where it calls sub_000768E0 */
static int marks_writes(void)
{
    MEM32(0x1F67F0u) = 1;
    if (MEM8(0xB3E46Fu) & 1)
        return 0;
    if ((int8_t)MEM8(0xB3E473u) >= 0) {
        MEMF(0xB1AC28u) = MEMF(0x19D1E0u);
        MEM32(0x1F67F0u) = 0;
    } else if (MEM32(0xB195A0u) == 5) {
        MEMF(0xB1AC28u) = (float)(int32_t)MEM32(0xA919CCu);
        MEM32(0x1F67F0u) = 0;
    }
    MEMF(0x1D8748u) = MEMF(0x1D8750u);
    MEMF(0x1D874Cu) = MEMF(0x1D8754u);
    return MEM32(0xB1AC20u) != 1 && (MEM32(0xB1AC24u) || MEM32(0xB1AC30u));
}

const ChScopeGame ss2_scope = {
    .tag = "ss2",
    .frame = 0xB3D034u,
    .aim_base = 0xB3DA04u, .aim_pitch = 0x148u, .aim_yaw = 0x14Au,
    .yaw_right = -1, .pitch_up = 1,
    .yaw_edge = 5500,
    .zoom_k = 0xB1AC1Cu, .cam_mode = 0x2C4E58u, .cam_mode_scope = 1,
    .scope_focal = 0x2C4E64u, .scope_surf = 0x3E8120u,
    .scope_w = 256.0f, .scope_proj = 1.0f,
    .scope_drawing = 0x201070u,
    .cam_d = 0xA85220u, .cam_w = 0xA85224u, .cam_half_w = 0xA8522Cu, .cam_sec = 0xA85240u,
    .quarter = 0x1A6340u, .main_w = 640.0f,
    .main_cam = 0xB3D1A4u, .scope_cam = 0xB3D2F4u,
    .circle_writes = circle_writes,
    .marks_writes = marks_writes,
};
