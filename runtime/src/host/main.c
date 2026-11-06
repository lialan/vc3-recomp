/*
 * Virtua Cop 3 (Sega Chihiro, 2003) - static recompilation host entry.
 *
 * POSIX hosts (macOS arm64 first). Boots the lifted vc3.xbe on the xboxrecomp
 * runtime with the Chihiro's 128 MB RAM layout.
 *
 *   vc3 [game_dir]     game_dir holds vc3.xbe and media/; default $VC3_GAME_DIR
 *
 * Saves (the partition1 TDATA/UDATA trees and the HLE'd baseboard EEPROM and
 * backup RAM) go to $VC3_SAVE_DIR, default ~/Library/Application Support/vc3-recomp.
 */

#if defined(_WIN32)
#error "vc3-recomp's host entry is POSIX-only; use the xboxrecomp template on Windows"
#endif

#if !defined(__APPLE__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE     /* glibc: dladdr, Dl_info, REG_* in ucontext */
#endif
#include "thread_acct.h"
#include <dlfcn.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>   /* _NSGetExecutablePath */
#include <mach/mach.h>      /* the crash report's page protections */
#include <mach/mach_vm.h>
#endif
#include <ctype.h>
#include <limits.h>
#include <sys/ucontext.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <xbox/xboxrecomp.h>
#include "recomp_types.h"
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "display_settings.h"
#include "chihiro/ch_env.h"
#include "chihiro/chihiro_profile.h"

extern void xbe_entry_point(void);
extern void xbox_path_init(const char *game_dir, const char *save_dir);
extern void chihiro_init(const char *save_dir);
extern void xbox_OhciInit(void);                 /* toolkit src/usb/ohci.c */
extern void chihiro_d3d_init(void);              /* chihiro/d3d_fence.c */
extern void vc3_gl_backend_register(void);   /* src/gpu/gl_backend.c */
#if defined(CHIHIRO_RENDERER_VULKAN)
#include "gpu/vulkan/vk_runtime.h"
#endif

typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;

/* ── Fault handler ─────────────────────────────────────────── */

static void dump_guest(uintptr_t fault)
{
    ptrdiff_t off = xbox_GetMemoryOffset();
    const uint32_t *sp;
    int i, shown = 0;
    uint32_t k;

    fprintf(stderr, "[CRASH] fault at host %p (guest VA 0x%08X)\n",
            (void *)fault, (uint32_t)(fault - (uintptr_t)off));
    fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx);
    fprintf(stderr, "  esp=%08X esi=%08X edi=%08X\n", g_esp, g_esi, g_edi);
    if (!g_esp)
        return;
    sp = (const uint32_t *)((uintptr_t)off + g_esp);
    fprintf(stderr, "  guest return addresses (innermost first):\n");
    for (i = 0; i < 256 && shown < 24; i++) {
        if (sp[i] > g_xbox_code_lo && sp[i] < g_xbox_code_hi) {
            fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, sp[i]);
            shown++;
        }
    }
    fprintf(stderr, "  recent ICALL targets:");
    for (k = 0; k < 16; k++)
        fprintf(stderr, " %08X", g_icall_trace[(g_icall_trace_idx + k) & 15]);
    fprintf(stderr, "\n");
}

static struct sigaction s_old_segv, s_old_bus;

