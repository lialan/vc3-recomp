/*
 * Silent Scope 2 (ss2.xbe): the guest functions this game replaces.
 *
 * A `void sub_XXXXXXXX(void)` defined here replaces the lifted body (re-lift
 * with scripts/lift.sh ss2, which passes this file as --exclude-manual). The
 * diagnostics the lifted code calls are the shared runtime's
 * (host/recomp_diag.c).
 */
#include <stdint.h>

#include "recomp_types.h"

/* XDK D3D's fence wait, against the push-buffer executor (runtime
 * chihiro/d3d_fence.c, as ss1's 0xFC580 and VC3's 0x10CAC0). */
extern void chihiro_d3d_block_on_time(void);
void sub_0012CB50(void) { chihiro_d3d_block_on_time(); }   /* D3D_BlockOnTime */

/* The shared scope game (runtime host/scope_game.h; ss2's addresses in
 * scope.c): at 16:9 the main view's culling and the aim's yaw limit widen
 * with the picture; in VR the culling widens to the headset's view and the
 * rifle sets the aim, the scope view publishes its picture for the lens
 * with its aim and projection, and with the rifle on the circle is not
 * drawn (its zoom factor is still written, times the rifle's zoom, over
 * the guard band, followed into the scope's focal length), nor the aim
 * marks while there is a scope picture, nor the scope's reticle (the
 * headset draws one). The generated bodies run as sub_X_gen. */
#include "host/scope_game.h"
extern void sub_00097C60_gen(void);
extern void sub_0006D590_gen(void);
extern void sub_0009D5F0_gen(void);
extern void sub_000A4240_gen(void);
extern void sub_000768E0_gen(void);
extern void sub_00076D90_gen(void);
extern void sub_00066AA0_gen(void);
void sub_00097C60(void) { ch_scope_camera(sub_00097C60_gen); }     /* camera block -> cull */
void sub_0006D590(void) { ch_scope_aim_step(sub_0006D590_gen); }   /* aim step and clamp */
void sub_0009D5F0(void) { ch_scope_pass(sub_0009D5F0_gen); }       /* scope pass (sets its target up) */
void sub_000A4240(void) { ch_scope_focal(sub_000A4240_gen); }      /* a camera mode's focal length */
void sub_000768E0(void) { ch_scope_circle(sub_000768E0_gen); }     /* scope circle */
void sub_00076D90(void) { ch_scope_marks(sub_00076D90_gen); }      /* circle at the aim, aim marks */
void sub_00066AA0(void) { ch_scope_reticle(sub_00066AA0_gen); }    /* the scope's reticle */
/* The main loop's tick, once a frame in every state (before the frame
 * counter 0xB3D034 steps): the bot's buttons (SS2_BOT) */
extern void sub_00069D00_gen(void);
void sub_00069D00(void) { ch_scope_frame(sub_00069D00_gen); }

/* Return a function to run instead of the given guest address, or NULL for
 * the generated dispatch table. */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    (void)xbox_va;
    return (recomp_func_t)0;
}
