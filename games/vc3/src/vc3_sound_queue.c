/*
 * VC3's sound event queue: one host lock for its two sides.
 *
 * The game hands its sound engine events (MIDI-like: note on and off,
 * controls; every sound effect starts as one) through a 64-entry ring at
 * *(0x51FA2C)+0x1A0. The sender, sub_000ECB80, called from the game's own
 * code, stores an event at the write index (+0x5A3) and adds one to the
 * count byte (+0x5A1) with `inc byte [..]`. The sound thread, once a vblank
 * (sub_000ED900), takes events from the read index (+0x5B6) in
 * sub_000ED750 and takes one off the count with `dec byte [..]`. The
 * "lock" each side takes is a critical section on its own stack, which
 * excludes nothing: on the console's one CPU a single-instruction inc and
 * dec could not interleave, so nothing more was needed.
 *
 * Lifted, each is a load, an add and a store, and on two host cores a
 * decrement now and then was lost to an increment beside it. The count is
 * then one too high: the sound thread takes the empty slot after the last
 * event and from then on reads one slot ahead of the sender, where nothing
 * has been written yet -- every sound effect after that is dropped, for the
 * rest of the run. The music, streamed outside this queue, plays on.
 *
 * Here both functions run whole under one host lock, so the two sides never
 * interleave, as on the console. Recursive, for an event sent from the
 * sound thread's own side. A guest thread waiting for it lets go of the
 * guest CPU (RECOMP_GUEST_LOCK) as a blocking kernel call does: the holder
 * may need the CPU to finish. VC3_SOUND_QUEUE_LOCK=0 runs them unlocked,
 * as before.
 */
#include <pthread.h>
#include <stdint.h>

#include "chihiro/ch_env.h"

extern int  xbox_GuestCpuRelease(int kind, uint32_t where);
extern void xbox_GuestCpuAcquire(void);
extern void xbox_GuestCpuBlocked(void);

static pthread_mutex_t s_lock;
static pthread_once_t s_once = PTHREAD_ONCE_INIT;
static int s_off;

static void lock_init(void)
{
    const char *e = ch_env("SOUND_QUEUE_LOCK");
    pthread_mutexattr_t a;
    s_off = e && *e == '0';
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_lock, &a);
    pthread_mutexattr_destroy(&a);
}

void vc3_sound_queue_locked(void (*generated)(void))
{
    pthread_once(&s_once, lock_init);
    if (s_off) {
        generated();
        return;
    }
    if (pthread_mutex_trylock(&s_lock) != 0) {
        int held = xbox_GuestCpuRelease(0 /* XBOX_GUEST_CPU_KERNEL */, 0);
        if (held)
            xbox_GuestCpuBlocked();
        pthread_mutex_lock(&s_lock);
        if (held)
            xbox_GuestCpuAcquire();
    }
    generated();
    pthread_mutex_unlock(&s_lock);
}
