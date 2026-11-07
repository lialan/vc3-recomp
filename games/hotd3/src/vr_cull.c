/*
 * vr_cull.c - HOTD3's CPU culling of its stage, widened for the immersive view.
 *
 * The stage is drawn by sub_00013F00 from a list of parts for the camera's
 * stretch of the rail (16 words at [0x276498] + 32 * the stretch, word
 * [0x27A224], -1 ending it; 3-8 parts in chapter 1's lab). A part ([0x27A100]
 * + 4 * its index: model word +0, offset +2, how it is culled +3) with +3 1
 * is always drawn; with 0, only if its bounding sphere (the model's +0x14,
 * sub_000FAD70) passes sub_00048F50 (cdecl: x, y, z, radius; eax: drawn).
 * That test takes the view's half-angles from the focal, atan(320 / f) and
 * atan(240 / f) with f = [0x27A148] (sub_000F87E0), and checks the sphere,
 * in the camera's frame, against the planes turned by them. Run with the
 * focal of a wide view (240 / tan 70 degrees) it rejected parts that it
 * passes with the game's own -- the room around the camera -- so it is not
 * widened through its angles: while the immersive view asks for more than
 * the game's own view (hotd3_vr_cull_half_angle), every part listed for the
 * stretch is drawn. The test still runs (it saves and restores the matrix
 * stack it uses); only its answer is replaced.
 * Parts not listed for the current stretch -- the room behind a door the
 * player has gone through, loaded but listed for other stretches -- are not
 * drawn at all: that is the game's own and not widened here.
 */
#include <math.h>
#include <stdint.h>
#include "recomp_types.h"

#define FOCAL 0x27A148u             /* 240 / tan(FOVY / 2), pixels */

/* Half the vertical angle (radians) to cull with; 0: the game's own. */
float hotd3_vr_cull_half_angle;
void hotd3_vr_cull_set(float half) { hotd3_vr_cull_half_angle = half; }

void hotd3_cull_sphere(void (*generated)(void))
{
    const float half = hotd3_vr_cull_half_angle, focal = MEMF(FOCAL);
    generated();
    if (half > 0.0f && isfinite(focal) && focal > 0.0f && half > atanf(240.0f / focal))
        g_eax = 1;
}
