/*
 * host_stats.c - see host_stats.h. From the Lindbergh ports' runtime
 * (lindberghrecomp, runtime/host_stats.c; MIT), as in The House of the Dead 4 VR.
 *
 * Sources, all readable by an app (checked on the Quest 3 under its app
 * sandbox): the process's own CPU time (getrusage), its resident memory
 * (/proc/self/status), and the GPU through sysfs - Adreno's kgsl on Android
 * (/sys/class/kgsl/kgsl-3d0: gpu_busy_percentage, devfreq/cur_freq, temp),
 * else a devfreq device named for the GPU and a thermal zone whose type names
 * it (desktop Linux, e.g. the Steam Frame). Where neither gives the load (the
 * Frame's msm devfreq has no load file), it is the GPU time the DRM driver
 * counts for each open client in /proc/<pid>/fdinfo, summed over every client
 * we may read (see "DRM fdinfo" below). System-wide CPU (/proc/stat) is closed to apps
 * on the Quest, so CPU is the game's own use. The paths are found once;
 * VC3_STATS_ROOT prefixes them (for tests).
 */
#include "host_stats.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/syscall.h>

static volatile float g_game_ms = -1;
void lg_stats_set_game_ms(float ms) { g_game_ms = ms; }

static char g_root[256];
static char g_gpu_busy[320], g_gpu_freq[320], g_gpu_temp[320];
static int g_busy_kgsl;         /* g_gpu_busy is kgsl's "N %" (else devfreq's "N" or "N@...") */
static int g_found;

static int read_line(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = fgets(buf, (int)n, f) != NULL;
    fclose(f);
    return ok;
}

static int readable(const char *path) { char b[64]; return read_line(path, b, sizeof b); }

