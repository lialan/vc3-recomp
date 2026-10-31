/*
 * Software JVS I/O board for the Chihiro baseboard HLE.
 *
 * The game builds JVS request frames itself and hands them to the SEGA
 * library's JvsNodeSendPacket (0x13E110); replies come back through
 * JvsNodeReceivePacket (0x13E030). Both are replaced here (named in
 * src/recomp_manual.c), and the frames are answered by an emulated I/O board
 * written from the JVS specification: identify, revisions, feature list,
 * switches, coins, analog and screen-position inputs, outputs.
 *
 * Baseboard packet formats (from the game's wrappers 0x104C70 / 0x104D80):
 *   send:    [seq][count] then per frame [flag][E0 node len payload.. sum]
 *   receive: [seq][count] then per node  [node][status=0][len lo][len hi]
 *            [E0 00 len 01 reports.. sum]
 * The frames are unescaped: the baseboard does the wire's E0/D0 escaping.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "chihiro.h"
#include "chihiro/ch_env.h"

#define STDCALL_ARG(n)  MEM32(g_esp + 4 + 4 * (n))

#define JVS_SYNC        0xE0
#define JVS_BROADCAST   0xFF
#define JVS_MASTER      0x00
#define JVS_STATUS_OK   0x01
#define JVS_REPORT_OK   0x01
#define JVS_REPORT_UNSUPPORTED 0x02  /* "parameter error" */

#define JVS_PLAYERS     2
#define JVS_SWITCH_BYTES 2           /* per player */
#define JVS_COIN_SLOTS  2
#define JVS_ANALOG_CH   8
#define JVS_SCREEN_CH   2

/* Identifies as a generic Sega-compatible I/O board; the game only checks
 * the feature list against its spec. */
static const char k_ident[] = "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10";

static uint8_t s_addr;               /* 0 until the master assigns one */
static chihiro_input_t s_in;
static uint16_t s_coins[2];         /* owned by the board: inserted minus spent */

/* Replies waiting for the next receive: each a full frame. */
#define MAX_REPLIES 8
static struct { uint8_t node; uint16_t len; uint8_t data[256]; } s_reply[MAX_REPLIES];
static int s_nreply;

static int s_log_budget = 200;
static int s_trace = -1;

static int jvs_trace(void)
{
    if (s_trace < 0) {
        const char *v = ch_env("JVS_TRACE");
        s_trace = v && *v && *v != '0';
    }
    return s_trace;
}

void chihiro_input_set(const chihiro_input_t *in)
{
    s_in = *in;
}

void chihiro_coin_insert(int slot)
{
    if (slot >= 0 && slot < JVS_COIN_SLOTS && s_coins[slot] < 0x3FFF)
        s_coins[slot]++;
}

/* ── Reply construction ──────────────────────────────────── */

typedef struct { uint8_t buf[256]; int n; } out_t;

static void put(out_t *o, uint8_t b)
{
    if (o->n < (int)sizeof(o->buf))
        o->buf[o->n++] = b;
}

static void put16be(out_t *o, uint16_t v)
{
    put(o, (uint8_t)(v >> 8));
    put(o, (uint8_t)v);
}

/* Switch bytes for one player: start, service, up, down, left, right,
 * push1..push10. */
static void put_player_switches(out_t *o, int p, int nbytes)
{
    uint16_t sw = s_in.player[p];
    int i;

    for (i = 0; i < nbytes; i++)
        put(o, i < 2 ? (uint8_t)(sw >> (8 * (1 - i))) : 0);
}

/* Handle one command at payload[*pos]; append its report. Returns 0 on an
 * unknown command (the rest of the frame cannot be parsed). */
