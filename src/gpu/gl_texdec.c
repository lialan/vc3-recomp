/*
 * Texture uploads: what they cost (VC3_TEX_STATS), and their decode on the
 * GPU.
 */
#include "gl_texdec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Counters (VC3_TEX_STATS=1) ─────────────────────────────── */

int g_texstat_on;

static struct {
    uint64_t n[3][256], texels[3][256], dec_ns[3][256], gl_ns[3][256], max_ns[3][256];
    uint64_t hash_ns, hashes;
} s_ts;

uint64_t texstat_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void texstat_init(void)
{
    const char *e = getenv("VC3_TEX_STATS");
    g_texstat_on = e && *e && *e != '0';
}

void texstat_add(uint32_t fmt, int path, uint64_t texels, uint64_t dec_ns, uint64_t gl_ns)
{
    fmt &= 0xFF;
    s_ts.n[path][fmt]++;
    s_ts.texels[path][fmt] += texels;
    s_ts.dec_ns[path][fmt] += dec_ns;
    s_ts.gl_ns[path][fmt] += gl_ns;
    if (dec_ns + gl_ns > s_ts.max_ns[path][fmt])
        s_ts.max_ns[path][fmt] = dec_ns + gl_ns;
}

void texstat_hash(uint64_t ns)
{
    s_ts.hash_ns += ns;
    s_ts.hashes++;
}

/* Every 300 flips: the run so far, per path and format. */
void texstat_flip(uint32_t frame)
{
    static const char *const PATH[3] = { "cpu", "gpu", "native" };
    uint64_t n = 0, dec = 0, gl = 0, mx = 0;
    int p, f;

    if (!g_texstat_on || frame % 300)
        return;
    fprintf(stderr, "[TEXSTAT] flip %u: hash %.2f ms over %llu checks\n", frame,
            s_ts.hash_ns / 1e6, (unsigned long long)s_ts.hashes);
    for (p = 0; p < 3; p++)
        for (f = 0; f < 256; f++) {
            if (!s_ts.n[p][f])
                continue;
            fprintf(stderr, "[TEXSTAT]   %-6s fmt %02X: %5llu uploads %8.2f Mtexels"
                    " decode %8.2f ms  gl %7.2f ms  max %6.2f ms\n", PATH[p], f,
                    (unsigned long long)s_ts.n[p][f], s_ts.texels[p][f] / 1e6,
                    s_ts.dec_ns[p][f] / 1e6, s_ts.gl_ns[p][f] / 1e6,
                    s_ts.max_ns[p][f] / 1e6);
            n += s_ts.n[p][f];
            dec += s_ts.dec_ns[p][f];
            gl += s_ts.gl_ns[p][f];
            if (s_ts.max_ns[p][f] > mx)
                mx = s_ts.max_ns[p][f];
        }
    fprintf(stderr, "[TEXSTAT]   total: %llu uploads, decode %.2f ms, gl %.2f ms, max %.2f ms\n",
            (unsigned long long)n, dec / 1e6, gl / 1e6, mx / 1e6);
}