static int readable_dir(const char *p)
{
    int f = open(p, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (f >= 0) close(f);
    return f >= 0;
}

static void path(char *out, size_t n, const char *rest) { snprintf(out, n, "%s%s", g_root, rest); }

/*
 * GPU load from DRM fdinfo (Linux; used on the Steam Frame). Each open DRM
 * file shows, in /proc/<pid>/fdinfo/<fd>, its drm-client-id and the GPU time
 * spent on its work ("drm-engine-gpu: <ns> ns" on msm). The sum of those
 * times over all clients, per second of wall time, is the GPU's busy share.
 *
 * We can read the fdinfo of our own user's processes: the game's own device,
 * SteamVR's compositor and server, Steam. Not seen: other users' processes
 * and non-dumpable ones (gamescope runs with CAP_SYS_NICE, so its /proc fd
 * list is root's), and the kernel's own clients. On the Frame those add
 * almost nothing while in VR: the sum matched the msm debugfs "load" (whole
 * GPU, but reset by every read, and only open to the perf group) within a
 * point or so, 30-70% in combat.
 *
 * It is not cheap, so it runs on a thread of its own, not on the caller's
 * (the XR frame loop, holding the game's queue lock): reading a client's
 * fdinfo walks its GEM objects (the Frame's ~45 clients take about 1 ms), and
 * finding the files means reading every fd link of every process (some 5000
 * steps on the Frame, 12 ms). So the thread samples twice a second while the
 * panel asks, and sweeps /proc a few hundred steps at a time, resumed where
 * it stopped and started over when done; the game's own fds come first.
 * Directories are read with getdents64 into fixed buffers, so a sample
 * allocates nothing.
 */
#define FD_CLIENTS 128          /* clients followed; the Frame has some 45 */
#define FD_STEPS 512            /* sweep steps per sample (a process, or one fd link) */

typedef struct { int fd, pos, len; long long buf[512]; } Dirs;     /* a directory, read a little at a time */
typedef struct { int pid, fd; unsigned id; long long ns; } FdClient;

static FdClient g_fdc[FD_CLIENTS];
static int g_nfd, g_fd_on, g_fdpid;
static Dirs g_procd = { .fd = -1 }, g_fdd = { .fd = -1 };       /* the sweep: /proc, and g_fdpid's fd/ */

static int dir_open(Dirs *d, const char *p)
{
    d->fd = open(p, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    d->pos = d->len = 0;
    return d->fd >= 0;
}

/* The next name (not "." or ".."), or NULL at the end, which closes it. */
static const char *dir_next(Dirs *d)
{
    struct ent { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; };
    for (;;) {
        if (d->pos >= d->len) {
            long n = d->fd < 0 ? -1 : syscall(SYS_getdents64, d->fd, d->buf, sizeof d->buf);
            if (n <= 0) { if (d->fd >= 0) close(d->fd); d->fd = -1; return NULL; }
            d->len = (int)n; d->pos = 0;
        }
        struct ent *e = (struct ent *)((char *)d->buf + d->pos);
        d->pos += e->reclen;
        if (e->name[0] != '.') return e->name;
    }
}

/* A DRM file's client id and GPU time (its first engine: msm has one, "gpu"). */
static int fdinfo_read(int pid, int fd, unsigned *id, long long *ns)
{
    char p[320], b[1024];
    snprintf(p, sizeof p, "%s/proc/%d/fdinfo/%d", g_root, pid, fd);
    int f = open(p, O_RDONLY | O_CLOEXEC);
    if (f < 0) return 0;
    ssize_t n = read(f, b, sizeof b - 1);
    close(f);
    if (n <= 0) return 0;
    b[n] = 0;
    const char *c = strstr(b, "\ndrm-client-id:"), *e = b;
    while ((e = strstr(e + 1, "\ndrm-engine-")) && !strncmp(e + 12, "capacity-", 9)) {}
    if (!c || !e || !(e = strchr(e, ':'))) return 0;
    *id = (unsigned)strtoul(c + 15, NULL, 10);
    *ns = strtoll(e + 1, NULL, 10);
    return 1;
}

static void fd_add(int pid, int fd)
{
    unsigned id;
    long long ns;
    if (g_nfd == FD_CLIENTS || !fdinfo_read(pid, fd, &id, &ns)) return;
    for (int i = 0; i < g_nfd; i++)
        if (g_fdc[i].id == id) return;         /* already followed (a dup'd fd, or the last sweep) */
    g_fdc[g_nfd++] = (FdClient){ pid, fd, id, ns };     /* counted from the next sample */
}

/* One step of the sweep; 0 when it has been through /proc (the next starts over). */
static int fd_step(void)
{
    char p[320], t[16];
    if (g_fdd.fd < 0) {                        /* on to the next process */
        if (g_procd.fd < 0) { path(p, sizeof p, "/proc"); if (!dir_open(&g_procd, p)) return 0; }
        const char *pid = dir_next(&g_procd);
        if (!pid) return 0;
        g_fdpid = atoi(pid);                   /* 0 for "self", "sys", ... */
        snprintf(p, sizeof p, "%s/proc/%d/fd", g_root, g_fdpid);
        if (g_fdpid > 0) dir_open(&g_fdd, p);  /* fails for processes we may not read */
        return 1;
    }
    const char *fd = dir_next(&g_fdd);
    if (!fd) return 1;
    snprintf(p, sizeof p, "%s/proc/%d/fd/%s", g_root, g_fdpid, fd);
    ssize_t n = readlink(p, t, sizeof t);
    if (n >= 9 && !memcmp(t, "/dev/dri/", 9)) fd_add(g_fdpid, atoi(fd));
    return 1;
}

static void fd_sweep(int steps) { while (steps-- > 0 && fd_step()) {} }

/* The GPU time of all followed clients since the last call, ns, and in *own
 * that of this process's; clients whose file is gone (closed, or the process
 * ended) are dropped. */
static long long fd_busy_ns(long long *own)
{
    long long sum = 0;
    const int self = (int)getpid();
    *own = 0;
    for (int i = 0; i < g_nfd;) {
        unsigned id;
        long long ns;
        if (!fdinfo_read(g_fdc[i].pid, g_fdc[i].fd, &id, &ns) || id != g_fdc[i].id) { g_fdc[i] = g_fdc[--g_nfd]; continue; }
        if (ns > g_fdc[i].ns) {
            sum += ns - g_fdc[i].ns;
            if (g_fdc[i].pid == self) *own += ns - g_fdc[i].ns;
        }
        g_fdc[i++].ns = ns;
    }
    return sum;
}

static double now_s(void);
static volatile int g_fd_pct = -1;      /* the thread's latest load, % */
static volatile int g_fd_own_pct = -1;  /* of it, this process's */
static volatile long g_fd_asked_ms;     /* when lg_stats_sample last read it; the thread rests when that is old */

static void *fd_thread(void *arg)
{
    char p[320];
    double wall0 = -1;
    (void)arg;
    setpriority(PRIO_PROCESS, 0, 10);          /* this thread only (Linux): behind the game's */
    g_fdpid = (int)getpid();
    snprintf(p, sizeof p, "%s/proc/%d/fd", g_root, g_fdpid);
    if (dir_open(&g_fdd, p)) while (g_fdd.fd >= 0) fd_step();
    for (;; usleep(500000)) {
        double wall = now_s();
        if (wall * 1000 - g_fd_asked_ms > 2000) { wall0 = -1; g_fd_pct = g_fd_own_pct = -1; continue; }   /* panel hidden */
        long long own, ns = fd_busy_ns(&own);
        double pct = wall0 >= 0 && wall > wall0 ? ns / ((wall - wall0) * 1e7) : -1;
        double own_pct = wall0 >= 0 && wall > wall0 ? own / ((wall - wall0) * 1e7) : -1;
        g_fd_pct = pct < 0 || !g_nfd ? -1 : pct > 100 ? 100 : (int)(pct + .5);
        g_fd_own_pct = own_pct < 0 || !g_nfd ? -1 : own_pct > 100 ? 100 : (int)(own_pct + .5);
        wall0 = wall;
        fd_sweep(FD_STEPS);
    }
    return NULL;
}

static void find_sources(void)
{
    const char *r = getenv("VC3_STATS_ROOT");
    snprintf(g_root, sizeof g_root, "%s", r ? r : "");
    char p[320];
    /* Adreno under Android's kgsl driver */
    path(p, sizeof p, "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage");
    if (readable(p)) { strcpy(g_gpu_busy, p); g_busy_kgsl = 1; }
    path(p, sizeof p, "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq");
    if (readable(p)) strcpy(g_gpu_freq, p);
    path(p, sizeof p, "/sys/class/kgsl/kgsl-3d0/temp");
    if (readable(p)) strcpy(g_gpu_temp, p);
    /* desktop Linux: a devfreq device for the GPU (msm: "*.gpu") */
    char dir[300]; path(dir, sizeof dir, "/sys/class/devfreq");
    DIR *d = (!g_gpu_freq[0] || !g_gpu_busy[0]) ? opendir(dir) : NULL;
    for (struct dirent *e; d && (e = readdir(d));) {
        if (!strstr(e->d_name, "gpu")) continue;
        if (!g_gpu_freq[0]) { snprintf(p, sizeof p, "%s/%s/cur_freq", dir, e->d_name); if (readable(p)) strcpy(g_gpu_freq, p); }
        if (!g_gpu_busy[0]) {
            snprintf(p, sizeof p, "%s/%s/gpu_load", dir, e->d_name);
            if (!readable(p)) snprintf(p, sizeof p, "%s/%s/load", dir, e->d_name);
            if (readable(p)) strcpy(g_gpu_busy, p);
        }
    }
    if (d) closedir(d);
    /* a thermal zone that says it is the GPU's */
    path(dir, sizeof dir, "/sys/class/thermal");
    d = !g_gpu_temp[0] ? opendir(dir) : NULL;
    for (struct dirent *e; d && (e = readdir(d));) {
        if (strncmp(e->d_name, "thermal_zone", 12)) continue;
        char type[64] = "";
        snprintf(p, sizeof p, "%s/%s/type", dir, e->d_name);
        if (!read_line(p, type, sizeof type) || !strstr(type, "gpu")) continue;
        snprintf(p, sizeof p, "%s/%s/temp", dir, e->d_name);
        if (readable(p)) { strcpy(g_gpu_temp, p); break; }
    }
    if (d) closedir(d);
    /* no load file: DRM fdinfo, if we can read our own */
    snprintf(p, sizeof p, "%s/proc/%d/fd", g_root, (int)getpid());
    if (!g_gpu_busy[0] && readable_dir(p)) {
        pthread_t th;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);      /* an ordinary thread, whatever the caller's class */
        pthread_attr_setschedpolicy(&a, SCHED_OTHER);
        g_fd_on = pthread_create(&th, &a, fd_thread, NULL) == 0;
        pthread_attr_destroy(&a);
    }
    g_found = 1;
}

static long read_long(const char *p)
{
    char b[64];
    if (!p[0] || !read_line(p, b, sizeof b)) return -1;
    return strtol(b, NULL, 10);     /* "45 %", "45", "45@587000000Hz" all lead with the number */
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void lg_stats_sample(LgStats *s)
{
    static double cpu0 = -1, wall0;
    if (!g_found) find_sources();

    struct rusage ru;
    double cpu = -1, wall = now_s();
    if (getrusage(RUSAGE_SELF, &ru) == 0)
        cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    s->cpu_pct = cpu >= 0 && cpu0 >= 0 && wall > wall0 && cores > 0
                 ? (float)((cpu - cpu0) / (wall - wall0) / cores * 100.0) : -1;
    cpu0 = cpu; wall0 = wall;
    s->game_ms = g_game_ms;

    if (g_fd_on) {
        g_fd_asked_ms = (long)(wall * 1000);
        s->gpu_pct = g_fd_pct;
        s->gpu_own_pct = g_fd_own_pct;
    } else {
        s->gpu_own_pct = -1;
        long busy = read_long(g_gpu_busy);
        s->gpu_pct = busy < 0 ? -1 : (int)(g_busy_kgsl || busy <= 100 ? busy : busy / 10);   /* devfreq load may be per mille */
    }
    long hz = read_long(g_gpu_freq);
    s->gpu_mhz = hz <= 0 ? -1 : (int)(hz >= 1000000 ? hz / 1000000 : hz);                 /* Hz, or already MHz */
    long t = read_long(g_gpu_temp);
    s->gpu_c = t < 0 ? -1 : (int)(t >= 1000 ? (t + 500) / 1000 : t);                     /* milli-Celsius */

    s->rss_mb = -1;
    char p[320], line[128];
    path(p, sizeof p, "/proc/self/status");
    FILE *f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "VmRSS:", 6)) { s->rss_mb = (int)(strtol(line + 6, NULL, 10) / 1024); break; }
    if (f) fclose(f);
}

