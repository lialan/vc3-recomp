/*
 * CRI middleware's lock, for any game linking CRI ADX / Sofdec (HOTD3; VC3's
 * is the same design).
 *
 * The ADX manager's lock (registered as the file system's lock callback;
 * HOTD3 0xBE6D0 / 0xBE710) raises the caller's priority, suspends the idle
 * thread and counts in a plain global. On the console's one CPU that is the
 * lock: while the holder runs nothing else of CRI does -- but when it sleeps
 * or waits, everything may. Lifted, the CRI threads (the vsync server, the
 * file-system server) and the game thread run on separate host cores,
 * SuspendThread of another thread only counts, and the library's shared state
 * is touched by two threads at once: in HOTD3 a read went out with one
 * request's length and another's file position, and the boot stalled on a
 * black screen one run in four.
 *
 * A host mutex is not that lock: HOTD3's game thread sleeps holding it,
 * waiting for a worker that needs it to drain a queue (0x7B000), and with a
 * mutex the two deadlocked. So the lock is the toolkit's scoped guest CPU
 * (kernel_guest_cpu.c): a thread inside holds the one CPU of the threads
 * inside until it blocks or leaves -- the priority boost -- and the servers'
 * per-tick steps run inside too (ch_cri_server), as the holder's priority
 * kept them out on the console. Threads outside run free.
 *
 * A game replaces its lock and unlock with ch_cri_lock/ch_cri_unlock and
 * wraps its server steps with ch_cri_server (games/<id>/src/recomp_manual.c).
 * CHIHIRO_CRI_LOCK=0 (or <PREFIX>_CRI_LOCK=0) leaves CRI unguarded.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "recomp_types.h"
#include "chihiro/ch_env.h"

void xbox_GuestCpuScopeEnable(void);
void xbox_GuestCpuScopeEnter(void);
void xbox_GuestCpuScopeLeave(void);

static pthread_once_t s_once = PTHREAD_ONCE_INIT;
static int s_on = 1;

static void cri_init(void)
{
    const char *e = ch_env("CRI_LOCK");
    s_on = !(e && *e == '0');
    if (s_on)
        xbox_GuestCpuScopeEnable();
    fprintf(stderr, "[CRI] middleware lock: %s\n",
            s_on ? "the scoped guest CPU (held until the holder blocks)" : "off (CRI_LOCK=0)");
}

void ch_cri_lock(void)
{
    pthread_once(&s_once, cri_init);
    if (s_on)
        xbox_GuestCpuScopeEnter();
}

void ch_cri_unlock(void)
{
    pthread_once(&s_once, cri_init);
    if (s_on)
        xbox_GuestCpuScopeLeave();
}

/* One server step (a lifted function, the generated body) inside the lock. */
void ch_cri_server(void (*step)(void))
{
    ch_cri_lock();
    step();
    ch_cri_unlock();
}
