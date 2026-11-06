/*
 * D3D_BlockOnTime for the push-buffer executor, for any game that links the
 * XDK's D3D (the profile's d3d block: VC3 0x10CAC0, Silent Scope 0xFC580).
 *
 * The games link the XDK's D3D statically. Its fence wait spins until the GPU's
 * semaphore (*[device+0x30]) passes a fence time in the ring written at
 * [device+0x2C]. With RECOMP_PB_EXEC the executor is that GPU: it writes the
 * semaphore on BACK_END_WRITE_SEMAPHORE_RELEASE once it is told where. This
 * is the replacement xboxrecomp's docs prescribe
 * (docs/technical/d3d8ltcg-device-context.md, "When Something Does Read the
 * Pushbuffer"): keep the real semaphore, kick off first when the fence is the
 * newest, and wait for it with a bound.
 *
 * XDK 5558's (Silent Scope's) also has a "GPU far behind" path: it patches a
 * software method (NO_OPERATION 5) into the fence's block and sleeps on an
 * event the GPU's interrupt for that method sets. Nothing delivers that
 * interrupt, so the original slept for good once the GPU fell 9 KB behind;
 * this wait polls the semaphore instead.
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
void nv2a_pb_set_semaphore_resolver(uint32_t (*resolve)(void));

/* g_pDevice, KickOff and its return address: the profile's d3d block */
#define DEV_WRITE_TIME   0x2C          /* newest fence time */
#define DEV_SEMAPHORE    0x30          /* -> GPU's progress */
#define WAIT_BOUND_NS    250000000ull
#define STRICT_WAIT_BOUND_NS 30000000000ull

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

/* The semaphore D3D reads GPU progress from (*[device+0x30]), for the
 * executor's first release: D3D's ring-space wait reads it too, and in
 * XDK 5558 (Silent Scope) it does so before any BlockOnTime has named it
 * -- with those releases dropped, the wait never ended. 0 until the device
 * exists (the release is then dropped, as before). */
static uint32_t d3d_semaphore_va(void)
{
    const ChihiroProfile *gp = chihiro_profile();
    uint32_t dev = gp->d3d.device_ptr ? MEM32(gp->d3d.device_ptr) : 0;
    return dev ? MEM32(dev + DEV_SEMAPHORE) : 0;
}

void chihiro_d3d_init(void)
{
    if (chihiro_profile()->d3d.device_ptr)
        nv2a_pb_set_semaphore_resolver(d3d_semaphore_va);
}

void chihiro_d3d_block_on_time(void)
{
    static uint32_t s_sem_va;
    static int s_timeouts;
    const ChihiroProfile *gp = chihiro_profile();
    uint32_t time = MEM32(g_esp + 4);
    uint32_t dev = MEM32(gp->d3d.device_ptr);
    uint32_t sem, head;
    uint64_t deadline, start;
    int slept = 0;
    const char *strict_env = ch_env("STRICT_GPU_FENCES");
#ifdef CHIHIRO_RENDERER_VULKAN
    int strict = !strict_env || atoi(strict_env) != 0;
#else
    int strict = strict_env && atoi(strict_env) != 0;
#endif
    int reported_slow = 0;
    static unsigned slow_reports;

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
    uint64_t strict_bound = STRICT_WAIT_BOUND_NS;
    /* Explicit offline cache training may spend minutes in driver compilation.
     * Extend only the watchdog, never acknowledge an incomplete GPU fence. */
    const char *training_ms = ch_env("GPU_FENCE_TIMEOUT_MS");
    if (strict && training_ms && *training_ms) {
        char *end = NULL;unsigned long ms = strtoul(training_ms, &end, 10);
        if (end && !*end && ms >= 30000 && ms <= 600000)
            strict_bound = (uint64_t)ms * 1000000ull;
    }
    deadline = start + (strict ? strict_bound : WAIT_BOUND_NS);
    for (;;) {
        head = MEM32(dev + DEV_WRITE_TIME);
        if (head - time >= head - MEM32(sem))
            break;
        uint64_t now = xbox_HostNowNs();
        if (strict && !reported_slow && now - start >= WAIT_BOUND_NS) {
            reported_slow = 1;
            if (slow_reports++ < 8)
                fprintf(stderr, "[D3D] BlockOnTime(0x%08X) awaiting actual GPU completion beyond 250 ms: head 0x%08X sem 0x%08X\n",
                        time, head, MEM32(sem));
        }
        if (now >= deadline) {
            if (s_timeouts++ < 8)
                fprintf(stderr, "[D3D] BlockOnTime(0x%08X) timed out: head 0x%08X "
                        "sem 0x%08X\n", time, head, MEM32(sem));
            /* A strict wait must never tell the SDK that an
             * incomplete fence is reusable. Keep the legacy bound as the
             * control; a stuck strict wait terminates with diagnostics. */
            if (strict) abort();
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

/* A fence in the push buffer here, not kicked off: D3D's own insert (the
 * profile's kickoff with flags 2; XDK 5558: insert a fence, kick unless
 * flags & 2). Returns its time -- the GPU has drawn everything pushed
 * before it once chihiro_d3d_progress() reaches it, in ring order
 * ((int32_t)(progress - time) >= 0) -- or 0 with no device yet. */
uint32_t chihiro_d3d_fence(void)
{
    const ChihiroProfile *gp = chihiro_profile();
    const uint32_t dev = gp->d3d.device_ptr ? MEM32(gp->d3d.device_ptr) : 0;
    if (!dev || !gp->d3d.kickoff)
        return 0;
    const uint32_t time = MEM32(dev + DEV_WRITE_TIME);    /* the time this fence releases */
    PUSH32(g_esp, 2);
    PUSH32(g_esp, gp->d3d.kickoff_ret);
    gp->d3d.kickoff();               /* ret 4 */
    return time;
}

/* The GPU's progress: the last fence time it released (0: no device yet) */
uint32_t chihiro_d3d_progress(void)
{
    const uint32_t sem = d3d_semaphore_va();
    return sem ? MEM32(sem) : 0;
}
