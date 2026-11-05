/*
 * SS1_DIAG=1: every 2 s, D3D's view of the GPU -- the progress semaphore,
 * the last kick's marker the GPU latched (PGRAPH 0x400B10), the device's
 * ring pointers -- for diagnosing D3D waits that do not end. Bring-up only.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "recomp_types.h"
#include "chihiro/ch_env.h"

volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset);

#define G_DEVICE 0x00105D28u

static void *diag_thread(void *arg)
{
    (void)arg;
    for (;;) {
        sleep(2);
        uint32_t dev = MEM32(G_DEVICE);
        if (!dev) continue;
        uint32_t sem_va = MEM32(dev + 0x30);
        uint32_t pads = MEM32(0x01CE4D88);      /* the game's input object */
        volatile uint32_t *latch = xbox_Nv2aRegPtr(0x400B10);
        volatile uint32_t *put = xbox_Nv2aRegPtr(0x800040);
        volatile uint32_t *get = xbox_Nv2aRegPtr(0x800044);
        fprintf(stderr, "[SS1 diag] dev %08X put %08X end %08X time %08X sem@%08X=%08X latch %08X "
                        "dma put %08X get %08X | flags %08X regs@%08X g105880 %08X fake put %08X get %08X"
                        " | frame %u fences %08X %08X"
                        " | pads mask %08X handles %08X %08X %08X %08X\n",
                dev, MEM32(dev), MEM32(dev + 4), MEM32(dev + 0x2C), sem_va,
                sem_va ? MEM32(sem_va) : 0, latch ? *latch : 0,
                put ? *put : 0, get ? *get : 0,
                MEM32(dev + 8), MEM32(dev + 0x1C10), MEM32(0x105880),
                MEM32(0x105870), MEM32(0x105874),
                MEM32(dev + 0x2468), MEM32(dev + 0x1964), MEM32(dev + 0x1968),
                pads ? MEM32(pads + 0xE4) : 0, pads ? MEM32(pads + 0xC8) : 0,
                pads ? MEM32(pads + 0xCC) : 0, pads ? MEM32(pads + 0xD0) : 0,
                pads ? MEM32(pads + 0xD4) : 0);
        {
            /* D3D's ring: start/end, lap size, segment sizes; each fence's
             * pushbuffer address (table of 64 at +0x60: time, address) */
            uint32_t sem = sem_va ? MEM32(sem_va) : 0, head = MEM32(dev + 0x2C);
            fprintf(stderr, "[SS1 diag]   ring %08X-%08X lap %08X seg %08X/%08X dma put %08X\n",
                    MEM32(dev + 0x24), MEM32(dev + 0x28), MEM32(dev + 0x40),
                    MEM32(0x001084C8), MEM32(0x001084CC), put ? *put : 0);
            for (uint32_t t = sem; t != head && head - t <= 0x40; t += 2) {
                uint32_t slot = dev + 0x60 + ((t >> 1) & 0x3F) * 8;
                fprintf(stderr, "[SS1 diag]   fence %08X at %08X (slot time %08X)\n",
                        t, MEM32(slot + 4), MEM32(slot));
            }
        }
        {
            /* XAPI's device nodes: count, array, 0x16 bytes each (flags at +4:
             * 1 in use, 8 ready, 2 removing; class at +0xE) */
            uint32_t count = MEM8(0x01CE8200) | (MEM8(0x01CE8201) << 8);
            uint32_t arr = MEM32(0x01CE8204);
            for (uint32_t i = 0; arr && i < count && i < 6; i++) {
                uint32_t node = arr + i * 0x16;
                uint32_t d = MEM32(node), c = MEM32(node + 0xE);
                fprintf(stderr, "[SS1 diag]   node %u: dev %08X flags %02X class %08X"
                        " | dev port %08X | class type %08X mask %08X chg %08X seen %08X"
                        " | table %08X %08X\n",
                        i, d, MEM8(node + 4), c, d ? MEM32(d + 0x14) : 0,
                        c ? MEM32(c + 4) : 0, c ? MEM32(c) : 0, c ? MEM32(c + 4) : 0,
                        c ? MEM32(c + 8) : 0, MEM32(0x00146A04), MEM32(0x00146A08));
            }
        }
    }
    return NULL;
}

void ss1_diag_start(void)
{
    const char *e = ch_env("DIAG");
    pthread_t t;
    if (!e || *e != '1')
        return;
    if (pthread_create(&t, NULL, diag_thread, NULL) == 0)
        pthread_detach(t);
}