static int do_command(const uint8_t *p, int len, int *pos, out_t *o, int *reset)
{
    uint8_t cmd = p[*pos];
    int i, n;

    switch (cmd) {
    case 0xF0:                       /* reset: D9; no reply */
        *pos += 2;
        s_addr = 0;
        *reset = 1;
        return 1;
    case 0xF1:                       /* set address */
        /* Only the first unaddressed board in the chain takes it. Once this
         * board has an address the broadcast goes downstream, where nothing
         * answers: that silence is how the master learns the chain has one
         * board. */
        *pos += 2;
        if (s_addr) {
            *reset = 1;              /* reuse "no reply" */
            return 1;
        }
        s_addr = p[*pos - 1];
        put(o, JVS_REPORT_OK);
        return 1;
    case 0x10:                       /* identify */
        *pos += 1;
        put(o, JVS_REPORT_OK);
        for (i = 0; i < (int)sizeof(k_ident); i++)
            put(o, (uint8_t)k_ident[i]);
        return 1;
    case 0x11: *pos += 1; put(o, JVS_REPORT_OK); put(o, 0x13); return 1; /* cmd rev 1.3 */
    case 0x12: *pos += 1; put(o, JVS_REPORT_OK); put(o, 0x30); return 1; /* JVS rev 3.0 */
    case 0x13: *pos += 1; put(o, JVS_REPORT_OK); put(o, 0x10); return 1; /* comm ver 1.0 */
    case 0x14:                       /* feature check */
        *pos += 1;
        put(o, JVS_REPORT_OK);
        put(o, 0x01); put(o, JVS_PLAYERS); put(o, 13); put(o, 0);       /* switches */
        put(o, 0x02); put(o, JVS_COIN_SLOTS); put(o, 0); put(o, 0);     /* coins */
        put(o, 0x03); put(o, JVS_ANALOG_CH); put(o, 10); put(o, 0);     /* analog */
        put(o, 0x06); put(o, 16); put(o, 16); put(o, JVS_SCREEN_CH);    /* screen pos */
        put(o, 0x12); put(o, 6); put(o, 0); put(o, 0);                  /* outputs */
        put(o, 0x00);
        return 1;
    case 0x15:                       /* master ID: ignore the string */
        for (*pos += 1; *pos < len && p[*pos]; (*pos)++)
            ;
        *pos += 1;
        put(o, JVS_REPORT_OK);
        return 1;
    case 0x20:                       /* switch inputs: players, bytes each */
        host_input_poll(&s_in);      /* first command of every poll frame */
        n = p[*pos + 1];
        i = p[*pos + 2];
        *pos += 3;
        /* VC3 asks for "2 players, 0 bytes" but sizes the reply it splits
         * (0x104470, table at node+0x13AD) from the feature list's switch
         * count. Answer with the advertised bytes, or every report after
         * this one is read at the wrong offset and counted as an error. */
        if (i == 0)
            i = JVS_SWITCH_BYTES;
        put(o, JVS_REPORT_OK);
        put(o, s_in.test ? 0x80 : 0x00);
        for (int pl = 0; pl < n; pl++)
            put_player_switches(o, pl < JVS_PLAYERS ? pl : 0, i);
        return 1;
    case 0x21:                       /* coin counters */
        n = p[*pos + 1];
        *pos += 2;
        put(o, JVS_REPORT_OK);
        for (i = 0; i < n; i++)
            put16be(o, i < JVS_COIN_SLOTS ? s_coins[i] : 0);
        return 1;
    case 0x22:                       /* analog: channels, 16-bit big-endian */
        n = p[*pos + 1];
        *pos += 2;
        put(o, JVS_REPORT_OK);
        for (i = 0; i < n; i++)
            put16be(o, i < JVS_ANALOG_CH ? s_in.analog[i] : 0);
        return 1;
    case 0x25:                       /* screen position: channel index */
        i = p[*pos + 1];
        *pos += 2;
        put(o, JVS_REPORT_OK);
        i = i >= 1 && i <= JVS_SCREEN_CH ? i - 1 : 0;
        put16be(o, s_in.screen_x[i]);
        put16be(o, s_in.screen_y[i]);
        return 1;
    case 0x2F:                       /* retransmit: answer again */
        *pos += 1;
        put(o, JVS_REPORT_OK);
        return 1;
    case 0x30:                       /* coin decrease: slot, amount */
        i = p[*pos + 1];
        n = p[*pos + 2] << 8 | p[*pos + 3];
        *pos += 4;
        if (i >= 1 && i <= JVS_COIN_SLOTS)
            s_coins[i - 1] = s_coins[i - 1] > n ? (uint16_t)(s_coins[i - 1] - n) : 0;
        put(o, JVS_REPORT_OK);
        return 1;
    case 0x32:                       /* general outputs: n bytes */
        n = p[*pos + 1];
        *pos += 2 + n;
        put(o, JVS_REPORT_OK);
        return 1;
    case 0x33:                       /* analog outputs: channels * 2 bytes */
        n = p[*pos + 1];
        *pos += 2 + 2 * n;
        put(o, JVS_REPORT_OK);
        return 1;
    case 0x35:                       /* coin increase: slot, amount */
        *pos += 4;
        put(o, JVS_REPORT_OK);
        return 1;
    default:
        if (s_log_budget > 0) {
            s_log_budget--;
            fprintf(stderr, "[JVS] unsupported command 0x%02X\n", cmd);
        }
        put(o, JVS_REPORT_UNSUPPORTED);
        return 0;
    }
}

