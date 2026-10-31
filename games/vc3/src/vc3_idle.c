/*
 * VC3's idle thread (sub_000BA050), without spinning a host core.
 *
 * The routine at 0x10BA190 starts a guest thread at 0xBA050 (PsCreate-
 * SystemThreadEx through sub_000B604F). That thread is the console's idle
 * loop: it adds one to a counter at 0x4BA83C for as long as the stop flag
 * at 0x4BA854 is zero, then acknowledges at 0x4BA858 and ends itself
 * (sub_000B5ED4: PsTerminateSystemThread, status 0xF0000001):
 *
 *     BA050: mov eax,[4BA854]; test eax,eax; jne stop
 *     BA060: inc dword [4BA83C]; cmp [4BA854],0; je BA060
 *     stop:  push F0000001; mov [4BA858],1; call B5ED4
 *
 * The shutdown side (0xBA405..0xBA47D) sets 0x4BA854, waits on the thread
 * and checks 0x4BA858. Nothing in the game reads the counter -- no
 * instruction names 0x4BA83C but this loop, and the block copy at 0xBA23B
 * that initialises the neighbouring words stops at 0x4BA833 -- so its rate
 * is free to change. On a console this thread ran only when nothing else
 * could; here it is a host thread of its own, and it kept a whole core
 * (one of the Quest's fast ones) busy, heat that throttles a long session.
 *
 * The same loop, sleeping a millisecond between checks: the counter still
 * counts, the stop flag is seen within a millisecond, and the exit path is
 * the original's. The flag is plain guest memory with no event behind it,
 * so a poll is all there is to wait on. VC3_IDLE_SPIN=1 spins as before.
 */
#include <stdint.h>
#include <stdlib.h>

#include "recomp_types.h"
#include "recomp_funcs.h"
#include "host_time.h"
#include "chihiro/ch_env.h"

#define IDLE_STOP     0x4BA854u   /* set by the shutdown side */
#define IDLE_COUNT    0x4BA83Cu   /* the idle counter */
#define IDLE_STOPPED  0x4BA858u   /* the thread's acknowledgement */

void vc3_idle_thread(void)
{
    static int spin = -1;
    if (spin < 0) {
        const char *e = ch_env("IDLE_SPIN");
        spin = e && *e == '1';
    }
    while (!MEM32(IDLE_STOP)) {
        MEM32(IDLE_COUNT) = MEM32(IDLE_COUNT) + 1;
        if (!spin)
            xbox_HostSleepNs(1000000);
    }
    PUSH32(g_esp, 0xF0000001u);
    MEM32(IDLE_STOPPED) = 1;
    PUSH32(g_esp, 0x000BA088u);
    sub_000B5ED4();                   /* PsTerminateSystemThread: no return */
    sub_000BA090();                   /* the original's fall-through after it */
}
