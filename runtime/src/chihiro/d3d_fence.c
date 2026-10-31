/*
 * D3D_BlockOnTime (0x10CAC0) for the push-buffer executor.
 *
 * VC3 links the XDK's D3D statically. Its fence wait spins until the GPU's
 * semaphore (*[device+0x30]) passes a fence time in the ring written at
 * [device+0x2C]. With RECOMP_PB_EXEC the executor is that GPU: it writes the
 * semaphore on BACK_END_WRITE_SEMAPHORE_RELEASE once it is told where. This
 * is the replacement xboxrecomp's docs prescribe
 * (docs/technical/d3d8ltcg-device-context.md, "When Something Does Read the
 * Pushbuffer"): keep the real semaphore, kick off first when the fence is the
 * newest, and wait for it with a bound.
 *
 * stdcall: BlockOnTime(uint32_t time, uint32_t flags), ret 8.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "recomp_types.h"
#include "host_time.h"
#include "chihiro/ch_env.h"
#include "chihiro/chihiro_profile.h"

void nv2a_pb_set_semaphore_target(uint32_t guest_va);

/* g_pDevice, KickOff and its return address: the profile's d3d block */
#define DEV_WRITE_TIME   0x2C          /* newest fence time */
#define DEV_SEMAPHORE    0x30          /* -> GPU's progress */
#define WAIT_BOUND_NS    250000000ull

/* VC3_FENCE_LOG=1: once a second, how many waits there were, how many had
 * to sleep, and the time spent in them. */
static void fence_stats(int slept, uint64_t ns)
{
    static int on = -1;
    static uint64_t t0, calls, sleeps, wait_ns;
    uint64_t now;
    if (on < 0) { const char *e = ch_env("FENCE_LOG"); on = e && *e == '1'; }
    if (!on) return;
    calls++; sleeps += slept; wait_ns += ns;
    now = xbox_HostNowNs();
    if (!t0) t0 = now;
    if (now - t0 >= 1000000000ull) {
        fprintf(stderr, "[fence] %llu waits/s, %llu slept, %.2f ms waiting\n",
                (unsigned long long)calls, (unsigned long long)sleeps, wait_ns / 1e6);
        t0 = now; calls = sleeps = wait_ns = 0;
    }
}

void vc3_d3d_block_on_time(void)
{
    static uint32_t s_sem_va;
    static int s_timeouts;
    const ChihiroProfile *gp = chihiro_profile();
    uint32_t time = MEM32(g_esp + 4);
    uint32_t dev = MEM32(gp->d3d.device_ptr);
    uint32_t sem, head;
    uint64_t deadline, start;
    int slept = 0;

    if (!dev)
        goto out;

    sem = MEM32(dev + DEV_SEMAPHORE);
    if (sem != s_sem_va) {           /* first call, or the device was reset */
        s_sem_va = sem;
        nv2a_pb_set_semaphore_target(sem);
        fprintf(stderr, "[D3D] GPU semaphore at 0x%08X\n", sem);
    }

    /* Nothing has submitted the release for the newest fence yet: kick off
     * the pushbuffer that holds it, as the original does first. */
    if (time == MEM32(dev + DEV_WRITE_TIME)) {
        PUSH32(g_esp, 0);
        PUSH32(g_esp, gp->d3d.kickoff_ret);
        gp->d3d.kickoff();           /* KickOff(0), ret 4 */
    }

    /* Done once the fence is no newer than the GPU's progress, in ring order
     * relative to the newest time (the original's unsigned comparison). */
    start = xbox_HostNowNs();
    deadline = start + WAIT_BOUND_NS;
    for (;;) {
        head = MEM32(dev + DEV_WRITE_TIME);
        if (head - time >= head - MEM32(sem))
            break;
        if (xbox_HostNowNs() >= deadline) {
            if (s_timeouts++ < 8)
                fprintf(stderr, "[D3D] BlockOnTime(0x%08X) timed out: head 0x%08X "
                        "sem 0x%08X\n", time, head, MEM32(sem));
            break;
        }
        xbox_HostSleepNs(50000);
        slept = 1;
    }
    fence_stats(slept, xbox_HostNowNs() - start);
out:
    g_eax = 0;
    g_esp += 12;                     /* return address + 2 args */
}
