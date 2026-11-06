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

/* Direct3D_CreateDevice (called by the game's IDirect3D8::CreateDevice
 * thunk sub_0013ECE0, with its presentation parameters at 0x41AEA0:
 * 640x480, 2-sample quincunx 0x1121): made without anti-aliasing
 * (the profile's d3d.no_multisample; runtime chihiro/d3d_fence.c).
 *
 * The game's screen copies read the back buffer as a texture, in its
 * pixels: sub_00149340 (the scope picture's copy into its 256x256 texture,
 * the screen grabs of the menus' transitions) takes a rectangle of its
 * 512x448 screen and scales it to the back buffer by 1280.0 / 512 across
 * (the float at 0x1F3240, read only there) and 480 / 448 down -- the
 * quincunx buffer's 1280 columns. So the width becomes the buffer's own:
 * 640 times the multisample type's columns per pixel (bits 4-7), 640
 * without anti-aliasing (1280 with it, the game's value). sub_00149760's
 * own 1280 (texture id 0x270F) is never reached. */
#define PRESENT_PARAMS  0x41AEA0u       /* BackBufferWidth +0, MultiSampleType +0x10 */
#define COPY_SRC_W      0x1F3240u       /* float: the back buffer's width for sub_00149340 */
extern void chihiro_d3d_create_device(void (*gen)(void));
extern void sub_00172E80_gen(void);
void sub_00172E80(void)
{
    chihiro_d3d_create_device(sub_00172E80_gen);
    MEMF(COPY_SRC_W) = (float)(MEM32(PRESENT_PARAMS) * ((MEM32(PRESENT_PARAMS + 0x10) >> 4) & 0xF));
}

/* The shared scope game (runtime host/scope_game.h; ss3's addresses in
 * scope.c): the rifle sets the aim (and builds the scope camera with it),
 * zooms through the scope's field of view, the copy into the scope's
 * texture publishes the lens picture with its aim and projection, and with
 * the rifle on the scope disc is not drawn, nor the reticle inside the
 * scope picture (the headset draws one); the main view's cull widens to the
 * headset's view, or with the picture at 16:9. The generated bodies run as
 * sub_X_gen. */
#include "host/scope_game.h"
extern void sub_001065B0_gen(void);
extern void sub_00105AE0_gen(void);
extern void sub_00105D20_gen(void);
extern void sub_00149340_gen(void);
extern void sub_000C1FB0_gen(void);
extern void sub_000C4FC0_gen(void);
extern void sub_000C13D0_gen(void);
void sub_001065B0(void) { ch_scope_aim_step(sub_001065B0_gen); }   /* aim from the cursor */
void sub_00105AE0(void) { ch_scope_aim_pre(sub_00105AE0_gen); }    /* scope camera from the aim */
void sub_00105D20(void) { ch_scope_fov(sub_00105D20_gen); }        /* the drawn scope projection */
void sub_00149340(void) { ch_scope_pass(sub_00149340_gen); }       /* screen copy (from 0xD2175: the scope's) */
void sub_000C1FB0(void) { ch_scope_circle(sub_000C1FB0_gen); }     /* scope disc and ring */
void sub_000C13D0(void) { ch_scope_cull(sub_000C13D0_gen); }       /* bounding-sphere cull */
/* the reticle: hidden only inside the scope picture (sub_000D2CB0's calls) */
void sub_000C4FC0(void)
{
    const uint32_t ret = MEM32(g_esp);
    if (ret == 0x000D2F0Eu || ret == 0x000D2FA7u || ret == 0x000D2FEEu)
        ch_scope_reticle(sub_000C4FC0_gen);
    else
        sub_000C4FC0_gen();
}

/* a view drawn into the back buffer (view 4: the scope's, for the copy) */
extern void sub_000D25F0_gen(void);
void sub_000D25F0(void) { ch_scope_screen_view(sub_000D25F0_gen); }

/* Return a function to run instead of the given guest address, or NULL for
 * the generated dispatch table. */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    (void)xbox_va;
    return (recomp_func_t)0;
}
