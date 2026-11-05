/*
 * Silent Scope (ss1.xbe): the guest functions this game replaces.
 *
 * A `void sub_XXXXXXXX(void)` defined here replaces the lifted body (re-lift
 * with scripts/lift.sh ss1, which passes this file as --exclude-manual): every
 * direct call, tail jump and ICALL goes to the definition. The diagnostics
 * the lifted code calls are the shared runtime's (host/recomp_diag.c).
 */
#include <stdint.h>

#include "recomp_types.h"

/* XDK D3D's fence wait, against the push-buffer executor (runtime
 * chihiro/d3d_fence.c, as VC3's 0x10CAC0). The original's "GPU far behind"
 * path sleeps on an interrupt nothing delivers. */
extern void chihiro_d3d_block_on_time(void);
void sub_000FC580(void) { chihiro_d3d_block_on_time(); }   /* D3D_BlockOnTime */

/* Return a function to run instead of the given guest address, or NULL for
 * the generated dispatch table. Called on every indirect call: keep it
 * cheap. A definition above is the usual way; this is for wrappers chosen
 * at run time. */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    (void)xbox_va;
    return (recomp_func_t)0;
}
