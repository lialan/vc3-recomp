/*
 * vr_cull.c - VC3's CPU culling frustum, widened for the immersive view.
 *
 * The game culls its scene on the CPU against a frustum it builds once a
 * frame in sub_000373B0 (cdecl: FOVY, aspect, near, far; called from
 * 0x23BB0 with the camera's FOVY, 4/3, 0.1, 1000), writing the side planes
 * at 0x330388.. that the scene walk (0x38AC0) tests. Only what lies in the
 * game's own 45-degree view is ever drawn, so in VR, turning the head shows
 * the edge of the world. While the immersive view is on (the host sets
 * vc3_vr_cull_half_angle), the builder runs twice -- as called, then with a
 * wider FOVY -- and only the side planes of the wide one are kept; camera,
 * projection and everything else the builder writes stay the game's. The
 * argument is restored: the caller's stack is left as it was.
 */
#include <math.h>
#include <stdint.h>
#include "recomp_types.h"

/* Half the vertical angle (radians) to cull with; 0: the game's own. */
float vc3_vr_cull_half_angle;

/* What the builder writes: 0x330370..0x33037F (its half-extents, which
 * other code reads -- widening them made the game drop heads, arms and
 * screens), near/far at 0x330380/84, the side planes at 0x330388..0x3303C7.
 * Only the planes are taken from the wide build. */
#define FRUSTUM_LO 0x330360u
#define FRUSTUM_HI 0x330400u
#define PLANES_LO 0x330388u
#define PLANES_HI 0x3303C8u

void vc3_cull_frustum_build(void (*generated)(void))
{
    const float target = vc3_vr_cull_half_angle;
    const uint32_t esp = g_esp, arg = esp + 4;     /* FOVY */
    const uint32_t saved = MEM32(arg);
    const float scale = MEMF(0x1406F8);        /* the builder's FOVY to half-angle factor */
    const float fovy = MEMF(arg);
    uint32_t keep[(FRUSTUM_HI - FRUSTUM_LO) / 4];
    unsigned i;
    generated();                               /* the game's own frustum */
    if (!(target > 0.0f && target < 1.55f && isfinite(fovy) && isfinite(scale) && scale > 0.0f
          && fovy * scale > 0.0f && fovy * scale < target))
        return;
    for (i = 0; i < sizeof keep / 4; i++)
        keep[i] = MEM32(FRUSTUM_LO + 4 * i);
    g_esp = esp;                               /* the call again, as made */
    MEMF(arg) = target / scale;
    generated();
    MEM32(arg) = saved;
    for (i = 0; i < sizeof keep / 4; i++) {
        uint32_t a = FRUSTUM_LO + 4 * i;
        if (a < PLANES_LO || a >= PLANES_HI)
            MEM32(a) = keep[i];
    }
}