/* One request frame [E0 node len payload.. sum]: queue the reply. */
static void handle_frame(const uint8_t *f, int avail)
{
    uint8_t node, len;
    out_t o = { { 0 }, 0 };
    int pos = 0, reset = 0, i;
    uint8_t sum;

    if (avail < 4 || f[0] != JVS_SYNC)
        return;
    node = f[1];
    len = f[2];
    if (len < 1 || len + 3 > avail)
        return;
    if (node != JVS_BROADCAST && node != s_addr)
        return;                      /* addressed to another node */

    if (jvs_trace()) {
        fprintf(stderr, "[JVS] -> node %02X:", node);
        for (i = 0; i < len - 1; i++)
            fprintf(stderr, " %02X", f[3 + i]);
        fprintf(stderr, "\n");
    }

    while (pos < len - 1)
        if (!do_command(f + 3, len - 1, &pos, &o, &reset))
            break;
    if (reset && o.n == 0)
        return;                      /* reset is not answered */
    if (s_nreply >= MAX_REPLIES)
        return;

    {
        uint8_t *r = s_reply[s_nreply].data;
        int rl = o.n + 2;            /* status + reports + sum */
        r[0] = JVS_SYNC;
        r[1] = JVS_MASTER;
        r[2] = (uint8_t)rl;
        r[3] = JVS_STATUS_OK;
        memcpy(r + 4, o.buf, o.n);
        sum = 0;
        for (i = 1; i < 4 + o.n; i++)
            sum += r[i];
        r[4 + o.n] = sum;
        /* A broadcast's reply belongs to slot 0: that is where the game's
         * reply handler (0x104470) looks for the F1 answer before it takes
         * the assigned address as the node count. */
        s_reply[s_nreply].node = node == JVS_BROADCAST ? 0 : node;
        s_reply[s_nreply].len = (uint16_t)(5 + o.n);
        if (jvs_trace()) {
            fprintf(stderr, "[JVS] <- %d bytes:", 5 + o.n);
            for (i = 3; i < 4 + o.n && i < 40; i++)
                fprintf(stderr, " %02X", r[i]);
            fprintf(stderr, "\n");
        }
        s_nreply++;
    }
}

/* JvsNodeSendPacket(u8 *buf, u32 len, u32 flags), stdcall. */
void chihiro_jvs_send(void)
{
    const uint8_t *b = (const uint8_t *)XBOX_PTR(STDCALL_ARG(0));
    int len = (int)STDCALL_ARG(1);
    int count, pos = 2, i;

    if (jvs_trace() && STDCALL_ARG(0) && s_log_budget > 0) {
        s_log_budget--;
        fprintf(stderr, "[JVS] send len=%d flags=0x%X:", len, STDCALL_ARG(2));
        for (i = 0; i < len && i < 48; i++)
            fprintf(stderr, " %02X", b[i]);
        fprintf(stderr, "\n");
    }
    if (len >= 2 && STDCALL_ARG(0)) {
        count = b[1];
        for (i = 0; i < count && pos + 4 <= len; i++) {
            const uint8_t *f = b + pos + 1;          /* skip the flag byte */
            handle_frame(f, len - pos - 1);
            pos += 1 + 3 + f[2];                      /* flag + header + len */
        }
    }
    g_eax = 0;
    g_esp += 16;
}

/* JvsNodeReceivePacket(u8 *buf, u32 *len, u32 flags), stdcall. */
void chihiro_jvs_receive(void)
{
    uint32_t buf = STDCALL_ARG(0);
    uint32_t lenp = STDCALL_ARG(1);
    uint8_t *b = (uint8_t *)XBOX_PTR(buf);
    int pos = 2, i;

    if (buf) {
        b[0] = 0;
        b[1] = (uint8_t)s_nreply;
        for (i = 0; i < s_nreply; i++) {
            b[pos + 0] = s_reply[i].node;
            b[pos + 1] = 0;
            b[pos + 2] = (uint8_t)s_reply[i].len;
            b[pos + 3] = (uint8_t)(s_reply[i].len >> 8);
            memcpy(b + pos + 4, s_reply[i].data, s_reply[i].len);
            pos += 4 + s_reply[i].len;
        }
    }
    if (lenp)
        MEM32(lenp) = (uint32_t)pos;
    s_nreply = 0;
    g_eax = 0;
    g_esp += 16;
}

/* 0x104F80: the JVS sense line, read through the baseboard (JVS_SendCommand
 * 0x15, then status bits 0..1 == 3). The game's JVS state machine (0xF5780)
 * wants it asserted after a reset (state 3; otherwise "I/O board is not
 * connected") and keeps assigning addresses while it stays asserted
 * (state 4). A board asserts it until it has an address, so: 1 while this
 * board is unaddressed. cdecl, no args. */
void chihiro_jvs_link_up(void)
{
    g_eax = s_addr == 0;
    g_esp += 4;
}
