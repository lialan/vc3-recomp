/*
 * CRI's stream ring error trap (0xBE8B0), returning instead of halting.
 *
 * VC3's CRI stream library keeps its data in ring buffers and checks every
 * chunk handed back: one that is not the next in the ring is reported, code
 * -3, to an error callback the library's owner installs -- here a `jmp $`
 * at 0xBE8B0, reached only through that callback's indirect call
 * (sub_000BEBE0). On the console it could not happen: the library's lock
 * (sub_000B9F70/sub_000B9FB0) raises the caller's priority and suspends the
 * library's server thread around each ring update, and with one CPU the two
 * never ran at once. Lifted, the kernel's SuspendThread of another thread
 * only counts (POSIX cannot stop a thread anywhere), so on two host cores
 * the server runs on beside the lock, a chunk now and then comes back out
 * of order, and the trap took the thread that saw it: a worker spinning a
 * core for the rest of the run, or the main thread, the game frozen.
 *
 * After its callback returns the library goes on (it drops the chunk from
 * the ring's count, sub_000BEBE0 at 0xBEC5D), so here the callback notes
 * the error and returns, a cdecl `ret`: a hiccup in a stream instead of a
 * hang. The race itself stays; stopping a thread at its next kernel call
 * for SuspendThread is the real fix (the toolkit's suspend-safe-points
 * branch: not yet, its waits stalled threads that run outside kernel calls).
 * VC3_CRI_TRAP=1 keeps the original halt.
 *
 * Installed in the flat dispatch table's entry once it is built
 * (chihiro_game_dispatch_ready, main.c): the trap is only ever called
 * through it, and an override by recomp_lookup_manual would unpublish that
 * table and send every indirect call down the slow lookup.
 */
#include <stdio.h>
#include <stdint.h>

#include "recomp_types.h"
#include "chihiro/ch_env.h"

void vc3_cri_stream_error(void)
{
    static unsigned seen;
    static int keep = -1;
    if (keep < 0) {
        const char *e = ch_env("CRI_TRAP");
        keep = e && *e == '1';
    }
    if (keep)
        for (;;) {}                    /* the original: jmp $ */
    if (seen++ < 16 || seen % 1000 == 0)
        fprintf(stderr, "[CRI] stream ring error %d (handle %08X): chunk out of order, going on (%u so far)\n",
                (int)MEM32(g_esp + 8), MEM32(g_esp + 4), seen);
    g_esp += 4;                        /* ret: the caller drops its two arguments */
}

void chihiro_game_dispatch_ready(void)
{
    uint32_t off = 0x000BE8B0u - g_recomp_flat_base;
    if (g_recomp_flat && off < g_recomp_flat_span && g_recomp_flat[off]) {
        g_recomp_flat[off] = (recomp_icall_fn_t)vc3_cri_stream_error;
        fprintf(stderr, "[CRI] stream ring error trap 0xBE8B0: returns instead of halting\n");
    } else
        fprintf(stderr, "[CRI] no flat dispatch: the stream ring error trap stays a halt\n");
}
