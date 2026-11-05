/*
 * vr_lens.c - the lens picture (vr_view.h): the guest surface a game draws
 * its scope view into, which the renderer hands the headset for the rifle's
 * lens. Set by the game (Silent Scope: its 256x256 scope target), read by
 * the renderer; 0 when the game has none (VC3).
 */
#include <stdint.h>

#include "vr_view.h"

static uint32_t s_lens_surface;

void vc3_vr_lens_surface_set(uint32_t va)
{
    __atomic_store_n(&s_lens_surface, va, __ATOMIC_RELAXED);
}

uint32_t vc3_vr_lens_surface(void)
{
    return __atomic_load_n(&s_lens_surface, __ATOMIC_RELAXED);
}