static void fault_handler(int sig, siginfo_t *si, void *ucv)
{
    uintptr_t fault = (uintptr_t)si->si_addr;

#if defined(CHIHIRO_RENDERER_VULKAN)
    if(ch_vk_runtime_write_fault(fault,ucv)) return;
#endif

    /* Trapped hardware registers (APU, AC'97, OHCI): decoded and stepped over. */
    if (xbox_PosixMmioFault(ucv, fault))
        return;

    dump_guest(fault);
    {
        /* The host PC names the lifted function (sub_XXXXXXXX) or runtime
         * routine that faulted. */
        uintptr_t pc = 0, lr = 0;
        Dl_info di;
#if defined(__APPLE__) && defined(__aarch64__)
        pc = (uintptr_t)((ucontext_t *)ucv)->uc_mcontext->__ss.__pc;
        lr = (uintptr_t)((ucontext_t *)ucv)->uc_mcontext->__ss.__lr;
#elif defined(__linux__) && defined(__aarch64__)
        pc = (uintptr_t)((ucontext_t *)ucv)->uc_mcontext.pc;
        lr = (uintptr_t)((ucontext_t *)ucv)->uc_mcontext.regs[30];
#endif
        if (pc && dladdr((void *)pc, &di) && di.dli_sname)
            fprintf(stderr, "  host pc %p in %s+0x%lx\n", (void *)pc, di.dli_sname,
                    (unsigned long)(pc - (uintptr_t)di.dli_saddr));
        if (lr && dladdr((void *)lr, &di) && di.dli_sname)
            fprintf(stderr, "  host lr %p in %s+0x%lx\n", (void *)lr, di.dli_sname,
                    (unsigned long)(lr - (uintptr_t)di.dli_saddr));
        /* The signal's kind and the instruction with its registers: a fault
         * address of 0 with SIGBUS is often an ordered access (ldar/stlr) the
         * alignment fixup did not take, which the registers then show. */
        fprintf(stderr, "  signal %d code %d", sig, si->si_code);
        if (pc)
            fprintf(stderr, ", instruction %08X", *(const uint32_t *)pc);
        fprintf(stderr, "\n");
#if defined(__APPLE__) && defined(__aarch64__)
        {
            const __uint64_t *x = ((ucontext_t *)ucv)->uc_mcontext->__ss.__x;
            int r;
            for (r = 0; r < 29; r++)
                fprintf(stderr, "  x%-2d=%016llx%s", r, (unsigned long long)x[r], r % 4 == 3 ? "\n" : "");
            fprintf(stderr, "\n  memory base %p\n", (void *)xbox_GetMemoryOffset());
            /* The protection of the pages the faulting instruction's base
             * registers point at (a page another thread closed shows here) */
            for (r = 0; r < 29; r++) {
                mach_vm_address_t at = (mach_vm_address_t)x[r];
                mach_vm_size_t size = 0;
                vm_region_basic_info_data_64_t info;
                mach_msg_type_number_t n = VM_REGION_BASIC_INFO_COUNT_64;
                mach_port_t obj = MACH_PORT_NULL;
                if (x[r] < 0x100000000ull || x[r] >= 0x7000000000ull)
                    continue;           /* guest values, guest memory */
                if (mach_vm_region(mach_task_self(), &at, &size, VM_REGION_BASIC_INFO_64,
                                   (vm_region_info_t)&info, &n, &obj) == KERN_SUCCESS)
                    fprintf(stderr, "  x%d's region %llx+%llx prot %d max %d\n", r,
                            (unsigned long long)at, (unsigned long long)size,
                            info.protection, info.max_protection);
                if (obj != MACH_PORT_NULL)
                    mach_port_deallocate(mach_task_self(), obj);
            }
        }
#endif
    }
    /* Re-raise for whoever handled it before: the crash reporter, the
     * debugger, Android's debuggerd (which writes the tombstone). */
    sigaction(sig, sig == SIGBUS ? &s_old_bus : &s_old_segv, NULL);
}

static void install_fault_handler(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fault_handler;
    /* SA_NODEFER: a fault taken inside the handler is handled at once. A
     * trapped register's emulation writes guest memory (the USB controller's
     * done head, in the HCCA), and that memory can share a host page with
     * GPU data the renderer's write watch has closed (16 KB pages on the
     * Mac). Deferred, that fault was held until the handler returned and
     * then delivered without an address at the next instruction -- an
     * unhandled crash (Silent Scope 3's intro, in its USB interrupt
     * routine). Handled now, the watch opens the page and the write goes
     * on, as for any other write. */
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &s_old_segv);
    sigaction(SIGBUS, &sa, &s_old_bus);
}

/* ── Helpers ───────────────────────────────────────────────── */

