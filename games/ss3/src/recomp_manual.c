/*
 * Silent Scope 3 (ss3.xbe): the guest functions this game replaces.
 *
 * A `void sub_XXXXXXXX(void)` defined here replaces the lifted body (re-lift
 * with scripts/lift.sh ss3, which passes this file as --exclude-manual). The
 * diagnostics the lifted code calls are the shared runtime's
 * (host/recomp_diag.c).
 */
#include <stdint.h>

#include "recomp_types.h"

/* XDK D3D's fence wait, against the push-buffer executor (runtime
 * chihiro/d3d_fence.c, as ss1's 0xFC580 and VC3's 0x10CAC0). */
extern void chihiro_d3d_block_on_time(void);
void sub_00175FE0(void) { chihiro_d3d_block_on_time(); }   /* D3D_BlockOnTime */

/* Return a function to run instead of the given guest address, or NULL for
 * the generated dispatch table. */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    (void)xbox_va;
    return (recomp_func_t)0;
}
