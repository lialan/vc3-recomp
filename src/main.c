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

#include <dlfcn.h>
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

#define VC3_ENTRY_POINT 0x000B6407 /* XBE entry, debug-key decoded */
#define VC3_XBE_NAME    "vc3.xbe"

extern void xbe_entry_point(void);
extern void xbox_path_init(const char *game_dir, const char *save_dir);
extern void chihiro_init(const char *save_dir);

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
#endif
        if (pc && dladdr((void *)pc, &di) && di.dli_sname)
            fprintf(stderr, "  host pc %p in %s+0x%lx\n", (void *)pc, di.dli_sname,
                    (unsigned long)(pc - (uintptr_t)di.dli_saddr));
        if (lr && dladdr((void *)lr, &di) && di.dli_sname)
            fprintf(stderr, "  host lr %p in %s+0x%lx\n", (void *)lr, di.dli_sname,
                    (unsigned long)(lr - (uintptr_t)di.dli_saddr));
    }
    signal(sig, SIG_DFL); /* re-raise for the crash reporter / debugger */
}

static void install_fault_handler(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
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

/* ── main ──────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    char game_dir[PATH_MAX], save_dir[PATH_MAX], xbe_path[PATH_MAX];
    const char *dir = argc > 1 ? argv[1] : getenv("VC3_GAME_DIR");
    const char *save = getenv("VC3_SAVE_DIR");
    void *xbe;
    size_t xbe_size = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (!dir || !realpath(dir, game_dir)) {
        fprintf(stderr, "usage: %s <game_dir>   (or set VC3_GAME_DIR)\n"
                        "game_dir must contain %s and media/\n",
                argv[0], VC3_XBE_NAME);
        return 2;
    }
    if (save)
        snprintf(save_dir, sizeof(save_dir), "%s", save);
    else
        snprintf(save_dir, sizeof(save_dir),
                 "%s/Library/Application Support/vc3-recomp",
                 getenv("HOME") ? getenv("HOME") : ".");
    mkdirs(save_dir);

    /* Silent by default: SDL's dummy driver consumes the mix without playing
     * it. VC3_SOUND=1 plays it; RECOMP_AUDIO_WAV captures it either way. */
    if (!getenv("VC3_SOUND") || strcmp(getenv("VC3_SOUND"), "1") != 0)
        setenv("SDL_AUDIODRIVER", "dummy", 1);

    /* The boot-time memory probe (0x34F60) allocates and frees contiguous
     * blocks of growing size up to 256 MB. Without reclaim the frees are
     * dropped and D3D's 9 MB framebuffer request no longer fits. */
    setenv("RECOMP_HEAP_RECLAIM", "1", 0);
    /* Chihiro drives a VGA monitor; VC3's 1024x768 mode only exists in the
     * VGA rows of D3D's mode table, so an HDTV pack fails device creation. */
    setenv("RECOMP_AV_PACK", "vga", 0);
    /* D3D is linked into the XBE, so frames exist only as NV2A pushbuffer:
     * the toolkit's executor runs it and answers D3D's fences. */
    setenv("RECOMP_PB_EXEC", "1", 0);
    /* D3D's present path waits on a KEVENT it built in the device itself
     * (0x119254), set by its vblank ISR/DPC: it needs title-built events and
     * a 60 Hz vblank that honours the interrupt enables. */
    setenv("RECOMP_TITLE_KEVENTS", "1", 0);
    setenv("RECOMP_VBLANK", "paced", 0);

    snprintf(xbe_path, sizeof(xbe_path), "%s/%s", game_dir, VC3_XBE_NAME);
    xbe = load_file(xbe_path, &xbe_size);
    if (!xbe) {
        fprintf(stderr, "cannot read %s\n", xbe_path);
        return 1;
    }
    printf("=== Virtua Cop 3 (recomp) ===\ngame: %s\nsave: %s\n",
           game_dir, save_dir);

    install_fault_handler();

    /* Chihiro has 128 MB, like a devkit; the game sizes its pools for it. */
    xbox_SetTotalRam(XBOX_DEVKIT_RAM);
    if (!xbox_MemoryLayoutInit(xbe, xbe_size)) {
        fprintf(stderr, "failed to map the Xbox address space\n");
        return 1;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();

    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n", g_apu_state ? "up" : "FAILED");
    }

    xbox_kernel_init();
    xbox_path_init(game_dir, save_dir);
    xbox_kernel_bridge_init();
    chihiro_init(save_dir);

    g_esp = XBOX_STACK_TOP;
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; using binary search\n");
    xbox_WatchdogStart();

    printf("entry 0x%08X, esp 0x%08X\n", VC3_ENTRY_POINT, g_esp);
    xbe_entry_point();

    printf("game returned\n");
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe);
    return 0;
}
