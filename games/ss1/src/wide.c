/*
 * wide.c - Silent Scope at 16:9: the game's side of the shared renderer's
 * 16:9 view (runtime/src/gpu/vulkan/vk_wide.h).
 *
 * The renderer draws the game's 4:3 picture unchanged in the middle of a
 * 16:9 one (640 of 854 columns) and lets the 3D view carry on into the
 * sides. What the game puts there is its own business, so two of its limits
 * widen with it while the display is set to 16:9 (display_settings.c):
 *
 *   Culling. sub_0006D650 loads the camera block {focal length D, width W,
 *   height, half width, half height} the bounding-sphere cull (0x6EE40)
 *   tests against, and the secant of the horizontal half-angle from W and D.
 *   For the main view (W 640; the scope's view is 256) W and the half width
 *   grow by 854/640 and the secant follows, so objects at the new sides are
 *   drawn instead of culled. Only the cull reads these.
 *
 *   Aim. sub_000588C0 steps the aim's yaw (0x1CEB75E, 65536 a turn; pitch
 *   0x1CEB75C) by the stick and clamps it to +-5500, the 4:3 view's edge.
 *   The step runs from a yaw of 0, where the clamp cannot be reached, and is
 *   added back here with the 16:9 view's edge as the limit. The stick paths
 *   only ever add a step or leave the yaw as it is; the light gun's path
 *   (absolute) is not reached while no gun is emulated.
 */
#include <math.h>
#include <stdint.h>
#include "recomp_types.h"

extern int vc3_wide_get(void);   /* host/display_settings.c */

/* The renderer's 16:9 width for a 480-line picture (vk_wide.c wide_size). */
#define WIDE_K (854.0f / 640.0f)

#define CAM_D      0x1D1F980u
#define CAM_W      0x1D1F984u
#define CAM_HALF_W 0x1D1F98Cu
#define CAM_SEC    0x1D1F994u
#define CAM_QUARTER 0x15A9E0u   /* the game's own 0.25: (W/2)^2 = W^2 * it */

void ss1_wide_camera(void (*generated)(void))
{
    generated();
    if (!vc3_wide_get() || MEMF(CAM_W) != 640.0f)
        return;
    const float d = MEMF(CAM_D), w = MEMF(CAM_W) * WIDE_K;
    if (!(d > 0.0f) || !isfinite(d))
        return;
    MEMF(CAM_W) = w;
    MEMF(CAM_HALF_W) = MEMF(CAM_HALF_W) * WIDE_K;
    MEMF(CAM_SEC) = sqrtf(w * w * MEMF(CAM_QUARTER) + d * d) / d;
}

#define AIM_YAW 0x1CEB75Eu
#define YAW_EDGE_4_3 5500.0f   /* the game's own clamp */

void ss1_wide_aim(void (*generated)(void))
{
    static int16_t edge;
    if (!vc3_wide_get()) {
        generated();
        return;
    }
    if (!edge) {   /* the 4:3 edge's angle, its tangent widened as the picture is */
        const double turn = 6.283185307179586 / 65536.0;
        edge = (int16_t)(atan(tan(YAW_EDGE_4_3 * turn) * WIDE_K) / turn);
    }
    const int32_t yaw = (int16_t)MEM16(AIM_YAW);
    MEM16(AIM_YAW) = 0;
    generated();
    int32_t step = (int16_t)MEM16(AIM_YAW), y = yaw + step;
    if (y > edge) y = edge;
    if (y < -edge) y = -edge;
    MEM16(AIM_YAW) = (uint16_t)(int16_t)y;
}
