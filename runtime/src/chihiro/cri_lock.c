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
 *     at once;
 *   - a thread held back STARVE_MS goes in anyway (gate()).
 * The count is the game's arithmetic exactly: its unlock is a bare
 * `dec; jnz`, so it goes below 0 and back. It does here: before a blocking
 * read 0xCA370 releases every share (other threads' too, which run beside
 * it here) and 0xCA3A0 takes them back after, so those threads' own unlocks
 * meanwhile take the count below 0. Stopped at 0, they were lost: the count
 * came back 1-2 too high and stayed held for good, holding the loaders back
 * until loading crawled (a Quest freeze, the game thread polling a load; none
 * in ~10 Quest runs since). The count drifted up too, on the Quest (to 26 in
 * one boot): a module that skips the lock while its server is inside lost
 * a share whenever the server came in between its lock and unlock --
 * ch_cri_scope_enter/leave, at the end of this file. The STARVE_MS escape
 * stays, so a share lost some other way only slows the loaders. (That
 * arithmetic was once tried without the "already inside" pass and starved
 * the loader 132 times in 200 s of Mac play.)
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
#include <time.h>

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

/* The calling thread's kernel id on Linux and Android (as [STACKS] and the
 * system's tools name it), else a short form of its pthread. */
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
static unsigned long cri_tid(void) { return (unsigned long)syscall(SYS_gettid); }
#else
static unsigned long cri_tid(void) { return (unsigned long)(uintptr_t)pthread_self() & 0xFFFFFu; }
#endif

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
            cri_tid(), prio, s_count, t_inside,
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

/* Who took the count from 0 (thread, guest return address and the code
 * addresses on its guest stack), for a wait that does not end; and each
 * thread's share inside, with where it last took it. */
static unsigned long s_holder;
static uint32_t s_holder_chain[6];
#define CRI_THREADS 32
static struct { unsigned long tid; int inside, takes, gives; int32_t prio; uint32_t chain[4]; } s_who[CRI_THREADS];
static int cri_slot(void)
{
    unsigned long me = cri_tid();
    for (int i = 0; i < CRI_THREADS; i++)
        if (s_who[i].tid == me) return i;
    for (int i = 0; i < CRI_THREADS; i++)
        if (!s_who[i].tid) { s_who[i].tid = me; return i; }
    return -1;
}

static void guest_chain(uint32_t *out, int n)
{
    int k, shown = 0;
    for (k = 0; k < 96 && shown < n; k++) {
        uint32_t v = MEM32(g_esp + 4u * k);
        if (v >= 0x10000u && v < 0x16D000u)
            out[shown++] = v;
    }
    while (shown < n)
        out[shown++] = 0;
}

/* Lock held. Wait as the console's spinner would make this thread wait --
 * but not for ever. The count is held for milliseconds on the console; here
 * it was seen held for 14 s and more on the Quest (the vblank server inside,
 * other threads nesting on top, the count never back to 0) while a thread
 * held back starved, and once every CRI thread then piled up behind it: the
 * game froze. Lifted, the threads run at once, so the count's nesting can
 * hold where the console's one CPU would have let the waiter in (and a
 * thread releasing other threads' shares, as 0xCA370 does before a read,
 * leaves them inside beside each other anyway). A thread held back STARVE_MS
 * goes in, as a starved thread is let run there; logged at most once a
 * second, with who is inside. Never reached in Mac boots or 200 s of Mac
 * play; at 1 s, threads that need the lock many times a second still
 * stalled the Quest for 10-40 s, so 100 ms. */
