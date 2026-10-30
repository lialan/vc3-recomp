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

#include "recomp_types.h"
#include "recomp_funcs.h"
#include "host_time.h"

void nv2a_pb_set_semaphore_target(uint32_t guest_va);

#define D3D_DEVICE_PTR   0x001178F8u   /* g_pDevice */
#define DEV_WRITE_TIME   0x2C          /* newest fence time */
#define DEV_SEMAPHORE    0x30          /* -> GPU's progress */
#define WAIT_BOUND_NS    250000000ull

void vc3_d3d_block_on_time(void)
{
    static uint32_t s_sem_va;
    static int s_timeouts;
    uint32_t time = MEM32(g_esp + 4);
    uint32_t dev = MEM32(D3D_DEVICE_PTR);
    uint32_t sem, head;
    uint64_t deadline;

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
        PUSH32(g_esp, 0x0010CAEFu);
        sub_0010CA10();              /* KickOff(0), ret 4 */
    }

    /* Done once the fence is no newer than the GPU's progress, in ring order
     * relative to the newest time (the original's unsigned comparison). */
    deadline = xbox_HostNowNs() + WAIT_BOUND_NS;
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
    }
out:
    g_eax = 0;
    g_esp += 12;                     /* return address + 2 args */
}
