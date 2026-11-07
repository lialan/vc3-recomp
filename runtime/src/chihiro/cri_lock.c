/*
 * CRI middleware's lock, for any game linking CRI ADX / Sofdec (HOTD3; VC3's
 * is the same design).
 *
 * The ADX manager's lock (registered as the file system's lock callback;
 * HOTD3 0xBE6D0 / 0xBE710) is a global count. Taking it from 0 saves the
 * caller's priority, raises it to time-critical (15) and resumes a spinner
 * thread at "highest" (2); releasing it to 0 suspends the spinner and gives
 * the releasing thread the saved priority. On the console's one CPU that is
 * the lock: while it is held nothing at priority 2 or below runs -- not even
 * while the holder sleeps or waits, because then the spinner has the CPU.
 * HOTD3's CRI threads are all there: the vsync server (1), the game thread
 * (0), the file-system server and the loader (-2). A thread above 2 (the
 * file-system server while the game boosts it to 15 for a synchronous step)
 * is not held back, as it would preempt on the console. The library leans on
 * the priority too: before a blocking read it releases the lock as many
 * times as it takes for its priority to drop below 15, and takes it back that
 * many times after (HOTD3 0xCA370 / 0xCA3A0).
 *
 * Lifted, those threads run at once on the host. A lock that lets them in
 * while the holder waits is not this one (in HOTD3 a read went out with one
 * stream's length and another's position, a black screen one boot in three);
 * nor is a mutex that ignores priorities (the holder waits for a boosted
 * server step). So, under one host mutex:
 *   - one global count, as the game keeps it; any thread may release it;
 *   - the priority effects are the game's: from 0 the caller is raised to
 *     the holder's priority, to 0 the releaser gets the saved one back;
 *   - while the count is held, a thread at or below the spinner's priority
 *     waits to take it -- across the holder's waits, as the spinner made it;
 *   - a thread above the spinner's priority, or one already inside, takes it
 *     at once.
 * Priorities are the game's own, kept by the kernel bridge
 * (xbox_GuestThreadBasePriority; time-critical is 16 there, as in the kernel).
 *
 * The CRI modules nest the lock behind their own counters (HOTD3: ADXT
 * 0xBFB00 / 0xBFB20 on [0x30F8A0], the stream file system 0xC8260 / 0xC8280
 * on [0x30FF30]): take the lock only when the counter is 0, give it back when
 * it returns to 0. On the console only the holder runs while that counter is
 * above 0; here another thread could read it, skip the lock and run inside
 * beside the holder, and the two lost each other's increments -- the counter
 * never came back to 0 and the boot froze. ch_cri_counter_lock/unlock do the
 * same thing atomically, waiting as the lock does.
 *
 * A game replaces its lock, unlock and counter wrappers with these
 * (games/<id>/src/recomp_manual.c). CHIHIRO_CRI_LOCK=0 (or
 * <PREFIX>_CRI_LOCK=0) leaves CRI unguarded; CHIHIRO_CRI_TRACE=1 logs every
 * lock and unlock with the guest call chain.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "recomp_types.h"
#include "chihiro/ch_env.h"

int32_t xbox_GuestThreadBasePriority(void);           /* LONG */
int32_t xbox_GuestThreadSetBasePriority(int32_t prio);

static pthread_once_t s_once = PTHREAD_ONCE_INIT;
static int s_on = 1;
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_free = PTHREAD_COND_INITIALIZER;
static int s_count;                       /* the game's count */
static int32_t s_saved;                   /* the first holder's priority */
static RECOMP_TLS int t_inside;           /* this thread's net share of it */
static int s_trace;