/* "-" where a value is unknown */
static const char *num(char *b, size_t n, const char *fmt, double v, int known)
{
    if (known) snprintf(b, n, fmt, v); else snprintf(b, n, "-");
    return b;
}

void lg_stats_format(const LgStats *s, char lines[LG_STATS_LINES][LG_STATS_LINE])
{
    char a[16], b[16], c[16], d[16];
    snprintf(lines[0], LG_STATS_LINE, "%s fps  %s ms", num(a, sizeof a, "%.0f", s->fps, s->fps >= 0),
             num(b, sizeof b, "%.1f", s->frame_ms, s->frame_ms > 0));
    snprintf(lines[1], LG_STATS_LINE, "CPU  game %s%%  main %s ms", num(a, sizeof a, "%.0f", s->cpu_pct, s->cpu_pct >= 0),
             num(b, sizeof b, "%.1f", s->game_ms, s->game_ms >= 0));
    if (s->gpu_pct < 0 && s->gpu_mhz <= 0 && s->gpu_c < 0) snprintf(lines[2], LG_STATS_LINE, "GPU  n/a");
    else if (s->gpu_own_pct >= 0)   /* the whole GPU and the game's own share (the panel fits ~26 characters) */
        snprintf(lines[2], LG_STATS_LINE, "GPU  %s%%  game %s%%  %sC",
                 num(a, sizeof a, "%.0f", s->gpu_pct, s->gpu_pct >= 0), num(d, sizeof d, "%.0f", s->gpu_own_pct, 1),
                 num(c, sizeof c, "%.0f", s->gpu_c, s->gpu_c >= 0));
    else snprintf(lines[2], LG_STATS_LINE, "GPU  %s%%  %s MHz  %sC", num(a, sizeof a, "%.0f", s->gpu_pct, s->gpu_pct >= 0),
                  num(b, sizeof b, "%.0f", s->gpu_mhz, s->gpu_mhz > 0), num(c, sizeof c, "%.0f", s->gpu_c, s->gpu_c >= 0));
    snprintf(lines[3], LG_STATS_LINE, "MEM %s MB  %sx%s%s", num(a, sizeof a, "%.0f", s->rss_mb, s->rss_mb >= 0),
             num(b, sizeof b, "%.0f", s->eye_w, s->eye_w > 0), num(d, sizeof d, "%.0f", s->eye_h, s->eye_h > 0),
             s->scale_pct > 0 && s->scale_pct < 100
                 ? (snprintf(c, sizeof c, " %d%%", s->scale_pct), c) : "");
}
