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

#include "thread_acct.h"
#include <dlfcn.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>   /* _NSGetExecutablePath */
#endif
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

#define VC3_ENTRY_POINT 0x000B6407 /* XBE entry, debug-key decoded */
#define VC3_XBE_NAME    "vc3.xbe"

extern void xbe_entry_point(void);
extern void xbox_path_init(const char *game_dir, const char *save_dir);
extern void chihiro_init(const char *save_dir);
extern void vc3_gl_backend_register(void);   /* src/gpu/gl_backend.c */

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
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
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
    if (getenv("VC3_AUTOKEYS_RELAUNCH"))
        setenv("VC3_AUTOKEYS", getenv("VC3_AUTOKEYS_RELAUNCH"), 1);
    else
        unsetenv("VC3_AUTOKEYS");
    unsetenv("VC3_AUTOKEYS_RELAUNCH");
    unsetenv("VC3_TEST_LAUNCH");      /* leaving test mode boots the game */
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
    chihiro_init(s_save_dir);
    vc3_gl_backend_register();   /* before the title submits any work */

    g_esp = XBOX_STACK_TOP;
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; using binary search\n");
    xbox_WatchdogStart();

    printf("entry 0x%08X, esp 0x%08X\n", VC3_ENTRY_POINT, g_esp);
    xbox_ThreadAcctName("guest main");      /* RECOMP_THREAD_LOG */
    xbe_entry_point();

    printf("game returned\n");
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    return 0;
}

/* ── main ──────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    char *game_dir = s_game_dir, *save_dir = s_save_dir, xbe_path[PATH_MAX];
    const char *dir = argc > 1 ? argv[1] : getenv("VC3_GAME_DIR");
    const char *save = getenv("VC3_SAVE_DIR");
    int rc;

    s_argv = argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (!dir || !realpath(dir, game_dir)) {
        fprintf(stderr, "usage: %s <game_dir>   (or set VC3_GAME_DIR)\n"
                        "game_dir must contain %s and media/\n",
                argv[0], VC3_XBE_NAME);
        return 2;
    }
    if (save)
        snprintf(save_dir, sizeof(s_save_dir), "%s", save);
    else
        snprintf(save_dir, sizeof(s_save_dir),
                 "%s/Library/Application Support/vc3-recomp",
                 getenv("HOME") ? getenv("HOME") : ".");
    mkdirs(save_dir);

    /* Silent by default: SDL's dummy driver consumes the mix without playing
     * it. VC3_SOUND=1 plays it; RECOMP_AUDIO_WAV captures it either way. */
    if (!getenv("VC3_SOUND") || strcmp(getenv("VC3_SOUND"), "1") != 0)
        setenv("SDL_AUDIODRIVER", "dummy", 1);
    /* The emulated MCPX APU: without it the game has no sound at all. With
     * it, VC3 starts the APU itself and mixes music and effects (measured:
     * RMS 1-3.4k from the attract onwards, RECOMP_AUDIO_WAV capture). */
    setenv("RECOMP_AC97_READY", "1", 0);

    /* The boot-time memory probe (0x34F60) allocates and frees contiguous
     * blocks of growing size up to 256 MB. Without reclaim the frees are
     * dropped and D3D's 9 MB framebuffer request no longer fits. */
    setenv("RECOMP_HEAP_RECLAIM", "1", 0);
    /* Chihiro drives a VGA monitor; VC3's modes (640x480p by default, see
     * chihiro_dip_switches) are looked up in the VGA rows of D3D's mode
     * table, so an HDTV pack fails device creation. */
    setenv("RECOMP_AV_PACK", "vga", 0);
    /* D3D is linked into the XBE, so frames exist only as NV2A pushbuffer:
     * the toolkit's executor runs it and answers D3D's fences. */
    setenv("RECOMP_PB_EXEC", "1", 0);
    /* Fixed-function batches -- nearly all of VC3's geometry -- and the 3D
     * vertex-program batches (E.S. mode's meshes: millions of triangles)
     * transform on the GPU; the screen-space program batches (HUD and
     * composite quads) stay on the CPU, where the back end can see their
     * vertices. RECOMP_GPU_XF=ff keeps programs on the CPU, =0 everything. */
    setenv("RECOMP_GPU_XF", "all", 0);
    /* D3D's present path waits on a KEVENT it built in the device itself
     * (0x119254), set by its vblank ISR/DPC: it needs title-built events and
     * a 60 Hz vblank that honours the interrupt enables. */
    setenv("RECOMP_TITLE_KEVENTS", "1", 0);
    setenv("RECOMP_VBLANK", "paced", 0);
    /* A window by default, sized for the display mode the DIP switches pick:
     * 640x480 shown at 2x, the larger modes at 1x. On the Quest the headset
     * shows the frames (src/android/quest_host.c), not the toolkit's window. */
#if !defined(__ANDROID__)
    setenv("RECOMP_FB_WINDOW", "1", 0);
#endif
    {
        const char *res = getenv("VC3_RES");
        if (res && !strcmp(res, "1024")) {
            setenv("RECOMP_FB_SIZE", "1024x768", 0);
            setenv("RECOMP_WINDOW_SCALE", "1", 0);
        } else if (res && !strcmp(res, "800")) {
            setenv("RECOMP_FB_SIZE", "800x600", 0);
            setenv("RECOMP_WINDOW_SCALE", "1", 0);
        } else {
            setenv("RECOMP_FB_SIZE", "640x480", 0);
            setenv("RECOMP_WINDOW_SCALE", "2", 0);
        }
    }

    snprintf(xbe_path, sizeof(xbe_path), "%s/%s", game_dir, VC3_XBE_NAME);
    s_xbe = load_file(xbe_path, &s_xbe_size);
    if (!s_xbe) {
        fprintf(stderr, "cannot read %s\n", xbe_path);
        return 1;
    }
    printf("=== Virtua Cop 3 (recomp) ===\ngame: %s\nsave: %s\n",
           game_dir, save_dir);

    /* Shown frames per game frame (VC3_FPS, else the saved setting); the
     * Display menu changes it from the window. */
    vc3_display_init();
#if defined(VC3_MACOS_MENU)
    xbox_HostWindowOnOpen(vc3_menu_install);
#endif

#if defined(__ANDROID__)
    rc = vc3_quest_run(guest_main);
#else
    rc = xbox_HostWindowMain(guest_main);
#endif
    free(s_xbe);
    return rc;
}
