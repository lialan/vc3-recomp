/*
 * Silent Scope Complete's launcher (default.xbe): the guest functions it
 * replaces.
 *
 * A `void sub_XXXXXXXX(void)` defined here replaces the lifted body (re-lift
 * with scripts/lift.sh ssc, which passes this file as --exclude-manual). The
 * diagnostics the lifted code calls are the shared runtime's
 * (host/recomp_diag.c).
 */
#include <stdint.h>

#include "recomp_types.h"

/* XDK D3D's fence wait, against the push-buffer executor (runtime
 * chihiro/d3d_fence.c, as the games'). */
extern void chihiro_d3d_block_on_time(void);
void sub_00094EF0(void) { chihiro_d3d_block_on_time(); }   /* D3D_BlockOnTime */

/* Direct3D_CreateDevice (presentation parameters at 0x108C20: 640x480,
 * 2-sample quincunx 0x1121, as ss3's): made without anti-aliasing (the
 * profile's d3d.no_multisample; runtime chihiro/d3d_fence.c). */
extern void chihiro_d3d_create_device(void (*gen)(void));
extern void sub_000943F0_gen(void);
void sub_000943F0(void) { chihiro_d3d_create_device(sub_000943F0_gen); }

/* Return a function to run instead of the given guest address, or NULL for
 * the generated dispatch table. */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    (void)xbox_va;
    return (recomp_func_t)0;
}