#define STARVE_MS 100
static void gate(int spin_prio)
{
    int32_t prio = xbox_GuestThreadBasePriority();
    if (!t_inside && prio <= kprio(spin_prio) && s_count > 0) {
        struct timespec start, until;
        cri_trace("wait", prio);
        clock_gettime(CLOCK_REALTIME, &start);
        until = start;
        until.tv_nsec += STARVE_MS * 1000000L;
        if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
        while (s_count > 0)
            if (pthread_cond_timedwait(&s_free, &s_mu, &until) != 0 && s_count > 0) {
                static uint64_t logged_s;
                if (until.tv_sec != (time_t)logged_s) { logged_s = (uint64_t)until.tv_sec;
                    uint32_t me[6];
                    guest_chain(me, 6);
                    fprintf(stderr, "[CRI] starved %d ms, let in: count %d taken by tid %lx from %X %X %X %X %X %X;"
                            " waiter tid %lx prio %d from %X %X %X %X %X %X\n", STARVE_MS, s_count, s_holder,
                            s_holder_chain[0], s_holder_chain[1], s_holder_chain[2], s_holder_chain[3],
                            s_holder_chain[4], s_holder_chain[5],
                            cri_tid(), prio,
                            me[0], me[1], me[2], me[3], me[4], me[5]);
                    for (int i = 0; i < CRI_THREADS; i++)
                        if (s_who[i].tid && (s_who[i].inside || s_who[i].takes != s_who[i].gives))
                            fprintf(stderr, "[CRI]   tid %lx inside %d takes %d gives %d prio %d last take from %X %X %X %X\n",
                                    s_who[i].tid, s_who[i].inside, s_who[i].takes, s_who[i].gives, s_who[i].prio,
                                    s_who[i].chain[0], s_who[i].chain[1], s_who[i].chain[2], s_who[i].chain[3]);
                }
                break;
            }
    }
}

/* Lock held: the game's lock body. */
static void take(int hold_prio)
{
    if (s_count++ == 0) {
        s_saved = xbox_GuestThreadSetBasePriority(kprio(hold_prio));
        s_holder = cri_tid();
        guest_chain(s_holder_chain, 6);
    }
    t_inside++;
    {
        int i = cri_slot();
        if (i >= 0) {
            s_who[i].inside = t_inside; s_who[i].takes++;
            s_who[i].prio = xbox_GuestThreadBasePriority();
            guest_chain(s_who[i].chain, 4);
        }
    }
    cri_trace("lock", xbox_GuestThreadBasePriority());
}

/* Lock held: the game's unlock body. */
static void give(void)
{
    if (t_inside > 0)
        t_inside--;
    {
        int i = cri_slot();
        if (i >= 0) { s_who[i].inside = t_inside; s_who[i].gives++; }
    }
    /* the game's `dec; jnz`: below 0 too (see the header) */
    if (--s_count == 0) {
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

/* A lock taken only while a module's "inside" counter is 0, and given back
 * the same way (HOTD3's Sofdec 0xD3D20 / 0xD3D30 on [0x4FB178]: the server
 * 0xD5C70 counts itself inside around its work, which already holds the
 * lock, so its own nested calls skip it). The game decides each half from
 * the counter as it is at that moment. On the console nothing else runs
 * between a thread's two halves to change it; lifted, the server raises it
 * on another core in between, and the unlock skips the lock its own lock
 * took: the count leaked one share at a time (to 26 in a Quest boot), every
 * other CRI thread waited out STARVE_MS on each take, and loading crawled --
 * the boot looked frozen. The halves are scoped in one function (HOTD3: 22
 * functions, an unlock on each exit), so each thread keeps its own record:
 * enter decides from the counter, as the game does, and remembers; leave
 * gives back exactly what its enter took. Returns 1 when the caller should
 * take (enter) or give (leave) the lock. More than 64 deep, or a leave with
 * no enter on this thread: the counter, as the game. */
static RECOMP_TLS uint64_t t_scope_took;
static RECOMP_TLS int t_scope_depth;

int ch_cri_scope_enter(uint32_t counter_va)
{
    int take = (int32_t)MEM32(counter_va) <= 0;
    if (t_scope_depth < 64) {
        if (take) t_scope_took |= 1ull << t_scope_depth;
        else      t_scope_took &= ~(1ull << t_scope_depth);
    }
    t_scope_depth++;
    return take;
}

int ch_cri_scope_leave(uint32_t counter_va)
{
    if (t_scope_depth > 0 && --t_scope_depth < 64)
        return (int)((t_scope_took >> t_scope_depth) & 1u);
    return (int32_t)MEM32(counter_va) <= 0;
}
