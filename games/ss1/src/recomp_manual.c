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

/* At 16:9 (wide.c): the main view's culling and the aim's yaw limit widen
 * with the picture; in VR the culling widens to the headset's view and the
 * rifle sets the aim (vr_rifle.c). The generated bodies run as sub_X_gen. */
extern void ss1_wide_camera(void (*generated)(void));
extern void ss1_wide_aim(void (*generated)(void));
extern void ss1_vr_aim(void);
extern void sub_0006D650_gen(void);
extern void sub_000588C0_gen(void);
void sub_0006D650(void) { ss1_wide_camera(sub_0006D650_gen); }   /* camera block -> cull */
void sub_000588C0(void) { ss1_wide_aim(sub_000588C0_gen); ss1_vr_aim(); }   /* aim step and clamp */

/* The VR rifle (vr_rifle.c): the scope pass publishes its picture for the
 * lens; with the rifle on, the circle is not drawn (its zoom factor is still
 * written, times the rifle's zoom), nor the aim marks while there is a
 * scope picture. */
extern void ss1_vr_scope_pass(void (*generated)(void));
extern void ss1_vr_circle(void (*generated)(void));
extern void ss1_vr_marks(void (*generated)(void));
extern void sub_000744B0_gen(void);
extern void sub_00075F40_gen(void);
extern void sub_00076490_gen(void);
void sub_000744B0(void) { ss1_vr_scope_pass(sub_000744B0_gen); }   /* scope pass */
void sub_00075F40(void) { ss1_vr_circle(sub_00075F40_gen); }       /* scope circle */
void sub_00076490(void) { ss1_vr_marks(sub_00076490_gen); }        /* circle at the aim, aim marks */

/* Return a function to run instead of the given guest address, or NULL for
 * the generated dispatch table. Called on every indirect call: keep it
 * cheap. A definition above is the usual way; this is for wrappers chosen
 * at run time. */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    (void)xbox_va;
    return (recomp_func_t)0;
}
