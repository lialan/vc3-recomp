/*
 * scope.c - Silent Scope's side of the shared scope game (the runtime's
 * host/scope_game.h: the VR rifle and the 16:9 view): where its aim, zoom
 * and scope pass live, and what its circle and aim marks write besides
 * drawing. recomp_manual.c wraps the functions named here.
 *
 * Aim. sub_000588C0 steps the aim -- pitch 0x1CEB75C, yaw 0x1CEB75E, int16,
 * 65536 a turn -- by the stick and clamps the yaw to +-5500, the 4:3 view's
 * edge; the stick paths only ever add a step or leave the yaw as it is (the
 * light gun's path, absolute, is not reached while no gun is emulated). The
 * scope camera (sub_00059F80) turns the main camera (matrix 0x1CEB084) by
 * them, and a shot flies from it along minus the scope matrix's third row
 * (0x1CEB210, sub_0003EB30). Measured in stage 1 with the stick held
 * (SS1_AIM_LOG=1): right takes the yaw negative (0 to -4800 in 4 s; the
 * circle moves right, the shot 26.4 degrees right of the main camera's
 * ahead), up takes the pitch positive (0 to 3600 in 3 s; the circle moves
 * up, the shot 19.8 degrees up). The scope turns by the yaw, then by the
 * pitch about its turned right axis -- the rifle's own convention -- so the
 * angles go across as they are, the yaw negated. The main loop's frame
 * counter is 0x1CEAF90.
 *
 * Zoom. The scope's focal length (sub_00074610, for the next frame; mode 1
 * at 0x1CEA0E0 is the scope's) is multiplied by 0x1ADFC8, which only
 * sub_00075F40 writes: 1.0, or 1/s for the type-3 scope (sub_000393F0() ==
 * 3; s = sub_00039420()).
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
 * All three replaced functions are cdecl (sub_00075F40 four floats,
 * sub_00076490 none, sub_00035A20 the reticle's type): the caller drops the
 * arguments.
 *
 * The scope pass. sub_000744B0 renders the scope view into a 256x256 target
 * (D3D surface object at [0x1ED214]), with the focal length its camera block
 * hands its camera, 0x1CEA0D4. Its projection in the target's pixels is
 * 45.583 times that focal length (which only the culling reads as pixels):
 * every 3D draw into the target has position rows (c188, c191) with |x row|
 * / |w row| = |y row| / |w row| = 4509.7, 9019.3, 18038.6, 36077.3 at zoom
 * 0.5, 1, 2, 4 (guard 1.5) against 98.93, 197.87, 395.73, 791.47 there --
 * 45.583 each time, and through stage 1 (measured on the Mac). At zoom 1
 * the scope sees 1.1 degrees across: 13529 pixels a radian. The scope's
 * reticle (sub_00035A20: model [0x1DB24F0 + 2 type] 570 units ahead, drawn
 * in the scope pass) would move with the reprojected picture, so it is not
 * drawn while the rifle aims. Left as they are, moving by the small aim
 * delta only: the scope pass's text (sub_000359B0: WAIT, NOW RELOADING) and
 * sub_00035BD0's arrow (model [0x1DB251A] 850 units ahead, turned from the
 * aim toward the target object [0x1DB394C]; not an aim mark).
 *
 * The cull. sub_0006D650 loads the camera block {focal length D, width W,
 * height, half width, half height} at 0x1D1F980 that the bounding-sphere
 * cull (0x6EE40) tests against, and the secant of the horizontal half-angle
 * from W and D (0x1D1F994; (W/2)^2 = W^2 * the game's 0.25 at 0x15A9E0). The
 * main view's W is 640, the scope's 256. Only the cull reads these.
 */
#include <stdint.h>

#include "recomp_types.h"
#include "host/scope_game.h"

#define ONE         0x14D304u       /* the game's 1.0 */

/* sub_00075F40's writes besides drawing; the zoom factor it writes */
static float circle_writes(void)
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
    MEM32(0x1CEA13Cu) = 1;
    return k;
}

/* sub_00076490's writes besides drawing; 1 where it calls sub_00075F40 */
static int marks_writes(void)
{
    if ((int8_t)MEM8(0x1CEBEF0u) >= 0)
        MEMF(0x1CEA0A8u) = MEMF(0x15B588u);
    if (!MEM32(0x1D4AC4Cu) && !MEM32(0x1ED224u))
        MEM32(0x18F6E24u) = 1;
    return MEM32(0x1CEA0A0u) != 1 && (MEM32(0x1CEA0A4u) || MEM32(0x1CEA0B0u));
}

const ChScopeGame ss1_scope = {
    .tag = "ss1",
    .frame = 0x1CEAF90u,
    .aim_pitch = 0x1CEB75Cu, .aim_yaw = 0x1CEB75Eu,
    .yaw_right = -1, .pitch_up = 1,
    .yaw_edge = 5500,
    .zoom_k = 0x1ADFC8u, .cam_mode = 0x1CEA0E0u, .cam_mode_scope = 1,
    .scope_focal = 0x1CEA0D4u, .scope_surf = 0x1ED214u,
    .scope_w = 256.0f, .scope_proj = 45.583f,
    .cam_d = 0x1D1F980u, .cam_w = 0x1D1F984u, .cam_half_w = 0x1D1F98Cu, .cam_sec = 0x1D1F994u,
    .quarter = 0x15A9E0u, .main_w = 640.0f,
    .main_cam = 0x1CEB084u, .scope_cam = 0x1CEB204u,
    .circle_writes = circle_writes,
    .marks_writes = marks_writes,
};
