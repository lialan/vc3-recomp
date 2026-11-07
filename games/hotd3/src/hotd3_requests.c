/*
 * HOTD3's request thread (0x7AF10), without spinning a host core while a
 * request is pending.
 *
 * The thread serves a queue of requests (loads and the like) through
 * sub_0007A480, which runs the head request's handler under the request's
 * critical section and returns 0 when the queue is empty, 2 while the head
 * request is still pending, 3 to stop. The routine:
 *
 *     7AF15: mov byte [2D9442],1
 *     7AF20: ecx=self; call 7A480
 *            0: ResetEvent / WaitForSingleObject([self+D04C], -1), then
 *               sub_00135C10 three times; again
 *            3: mov byte [2D9442],0; return 0 (ret 4)
 *            else (2): again at once
 *
 * On the console's one CPU the "again at once" spun only while nothing else
 * could run. Here the thread has a host core of its own, and on the Quest it
 * retried a pending request up to a million times a second -- each a guest
 * critical section taken and released, the thread's whole core spent -- most
 * in the seconds the game ran slow (KCALL times: 946,000 lock calls a second
 * in seconds under 50 frames, 115,000 in seconds at 58+).
 *
 * The same routine, sleeping HOTD3_REQUEST_SLEEP_US (default 100) between
 * retries of a pending request: the handlers poll work other threads finish
 * (file reads, decodes), so a retry 0.1 ms later sees the same progress.
 * HOTD3_REQUEST_SPIN=1 retries at once, as before.
 */
#include <stdint.h>
#include <stdlib.h>

#include "recomp_types.h"
#include "recomp_funcs.h"
#include "host_time.h"
#include "chihiro/ch_env.h"

#define REQ_RUNNING  0x2D9442u   /* byte: the thread is serving */
#define REQ_EVENT    0xD04Cu     /* +: the event a new request sets */

void hotd3_request_thread(void)
{
    static int spin = -1;
    static uint64_t sleep_ns;
    if (spin < 0) {
        const char *e = ch_env("REQUEST_SPIN");
        const char *us = ch_env("REQUEST_SLEEP_US");
        spin = e && *e == '1';
        sleep_ns = (uint64_t)(us ? strtoul(us, NULL, 10) : 100) * 1000u;
    }
    PUSH32(g_esp, g_esi);
    uint32_t self = MEM32(g_esp + 8);   /* the argument, past the return address and esi */
    g_esi = self;
    MEM8(REQ_RUNNING) = 1;
    for (;;) {
        g_ecx = self;
        PUSH32(g_esp, 0x0007AF27u);
        sub_0007A480();
        if (g_eax == 0) {                       /* queue empty: wait for a request */
            PUSH32(g_esp, MEM32(self + REQ_EVENT));
            PUSH32(g_esp, 0x0007AF37u);
            sub_000B81F1();
            PUSH32(g_esp, 0xFFFFFFFFu);
            PUSH32(g_esp, MEM32(self + REQ_EVENT));
            PUSH32(g_esp, 0x0007AF45u);
            sub_000B8326();
            PUSH32(g_esp, 0x0007AF4Au); sub_00135C10();
            PUSH32(g_esp, 0x0007AF4Fu); sub_00135C10();
            PUSH32(g_esp, 0x0007AF54u); sub_00135C10();
            continue;
        }
        if (g_eax == 3)
            break;
        if (!spin && sleep_ns)                  /* 2: the head request is pending */
            xbox_HostSleepNs(sleep_ns);
    }
    MEM8(REQ_RUNNING) = 0;
    g_eax = 0;
    POP32(g_esp, g_esi);
    g_esp += 8;                                 /* ret 4 */
}
