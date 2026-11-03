/*
 * vr_cull.c - VC3's CPU culling frustum, widened for the immersive view.
 *
 * The game culls its scene on the CPU against a frustum it builds once a
 * frame in sub_000373B0 (cdecl: FOVY, aspect, near, far; called from
 * 0x23BB0 with the camera's FOVY, 4/3, 0.1, 1000), writing the side planes
 * at 0x330388.. that the scene walk (0x38AC0) tests. Only what lies in the
 * game's own 45-degree view is ever drawn, so in VR, turning the head shows
 * the edge of the world. While the immersive view is on (the host sets
 * vc3_vr_cull_half_angle), the builder is called with a wider FOVY; the
 * camera, its projection and everything else stay the game's. The argument
 * is restored afterwards: the caller's stack is left as it was.
 */
#include <math.h>
#include <stdint.h>
#include "recomp_types.h"

/* Half the vertical angle (radians) to cull with; 0: the game's own. */
float vc3_vr_cull_half_angle;

void vc3_cull_frustum_build(void (*generated)(void))
{
    const float target = vc3_vr_cull_half_angle;
    const uint32_t arg = g_esp + 4;            /* FOVY */
    const uint32_t saved = MEM32(arg);
    const float scale = MEMF(0x1406F8);        /* the builder's FOVY to half-angle factor */
    const float fovy = MEMF(arg);
    int widened = 0;
    if (target > 0.0f && target < 1.55f && isfinite(fovy) && isfinite(scale) && scale > 0.0f
        && fovy * scale > 0.0f && fovy * scale < target) {
        MEMF(arg) = target / scale;
        widened = 1;
    }
    generated();
    if (widened)
        MEM32(arg) = saved;
}