static void cri_trace(const char *what, int prio)
{
    char chain[160];
    int n = 0, k, shown = 0;

    if (!s_trace)
        return;
    /* code addresses on the guest stack: the call chain, roughly */
    for (k = 1; k < 96 && shown < 8; k++) {
        uint32_t v = MEM32(g_esp + 4u * k);
        if (v >= 0x10000u && v < 0x16D000u) {
            n += snprintf(chain + n, sizeof chain - (size_t)n, " %X", v);
            shown++;
        }
    }
    chain[n] = 0;
    fprintf(stderr, "[CRI] %s tid %lx prio %d count %d inside %d from %08X:%s\n", what,
            (unsigned long)(uintptr_t)pthread_self() & 0xFFFFFu, prio, s_count, t_inside,
            MEM32(g_esp), chain);
}

static void cri_init(void)
{
    const char *e = ch_env("CRI_LOCK");
    const char *t = ch_env("CRI_TRACE");
    s_trace = t && *t == '1';
    s_on = !(e && *e == '0');
    fprintf(stderr, "[CRI] middleware lock: %s\n",
            s_on ? "the console's (held across waits; threads above the spinner's priority pass)"
                 : "off (CRI_LOCK=0)");
}

/* SetThreadPriority's terms to the kernel's: time-critical is 16. */
static int32_t kprio(int prio)
{
    return prio == 15 ? 16 : prio == -15 ? -16 : prio;
}

/* Lock held. Wait as the console's spinner would make this thread wait. */
static void gate(int spin_prio)
{
    int32_t prio = xbox_GuestThreadBasePriority();
    if (!t_inside && prio <= kprio(spin_prio) && s_count > 0) {
        cri_trace("wait", prio);
        while (s_count > 0)
            pthread_cond_wait(&s_free, &s_mu);
    }
}

/* Lock held: the game's lock body. */
static void take(int hold_prio)
{
    if (s_count++ == 0)
        s_saved = xbox_GuestThreadSetBasePriority(kprio(hold_prio));
    t_inside++;
    cri_trace("lock", xbox_GuestThreadBasePriority());
}

/* Lock held: the game's unlock body. */
static void give(void)
{
    if (t_inside > 0)
        t_inside--;
    if (s_count > 0 && --s_count == 0) {
        xbox_GuestThreadSetBasePriority(s_saved);
        pthread_cond_broadcast(&s_free);
    }
    cri_trace("unlock", xbox_GuestThreadBasePriority());
}

/* spin_prio, hold_prio: the spinner's and the holder's priorities in
 * SetThreadPriority's terms (HOTD3 [0x30F6F8] = 2, [0x30F6F4] = 15). */
void ch_cri_lock(int spin_prio, int hold_prio)
{
    pthread_once(&s_once, cri_init);
    if (!s_on)
        return;
    pthread_mutex_lock(&s_mu);
    gate(spin_prio);
    take(hold_prio);
    pthread_mutex_unlock(&s_mu);
}

void ch_cri_unlock(void)
{
    pthread_once(&s_once, cri_init);
    if (!s_on)
        return;
    pthread_mutex_lock(&s_mu);
    give();
    pthread_mutex_unlock(&s_mu);
}

/* A module's nesting counter at guest address counter_va: take the lock if it
 * is 0, then count. */
void ch_cri_counter_lock(uint32_t counter_va, int spin_prio, int hold_prio)
{
    pthread_once(&s_once, cri_init);
    if (!s_on) {
        MEM32(counter_va)++;
        return;
    }
    pthread_mutex_lock(&s_mu);
    gate(spin_prio);
    if (MEM32(counter_va) == 0)
        take(hold_prio);
    MEM32(counter_va)++;
    pthread_mutex_unlock(&s_mu);
}

/* Count down; give the lock back when the counter returns to 0. */
void ch_cri_counter_unlock(uint32_t counter_va)
{
    pthread_once(&s_once, cri_init);
    if (!s_on) {
        MEM32(counter_va)--;
        return;
    }
    pthread_mutex_lock(&s_mu);
    if (--MEM32(counter_va) == 0)
        give();
    pthread_mutex_unlock(&s_mu);
}