static void *load_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    void *data = NULL;
    long size;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0 && (data = malloc((size_t)size)) &&
            fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *out_size = (size_t)size;
    return data;
}

static void mkdirs(const char *path)
{
    char tmp[PATH_MAX];
    char *p;

    snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* ── Relaunch ──────────────────────────────────────────────── */

static char **s_argv;

/* XLaunchNewImage (0xB6256), replaced: VC3 relaunches d:\vc3.xbe to leave the
 * test menu, which on a Chihiro is a reboot into the game. Here it is a fresh
 * process with the same arguments; the next boot is a normal one (no TestLaunch data).
 * stdcall (path, launch data); returns only if exec fails. */
void vc3_relaunch(void)
{
    char exe[PATH_MAX] = { 0 };
    uint32_t size = sizeof(exe);
    uint32_t path = MEM32(g_esp + 4);

    fprintf(stderr, "[VC3] XLaunchNewImage(%s): restarting\n",
            path ? (const char *)XBOX_PTR(path) : "(null)");
    fflush(stdout);
    fflush(stderr);
    /* A scripted run's presses belong to the boot that read them; the next
     * boot gets VC3_AUTOKEYS_RELAUNCH, if any. */
    if (ch_env("AUTOKEYS_RELAUNCH"))
        ch_setenv("AUTOKEYS", ch_env("AUTOKEYS_RELAUNCH"), 1);
    else
        ch_unsetenv("AUTOKEYS");
    ch_unsetenv("AUTOKEYS_RELAUNCH");
    ch_unsetenv("TEST_LAUNCH");      /* leaving test mode boots the game */
#if defined(__ANDROID__)
    /* An app cannot exec itself: the activity starts a fresh process. */
    (void)exe; (void)size;
    {
        extern void vc3_quest_restart(void);   /* src/android/quest_host.c */
        vc3_quest_restart();
    }
#else
#if defined(__APPLE__)
    if (s_argv && _NSGetExecutablePath(exe, &size) == 0)
#else
    if (s_argv && readlink("/proc/self/exe", exe, size - 1) > 0)   /* exe zeroed */
#endif
        execv(exe, s_argv);
#endif
    perror("[VC3] relaunch failed");
    g_eax = 0x80004005u;
    g_esp += 12;
}

/* ── Launching another image ───────────────────────────────── */

/* The toolkit's launch handler (kernel.h xbox_SetLaunchHandler): a title's
 * XLaunchNewImage of another XBE on its disc ("\Device\CdRom0;ss2.xbe")
 * runs that game's binary, the one beside this one named after it, with
 * this one's arguments and the launch data page in a file it reads at boot
 * (RECOMP_LAUNCH_PAGE) -- the console's reboot into the new image.
 * default.xbe and the dashboard (an empty path) are the disc's launcher
 * (the profile's disc_launcher). Returns -- the title then exits, as
 * before -- without a disc launcher, a binary for that image, or on Android
 * (each game is its own app there). */
static void chihiro_launch_image(const char *path, const uint8_t *page)
{
    const ChihiroProfile *gp = chihiro_profile();
    const char *file = path, *p;
    char name[64], exe[PATH_MAX] = { 0 }, bin[PATH_MAX], page_path[PATH_MAX];
    size_t n = 0;

    if (!gp->disc_launcher || !s_argv)
        return;
    for (p = path; *p; p++)                 /* the file part: after ';', '\' or ':' */
        if (*p == ';' || *p == '\\' || *p == ':')
            file = p + 1;
    for (p = file; *p && *p != '.' && n < sizeof(name) - 1; p++)
        name[n++] = (char)tolower((unsigned char)*p);
    name[n] = 0;
    if (!n || !strcmp(name, "default"))
        snprintf(name, sizeof(name), "%s", gp->disc_launcher);
#if defined(__ANDROID__)
    /* Each image is its own app on the headset: the page goes into the game
     * folder (the working directory; android_main.c hands it to the next
     * app at its boot) and the sibling app named after the image starts
     * once this process has ended (quest_host.c). */
    (void)exe; (void)bin;
    {
        extern void chihiro_quest_launch(const char *image);
        snprintf(page_path, sizeof(page_path), "launch.bin");
        remove(page_path);
        if (page) {
            FILE *f = fopen(page_path, "wb");
            if (!f || fwrite(page, 1, 0x1000, f) != 0x1000) {
                if (f)
                    fclose(f);
                remove(page_path);
                fprintf(stderr, "[launch] %s: could not write %s, exiting\n", path, page_path);
                return;
            }
            fclose(f);
        }
        fprintf(stderr, "[launch] %s -> the %s app\n", path[0] ? path : "(dashboard)", name);
        chihiro_quest_launch(name);
        remove(page_path);          /* no app to hand it to */
    }
#else
    {
        uint32_t size = sizeof(exe);
        char *slash;
#if defined(__APPLE__)
        if (_NSGetExecutablePath(exe, &size) != 0)
            return;
#else
        if (readlink("/proc/self/exe", exe, size - 1) <= 0)
            return;
#endif
        if (!realpath(exe, bin))
            return;
        if ((slash = strrchr(bin, '/')))
            slash[1] = 0;
        if (strlen(bin) + strlen(name) + 1 > sizeof(bin))
            return;
        strcat(bin, name);
        if (access(bin, X_OK) != 0) {
            fprintf(stderr, "[launch] %s: no %s beside this binary, exiting\n", path, bin);
            return;
        }
    }
    unsetenv("RECOMP_LAUNCH_PAGE");
    /* Scripted runs across images: RECOMP_PAD_SCRIPT_CHAIN="next|after|..."
     * gives each launched image the next script (the toolkit's
     * RECOMP_PAD_SCRIPT), the rest kept for the image after it. */
    {
        const char *chain = getenv("RECOMP_PAD_SCRIPT_CHAIN");
        if (chain && *chain) {
            char head[4096];
            const char *bar = strchr(chain, '|');
            size_t len = bar ? (size_t)(bar - chain) : strlen(chain);
            if (len >= sizeof(head))
                len = sizeof(head) - 1;
            memcpy(head, chain, len);
            head[len] = 0;
            setenv("RECOMP_PAD_SCRIPT", head, 1);
            if (bar)
                setenv("RECOMP_PAD_SCRIPT_CHAIN", bar + 1, 1);
            else
                unsetenv("RECOMP_PAD_SCRIPT_CHAIN");
        }
    }
    if (page) {
        const char *tmp = getenv("TMPDIR");
        FILE *f;
        snprintf(page_path, sizeof(page_path), "%s/chihiro-launch-%d.bin",
                 tmp && *tmp ? tmp : "/tmp", (int)getpid());
        f = fopen(page_path, "wb");
        if (!f || fwrite(page, 1, 0x1000, f) != 0x1000) {
            if (f)
                fclose(f);
            fprintf(stderr, "[launch] %s: could not write %s, exiting\n", path, page_path);
            return;
        }
        fclose(f);
        setenv("RECOMP_LAUNCH_PAGE", page_path, 1);
    }
    fprintf(stderr, "[launch] %s -> %s\n", path[0] ? path : "(dashboard)", bin);
    fflush(stdout);
    fflush(stderr);
    s_argv[0] = bin;
    execv(bin, s_argv);
    perror("[launch] exec failed");
#endif
}

/* ── Boot (on the guest thread) ────────────────────────────── */

extern int xbox_HostWindowMain(int (*guest_main)(void));
extern int vc3_quest_run(int (*guest_main)(void));   /* src/android/quest_host.c */
extern void xbox_HostWindowOnOpen(void (*fn)(void));
extern void vc3_menu_install(void);          /* src/macos_menu.m */

static char s_game_dir[PATH_MAX], s_save_dir[PATH_MAX];
static void *s_xbe;
static size_t s_xbe_size;

/* Runs on the thread the toolkit's window starts (or directly, headless):
 * the guest registers are thread-local, so the whole boot belongs here. */
/* Once the dispatch table is built, before the first guest instruction: a
 * game patches its entries here (VC3: vc3_cri_trap.c). */
__attribute__((weak)) void chihiro_game_dispatch_ready(void) {}

static int guest_main(void)
{
    install_fault_handler();

    /* Chihiro has 128 MB, like a devkit; the game sizes its pools for it. */
    xbox_SetTotalRam(XBOX_DEVKIT_RAM);
    if (!xbox_MemoryLayoutInit(s_xbe, s_xbe_size)) {
        fprintf(stderr, "failed to map the Xbox address space\n");
        return 1;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();

    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n", g_apu_state ? "up" : "FAILED");
    }

    xbox_kernel_init();
    xbox_path_init(s_game_dir, s_save_dir);
    xbox_kernel_bridge_init();
    xbox_OhciInit();        /* USB controllers: only with RECOMP_USB (retail titles) */
    chihiro_init(s_save_dir);
    chihiro_d3d_init();     /* D3D's semaphore, from the profile's device pointer */
#if defined(CHIHIRO_RENDERER_VULKAN)
    if (!ch_vk_runtime_start())
        return 1; /* No guest execution and no fallback after Vulkan failure. */
#else
    vc3_gl_backend_register();   /* before the title submits any work */
#endif

    g_esp = XBOX_STACK_TOP;
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; using binary search\n");
    chihiro_game_dispatch_ready();
    xbox_WatchdogStart();

    printf("entry 0x%08X, esp 0x%08X\n", chihiro_profile()->entry, g_esp);
    xbox_ThreadAcctName("guest main");      /* RECOMP_THREAD_LOG */
    xbe_entry_point();

    printf("game returned\n");
    xbox_kernel_shutdown();
#if defined(CHIHIRO_RENDERER_VULKAN)
    ch_vk_runtime_stop();
#endif
    xbox_MemoryLayoutShutdown();
    return 0;
}

/* ── main ──────────────────────────────────────────────────── */

#if defined(__linux__) && !defined(__ANDROID__)
#include <sched.h>
#include <errno.h>
#include <sys/syscall.h>
/* Big.LITTLE (the Steam Frame: two Cortex-A520 at a fifth of an A720's
 * capacity): the game's frame passes from the guest thread to the executor
 * to the submission thread, and a stage the scheduler put on a little core
 * finished late -- 34 of 600 frames were handed over a vblank late, 4 kept
 * off them. Before any thread starts (they inherit it), the process runs on
 * the cores with at least half the biggest one's capacity. Nothing to do on
 * symmetric machines; CHIHIRO_BIG_CORES=0 leaves the affinity alone. */
static void big_cores(void)
{
    const char *e = getenv("CHIHIRO_BIG_CORES");
    unsigned cap[CPU_SETSIZE] = {0}, max = 0, n = 0, i, kept = 0;
    cpu_set_t set;
    if (e && *e == '0')
        return;
    for (i = 0; i < CPU_SETSIZE && i < 256; i++) {
        char path[96];
        FILE *f;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%u/cpu_capacity", i);
        if (!(f = fopen(path, "r")))
            break;
        if (fscanf(f, "%u", &cap[i]) != 1)
            cap[i] = 0;
        fclose(f);
        if (cap[i] > max)
            max = cap[i];
        n = i + 1;
    }
    if (!max || n < 2)
        return;
    CPU_ZERO(&set);
    for (i = 0; i < n; i++)
        if (cap[i] * 2 >= max) {
            CPU_SET(i, &set);
            kept++;
        }
    if (kept == n || !kept)
        return;
    if (sched_setaffinity(0, sizeof set, &set) == 0)
        fprintf(stderr, "[host] %u of %u cores (the big ones: capacity >= %u of %u)\n", kept, n, max / 2, max);
    {   /* A utilization floor for the clock governor (inherited by every
         * thread): at ~1/3 load schedutil kept the big cores at 1-2 of 3.1
         * GHz, and the frame's chain of threads finished later and less
         * evenly (p95 19-20 -> 18.5 ms, frames a vblank late 10 -> ~2 of
         * 600). CHIHIRO_UCLAMP_MIN=n (0..1024, 0: none). */
        const char *u = getenv("CHIHIRO_UCLAMP_MIN");
        if (!u || atoi(u) > 0) {
            struct { uint32_t size, policy; uint64_t flags; int32_t nice; uint32_t prio;
                     uint64_t runtime, deadline, period; uint32_t util_min, util_max; } a;
            memset(&a, 0, sizeof a);
            a.size = sizeof a;
            a.flags = 0x08 | 0x10 | 0x20;   /* KEEP_POLICY | KEEP_PARAMS | UTIL_CLAMP_MIN */
            a.util_min = u ? (uint32_t)atoi(u) : 512u;
            if (a.util_min > 1024u)
                a.util_min = 1024u;
            long r = syscall(SYS_sched_setattr, 0, &a, 0);
            fprintf(stderr, "[host] uclamp_min %u: %s\n", a.util_min, r == 0 ? "set" : strerror(errno));
        }
    }
}
#else
static void big_cores(void) {}
#endif

int main(int argc, char **argv)
{
    char *game_dir = s_game_dir, *save_dir = s_save_dir, xbe_path[PATH_MAX];
    const ChihiroProfile *gp;
    const char *dir, *save;
    int rc;

    big_cores();
    chihiro_profile_apply(chihiro_game());
    gp = chihiro_profile();
    dir = argc > 1 ? argv[1] : ch_env("GAME_DIR");
    save = ch_env("SAVE_DIR");

    s_argv = argv;
    xbox_SetLaunchHandler(chihiro_launch_image);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (!dir || !realpath(dir, game_dir)) {
        fprintf(stderr, "usage: %s <game_dir>   (or set %s_GAME_DIR)\n"
                        "game_dir must contain %s%s\n",
                argv[0], ch_env_prefix(), gp->xbe,
                gp->platform == CHIHIRO_PLATFORM_CHIHIRO ? " and media/" : "");
        return 2;
    }
    if (save)
        snprintf(save_dir, sizeof(s_save_dir), "%s", save);
    else
        snprintf(save_dir, sizeof(s_save_dir),
                 "%s/Library/Application Support/%s",
                 getenv("HOME") ? getenv("HOME") : ".", gp->save_name);
    mkdirs(save_dir);

    /* Silent by default: SDL's dummy driver consumes the mix without playing
     * it. VC3_SOUND=1 plays it; RECOMP_AUDIO_WAV captures it either way. */
    if (!ch_env("SOUND") || strcmp(ch_env("SOUND"), "1") != 0)
        setenv("SDL_AUDIODRIVER", "dummy", 1);
    /* The emulated MCPX APU: without it the game has no sound at all. With
     * it, VC3 starts the APU itself and mixes music and effects (measured:
     * RMS 1-3.4k from the attract onwards, RECOMP_AUDIO_WAV capture). */
    setenv("RECOMP_AC97_READY", "1", 0);

    if (gp->platform == CHIHIRO_PLATFORM_CHIHIRO) {
        /* The boot-time memory probe (VC3: 0x34F60) allocates and frees
         * contiguous blocks of growing size up to 256 MB. Without reclaim
         * the frees are dropped and D3D's 9 MB framebuffer request no
         * longer fits. */
        setenv("RECOMP_HEAP_RECLAIM", "1", 0);
        /* Chihiro drives a VGA monitor; VC3's modes (640x480p by default,
         * see chihiro_dip_switches) are looked up in the VGA rows of D3D's
         * mode table, so an HDTV pack fails device creation. */
        setenv("RECOMP_AV_PACK", "vga", 0);
    } else {
        /* A retail title reads its controller through XAPI's USB driver,
         * statically linked: the toolkit's OHCI model and Controller S
         * (xbox_OhciInit in guest_main), fed by SDL's game controllers. */
        setenv("RECOMP_USB", "1", 0);
        /* The keyboard is player 1's pad too (toolkit input/keyboard.c:
         * arrows d-pad, Enter START, Z/X/C/V A/B/X/Y, 1/3 triggers, WASD and
         * IJKL the sticks), merged on top of a real pad, so a Mac with no
         * controller can play. RECOMP_KEYBOARD=0 turns it off. */
        setenv("RECOMP_KEYBOARD", "1", 0);
        /* Walk the pushbuffer as the DMA engine does: JUMP/CALL/RETURN and
         * ring wraps followed, GET published as it goes, each kick's marker
         * latched in PGRAPH 0x400B10. XDK 5558 D3D (Silent Scope) spins in
         * its ring-space check (0xFC300) until that latch matches the
         * semaphore, and the segment scan VC3 runs on lost the commands
         * around a ring wrap at a stage load: their semaphore releases never
         * landed and D3D waited for good. The walk's "GPU far behind" path
         * (D3D sleeping on a software-method interrupt) is out of the way:
         * the game's D3D_BlockOnTime is the runtime's polling one. */
        setenv("RECOMP_PB_WALK", "dma", 0);
        /* One CPU at DISPATCH_LEVEL, as on the console: XAPI's
         * XGetDeviceChanges raises IRQL to read the USB driver's state
         * atomically against its DPCs. Without the gate the title saw a pad
         * arrive before its device node was ready, XInputOpen failed
         * (ERROR_DEVICE_NOT_CONNECTED) and it never asked again. */
        setenv("RECOMP_DISPATCH_GATE", "1", 0);
        /* A smaller clip on a surface draws into its existing target
         * (vk_backend.c): Silent Scope alternates 640x480 and 633x432 clips
         * on its back buffer every frame. */
        setenv("CHIHIRO_VULKAN_SURFACE_GROW", "1", 0);
        /* A flip that drew only smaller surfaces (Silent Scope 2's scope
         * views) shows the last screen surface again (vk_backend.c). */
        setenv("CHIHIRO_VULKAN_PRESENT_SCREEN", "1", 0);
        /* Flips complete at the vblank the swap's presentation interval
         * asks for, as on the console. Silent Scope's games present with
         * interval 2: 30 fps on an Xbox. Unpaced, Silent Scope 2 ran 300+
         * fps with its game clock 4x too fast (its logic steps per frame). */
        setenv("RECOMP_FLIP_PACING", "1", 0);
        /* A read on an asynchronous handle returns STATUS_PENDING, as a DVD
         * read does on the console (toolkit NtReadFile). Silent Scope 3
         * requires GetLastError() == ERROR_IO_PENDING after its overlapped
         * ReadFile and shows its disc-error screen otherwise. */
        setenv("RECOMP_ASYNC_IO", "1", 0);
    }
    /* D3D is linked into the XBE, so frames exist only as NV2A pushbuffer:
     * the toolkit's executor runs it and answers D3D's fences. */
    setenv("RECOMP_PB_EXEC", "1", 0);
#if defined(CHIHIRO_RENDERER_VULKAN) && defined(__linux__) && !defined(__ANDROID__)
    /* Flips complete at their vblank, as on the console: the frame leaves
     * the executor at the edge instead of whenever its commands ran out, so
     * frames reach the presenter evenly (Steam Frame, immersive: p95 frame
     * interval ~19.5 -> 18.4 ms, none over 20 ms in combat and E.S.).
     * RECOMP_FLIP_PACING=0: as soon as they are done. */
    setenv("RECOMP_FLIP_PACING", "1", 0);
#endif
    /* Fixed-function batches -- nearly all of VC3's geometry -- and the 3D
     * vertex-program batches (E.S. mode's meshes: millions of triangles)
     * transform on the GPU; the screen-space program batches (HUD and
     * composite quads) stay on the CPU, where the back end can see their
     * vertices. RECOMP_GPU_XF=ff keeps programs on the CPU, =0 everything. */
#if defined(CHIHIRO_RENDERER_VULKAN)
    /* Shared fixed and specialized vertex programs run on native Vulkan.
     * Context-write programs and unsupported offers retain the reference path.
     * RECOMP_GPU_XF=ff is the diagnostic CPU-program control. */
    setenv("RECOMP_GPU_XF", "all", 0);
#else
    setenv("RECOMP_GPU_XF", "all", 0);
#endif
#if defined(CHIHIRO_RENDERER_VULKAN)
#if defined(__APPLE__)
    /* SDL's scanout window uses Metal, never an implicit OpenGL renderer. */
    setenv("SDL_RENDER_DRIVER", "metal", 1);
#else
    setenv("SDL_RENDER_DRIVER", "software", 1);
#endif
#endif
    /* The pushbuffer decode and the GL submission on two threads (the
     * mission intro's executor CPU halves on the Mac). Not on the Quest by
     * default: there it measured no better (intro low10 at 2x 46.9 with,
     * 47.4 without; native 32.8 / 35.1) -- the GL thread is not its limit.
     * RECOMP_PB_DECODE_THREAD=1 to try it there, =0 for one thread here. */
#if !defined(__ANDROID__)
    setenv("RECOMP_PB_DECODE_THREAD", "1", 0);
#endif
    /* D3D's present path waits on a KEVENT it built in the device itself
     * (0x119254), set by its vblank ISR/DPC: it needs title-built events and
     * a 60 Hz vblank that honours the interrupt enables. */
    setenv("RECOMP_TITLE_KEVENTS", "1", 0);
    setenv("RECOMP_VBLANK", "paced", 0);
    /* A window by default, sized for the display mode the DIP switches pick:
     * 640x480 shown at 2x, the larger modes at 1x. On the Quest the headset
     * shows the frames (src/android/quest_host.c), not the toolkit's window. */
#if !defined(__ANDROID__) && !defined(CHIHIRO_XR_DESKTOP)
    setenv("RECOMP_FB_WINDOW", "1", 0);
#endif
    {
        /* the profile's display modes: the first is the default */
        const char *res = ch_env("RES");
        const ChihiroDisplayMode *m = &gp->modes[0];
        int i;
        for (i = 1; res && i < 4 && gp->modes[i].fb_size; i++)
            if (gp->modes[i].res && !strcmp(res, gp->modes[i].res))
                m = &gp->modes[i];
        /* No modes (a retail title picks its own): 640x480 shown at 2x */
        setenv("RECOMP_FB_SIZE", m->fb_size ? m->fb_size : "640x480", 0);
        setenv("RECOMP_WINDOW_SCALE", m->window_scale ? m->window_scale : "2", 0);
    }

    snprintf(xbe_path, sizeof(xbe_path), "%s/%s", game_dir, gp->xbe);
    s_xbe = load_file(xbe_path, &s_xbe_size);
    if (!s_xbe) {
        fprintf(stderr, "cannot read %s\n", xbe_path);
        return 1;
    }
    printf("=== %s (recomp) ===\ngame: %s\nsave: %s\n",
           gp->title, game_dir, save_dir);

    /* Shown frames per game frame (VC3_FPS, else the saved setting); the
     * Display menu changes it from the window. */
    vc3_display_init();
#if defined(VC3_MACOS_MENU)
    xbox_HostWindowOnOpen(vc3_menu_install);
#endif

#if defined(__ANDROID__) || defined(CHIHIRO_XR_DESKTOP)
#if defined(CHIHIRO_RENDERER_VULKAN)
    const char *offscreen=ch_env("VULKAN_OFFSCREEN");
    if(offscreen && !strcmp(offscreen,"1")) {
        fprintf(stderr,"[Vulkan] Android offscreen diagnostic: no XR session or VR performance acceptance\n");
        rc = xbox_HostWindowMain(guest_main);
    } else
#endif
    rc = vc3_quest_run(guest_main);
#else
    rc = xbox_HostWindowMain(guest_main);
#endif
    free(s_xbe);
    return rc;
}
