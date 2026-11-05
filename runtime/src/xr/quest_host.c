/*
 * The Meta Quest host: the headset, its controllers, and the threads.
 *
 * On the Quest the game is shown as it was meant to be, on a screen: a big
 * flat picture in front of the player (an OpenXR quad layer), which the
 * headset's compositor keeps steady as the head moves -- reprojected at the
 * display's rate (120 Hz asked for: every 60 Hz game frame shown twice,
 * evenly), so the game itself still runs at its own 60.
 *
 *   SDLThread (SDL_main)   the window, the EGL context the headset is given,
 *                          the OpenXR session and its frame loop, the
 *                          controllers;
 *   guest thread           the boot and the recompiled game (main.c);
 *   NV2A executor thread   the GL back end (gpu/gl_backend.c), in an EGL
 *                          context of its own that shares objects with the
 *                          first (vc3_quest_gl_context).
 *
 * Frames: at each flip the back end blits the presented surface, as drawn
 * (any internal resolution, 16:9 or 4:3), into one of three shared
 * textures and fences it (vc3_quest_frame); the headset loop takes the
 * newest, waits on its fence on the GPU, and copies it into the layer's
 * swapchain. (VC3_XR_DIRECT=1: the loop lends the back end a swapchain image
 * and the back end blits straight into it -- off by default, see "Lent
 * images".) Nothing is read back to the CPU.
 *
 * The gun is a handgun in the player's hand (quest_overlay.c, drawn in a
 * transparent layer over the screen), with iron sights, a red dot or a
 * holographic sight: the line its sight looks along meets the screen, and
 * that point in the game's 4:3 picture is where the gun reads
 * (host_input.c). The controls and the settings panel are The House of the
 * Dead 4 VR's -- see gun_frame and the panel's notes below. All of it is
 * saved in the settings file.
 *
 * Stereo 3D (on by default here): the back end draws the 3D view once per
 * eye, side by side in one frame (gl_backend.c), with depth around the
 * screen's plane; each eye's half goes to a quad layer that eye alone sees,
 * at the same place, so the screen is a window with depth behind it and
 * things coming out of it. The separation is set from the screen's size:
 * what is infinitely far lies one eye distance apart on the screen.
 *
 * Without an OpenXR runtime (a phone, the Android emulator) the frames go
 * to the SDL window instead, letterboxed, and the touch screen or mouse is
 * the gun -- how this is tested without a headset.
 *
 * Desktop Linux (CHIHIRO_XR_DESKTOP, Vulkan only; the Steam Frame under
 * SteamVR): the same host, as a process of its own -- no activity, no SDL
 * window, the OpenXR loader found the desktop way, and Restart is an exec.
 */
#if defined(__ANDROID__) || defined(CHIHIRO_XR_DESKTOP)
#if defined(CHIHIRO_XR_DESKTOP) && !defined(CHIHIRO_RENDERER_VULKAN)
#error "the desktop OpenXR host is Vulkan only"
#endif

#define _GNU_SOURCE                     /* cpu_set_t, sched_setaffinity */
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/resource.h>                 /* setpriority: a thread hint's fallback */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef CHIHIRO_RENDERER_VULKAN
#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#endif
#if defined(__ANDROID__)
#include <jni.h>
#include <sys/system_properties.h>
#endif
#include <SDL.h>

#if defined(__ANDROID__)
#define XR_USE_PLATFORM_ANDROID
#endif
#ifdef CHIHIRO_RENDERER_VULKAN
#include "gpu/vulkan/vk_runtime.h"
#include "gpu/vulkan/vk_gpu_mailbox.h"
#include "gpu/vulkan/vk_xr_quad.h"
#include "gpu/vulkan/vk_xr_bridge.h"
#include "gpu/vulkan/vk_overlay.h"
#define XR_USE_GRAPHICS_API_VULKAN
#else
#define XR_USE_GRAPHICS_API_OPENGL_ES
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "display_settings.h"
#include "host_stats.h"
#include "host_input.h"
#include "quest_menu.h"
#include "quest_overlay.h"
#include "retail_pad.h"
#include "vr_math.h"
#include "vr_view.h"
#include "chihiro/ch_env.h"
#include "chihiro/chihiro_profile.h"
#include "thread_acct.h"                  /* g_xbox_thread_named */

void vc3_quest_restart(void);       /* below: a fresh process */
void xbox_VblankAlign(uint64_t edge_ns);   /* the toolkit's kernel */
uint64_t xbox_HostNowNs(void);

#ifndef GL_FRAMEBUFFER_SRGB_EXT
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9
#endif

/* ── EGL: the host's context and the back end's ───────────────────────── */

static SDL_Window *s_win;
#ifndef CHIHIRO_RENDERER_VULKAN
static EGLDisplay s_dpy = EGL_NO_DISPLAY;
static EGLContext s_ctx = EGL_NO_CONTEXT;
static EGLConfig  s_cfg;
static int s_es_minor = 2;          /* 3.2 on the Quest; 3.0 in the emulator */
#endif

/* The threads the headset is told about (perf_frame): the loop's, the GL
 * back end's, the push-buffer decode's (the toolkit's "nv2a exec": the GL
 * thread itself unless RECOMP_PB_DECODE_THREAD splits them), the game's;
 * 0 until each has started. */
static volatile pid_t s_tid_host, s_tid_exec, s_tid_decode, s_tid_guest;

/* The toolkit names its threads as they start (thread_acct.h). */
static void thread_named(const char *name)
{
    if (!strcmp(name, "nv2a exec"))
        s_tid_decode = (pid_t)syscall(SYS_gettid);
}
static volatile int s_quit;

#ifndef CHIHIRO_RENDERER_VULKAN
/* Called once on the executor thread by gl_backend.c's gl_ready: a context
 * sharing the host's objects, current on that thread from here on. */
int vc3_quest_gl_context(void)
{
    s_tid_exec = (pid_t)syscall(SYS_gettid);
    EGLint ctx_attr[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE
    };
    EGLContext c;
    EGLSurface surf = EGL_NO_SURFACE;

    if (s_dpy == EGL_NO_DISPLAY || s_ctx == EGL_NO_CONTEXT)
        return 0;
    ctx_attr[3] = s_es_minor;
    c = eglCreateContext(s_dpy, s_cfg, s_ctx, ctx_attr);
    if (c == EGL_NO_CONTEXT) {
        fprintf(stderr, "[quest] no shared GLES 3.%d context (EGL 0x%x)\n", s_es_minor,
                eglGetError());
        return 0;
    }
    /* No surface needed (everything draws into framebuffers); where the
     * driver insists on one, a tiny pbuffer. */
    if (!eglMakeCurrent(s_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) {
        static const EGLint pb[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
        surf = eglCreatePbufferSurface(s_dpy, s_cfg, pb);
        if (surf == EGL_NO_SURFACE || !eglMakeCurrent(s_dpy, surf, surf, c)) {
            fprintf(stderr, "[quest] cannot make the back end's context current (EGL 0x%x)\n",
                    eglGetError());
            eglDestroyContext(s_dpy, c);
            return 0;
        }
    }
    return 1;
}

#endif

/* ── Frames from the back end: three shared textures ──────────────────── */

#ifndef CHIHIRO_RENDERER_VULKAN
typedef struct {
    GLuint tex;
    GLsizei w, h;            /* as drawn: both eyes, side by side, in stereo */
    int eyes;                /* 1, or 2 for a stereo pair (each w / 2 wide) */
    int lw, lh;              /* the picture's shape (guest pixels) */
    GLsync fence;            /* the back end's copy into it is done */
    GLsync read_done;        /* the host's last copy out of it is done */
    Vc3VrFrame vr;           /* immersive: the pose it was drawn with, the HUD */
    GLuint hud;              /* the HUD picture (vr.hud_w x vr.hud_h) */
    GLsizei hud_w, hud_h;
    int has_hud;
    int direct;              /* the picture is in the lent swapchain image
                                (w x h), not in tex */
    GLsizei tw, th;          /* tex's size */
} Frame;

static Frame s_frame[3];
static int s_back = 0, s_ready = 1, s_front = 2;
static uint64_t s_ready_seq, s_shown_seq;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

#endif

#ifdef CHIHIRO_RENDERER_VULKAN
typedef struct {
    int w,h,eyes,lw,lh;
    Vc3VrFrame vr;
    ChVkGpuFrame gpu;
} Frame;
static ChVkXrDevice s_vk;
static ChVkGpuMailbox *s_vk_frames;
static pthread_mutex_t s_vk_queue = PTHREAD_MUTEX_INITIALIZER;
static int vk_host_init(void);
static void vk_menu_upload(const void *);
#endif
static clockid_t s_guest_clock;  /* the game thread's CPU time, for the stats panel */
static int s_guest_clock_ok;
static uint64_t s_frames_in;   /* display thread's consumed sequence */

/* VC3_PACE_LOG=1: once a second, the spread of the times between game
 * frames (back end thread) and how many headset frames showed each (host). */
static int s_pace = -1;

static uint64_t pace_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void pace_game_frame(void)
{
    static uint64_t last, t0;
    static uint32_t dt[512];
    static int n;
    uint64_t now = pace_now_us();
    if (s_pace < 0)
        s_pace = ch_env("PACE_LOG") && *ch_env("PACE_LOG") == '1';
    if (!s_pace)
        return;
    if (last && n < 512)
        dt[n++] = (uint32_t)(now - last);
    last = now;
    if (!t0)
        t0 = now;
    if (now - t0 >= 1000000 && n > 0) {
        int over = 0, over33 = 0, over50 = 0, i;
        qsort(dt, (size_t)n, sizeof dt[0], cmp_u32);
        for (i = 0; i < n; i++) {
            over += dt[i] > 20000;
            over33 += dt[i] > 33333;
            over50 += dt[i] > 50000;
        }
        fprintf(stderr, "[pace] game %d frames: ms p50 %.1f p95 %.1f max %.1f, >20 ms %d, p99 %.1f >33.3 ms %d >50 ms %d\n", n,
                dt[n / 2] / 1000.0, dt[n * 95 / 100] / 1000.0, dt[n - 1] / 1000.0, over,
                dt[n * 99 / 100] / 1000.0, over33, over50);
        n = 0;
        t0 = now;
    }
}

#ifndef CHIHIRO_RENDERER_VULKAN
static int s_srgb_control;              /* below (xr_init) */
static GLuint lend_claim(int w, int h);    /* below: the lent image, or 0 */
static void lend_filled(void);

void vc3_quest_frame(GLuint fbo, int pw, int ph, int eyes, int lw, int lh, const void *vr_frame)
{
    const Vc3VrFrame *vf = (const Vc3VrFrame *)vr_frame;
    static GLuint dst_fbo, lend_fbo;
    GLuint ltex;
    Frame *f = &s_frame[s_back];        /* this thread alone touches s_back */
    int t;

    pw *= eyes;                         /* the eyes are side by side */
    if (!dst_fbo)
        glGenFramebuffers(1, &dst_fbo);
    /* The host may still have a copy out of this texture queued on the GPU
     * (it was the host's frame until the last swap): the GPU waits for it
     * before this one overwrites it. Without it the headset showed frames
     * torn between two game frames -- the flicker. */
    {
        GLsync rd;
        pthread_mutex_lock(&s_lock);
        rd = f->read_done;
        f->read_done = 0;
        pthread_mutex_unlock(&s_lock);
        if (rd) {
            glWaitSync(rd, 0, GL_TIMEOUT_IGNORED);
            glDeleteSync(rd);
        }
    }
    f->direct = 0;
    f->w = pw;
    f->h = ph;
    if ((ltex = lend_claim(pw, ph))) {
        /* straight into the image the loop lent, upright (the surfaces hold
         * the picture top row first, GL's bottom) */
        static int srgb_off;
        if (!lend_fbo)
            glGenFramebuffers(1, &lend_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, lend_fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ltex, 0);
        if (s_srgb_control && !srgb_off) {   /* the colours are sRGB already */
            glDisable(GL_FRAMEBUFFER_SRGB_EXT);
            srgb_off = 1;
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glBlitFramebuffer(0, 0, pw, ph, 0, ph, pw, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        f->direct = 1;
    } else if (!f->tex || f->tw != pw || f->th != ph) {
        if (f->tex)
            glDeleteTextures(1, &f->tex);
        glGenTextures(1, &f->tex);
        glBindTexture(GL_TEXTURE_2D, f->tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, pw, ph);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        f->tw = pw;
        f->th = ph;
    }
    if (f->fence) {
        glDeleteSync(f->fence);
        f->fence = 0;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_fbo);
    if (!f->direct) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->tex, 0);
        glBlitFramebuffer(0, 0, pw, ph, 0, 0, pw, ph, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        /* VC3_QUEST_DUMP=first,count: those game frames (both eyes) as PPM
         * files beside the game, to see frame-to-frame changes (stalls) */
        static long first = -2, count, n;
        if (first == -2) {
            const char *v = ch_env("QUEST_DUMP");
            first = -1;
            if (v && sscanf(v, "%ld,%ld", &first, &count) != 2)
                first = -1;
        }
        if (first >= 0 && n >= first && n < first + count) {
            static unsigned char *px;
            char path[256], dir[200];
            snprintf(dir, sizeof dir, "%s/dump", chihiro_profile()->vr.sdcard_dir);
            FILE *o;
            int y;
            if (!px)
                px = (unsigned char *)malloc((size_t)pw * ph * 4);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, dst_fbo);
            glReadPixels(0, 0, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, px);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
            snprintf(path, sizeof path, "%s/f%04ld.ppm", dir, n - first);
            if (px && (o = fopen(path, "wb"))) {
                fprintf(o, "P6\n%d %d\n255\n", pw, ph);
                for (y = ph - 1; y >= 0; y--) {
                    int x;
                    for (x = 0; x < pw; x++)
                        fwrite(px + ((size_t)y * pw + x) * 4, 1, 3, o);
                }
                fclose(o);
            }
        }
        n++;
    }
    /* the HUD picture, beside the frame */
    f->has_hud = 0;
    if (vf && vf->imm && vf->hud_fbo) {
        if (!f->hud || f->hud_w != vf->hud_w || f->hud_h != vf->hud_h) {
            if (f->hud)
                glDeleteTextures(1, &f->hud);
            glGenTextures(1, &f->hud);
            glBindTexture(GL_TEXTURE_2D, f->hud);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, vf->hud_w, vf->hud_h);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            f->hud_w = vf->hud_w;
            f->hud_h = vf->hud_h;
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)vf->hud_fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->hud, 0);
        glBlitFramebuffer(0, 0, f->hud_w, f->hud_h, 0, 0, f->hud_w, f->hud_h,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        f->has_hud = 1;
    }
    if (vf)
        f->vr = *vf;
    else
        memset(&f->vr, 0, sizeof f->vr);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    f->lw = lw;
    f->lh = lh;
    f->eyes = eyes;
    f->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();                          /* the fence must reach the GPU */

    pace_game_frame();
    pthread_mutex_lock(&s_lock);
    if (f->direct)
        lend_filled();                  /* the loop releases it when it takes f */
    t = s_ready;
    s_ready = s_back;
    s_back = t;
    s_ready_seq++;
    s_frames_in++;
    pthread_mutex_unlock(&s_lock);
}

/* After the host's copy out of f is queued: a fence the back end's GPU
 * waits on before writing f again. */
static void frame_read_done(Frame *f)
{
    GLsync fs = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0), old;
    glFlush();                          /* the fence must reach the GPU */
    pthread_mutex_lock(&s_lock);
    old = f->read_done;
    f->read_done = fs;
    pthread_mutex_unlock(&s_lock);
    if (old)
        glDeleteSync(old);
}

/* The newest frame, on the host's thread; NULL before the first. With
 * allow 0 the one shown stays (the display's cadence, xr_frame). */
static Frame *take_frame(int *is_new, int allow)
{
    Frame *f;

    pthread_mutex_lock(&s_lock);
    *is_new = allow && s_ready_seq != s_shown_seq;
    if (*is_new) {
        int t = s_front;
        s_front = s_ready;
        s_ready = t;
        s_shown_seq = s_ready_seq;
    }
    pthread_mutex_unlock(&s_lock);
    f = &s_frame[s_front];
    if (!f->tex && !f->direct)
        return NULL;
    /* (a direct frame's image is released by xr_frame: lend_take) */
    /* (the read_done fence of the frame given up stays: the back end waits
     * on it before writing that frame again) */
    if (*is_new && f->fence)
        glWaitSync(f->fence, 0, GL_TIMEOUT_IGNORED);
    return f;
}

/* ── Settings: the screen, the gun ────────────────────────────────────── */

#endif

#ifdef CHIHIRO_RENDERER_VULKAN
static Frame *take_frame(int *is_new,int allow)
{
    static Frame frame;
    ChVkGpuFrame next={0};
    *is_new=0;
    if(allow && ch_vk_gpu_mailbox_take_ready(s_vk_frames,&next)==VK_SUCCESS) {
        ch_vk_gpu_frame_release(&frame.gpu);
        frame.gpu=next;
        if(s_pace>0) {   /* when the headset gets each new frame */
            static uint64_t last;
            uint64_t now=pace_now_us();
            uint64_t due_us=next.due_ns/1000;
            extern uint64_t xbox_HostNowNs(void);
            if(last) fprintf(stderr,"[xr take] seq=%llu interval_ms=%.3f after_due_ms=%.3f\n",
                (unsigned long long)next.sequence,(now-last)/1000.0,
                next.due_ns?(xbox_HostNowNs()/1000.0-due_us)/1000.0:-1.0);
            last=now;
        }
        frame.w=frame.lw=(int)next.surface.width;
        frame.h=frame.lh=(int)next.surface.height;
        frame.eyes=1;
        memset(&frame.vr,0,sizeof frame.vr);
        if(next.immersive) {   /* the eyes side by side; the game's view as drawn */
            const VrGameProj *g=&next.imm.projection;
            frame.w=(int)next.imm.eye_width*2;frame.h=(int)next.imm.eye_height;frame.eyes=2;
            frame.vr.imm=1;frame.vr.view=next.imm.view;
            frame.vr.gp[0]=g->kx;frame.vr.gp[1]=g->ky;frame.vr.gp[2]=g->cx;frame.vr.gp[3]=g->cy;frame.vr.gp[4]=g->inv_pz;
            frame.vr.gp_ok=next.imm.projection_ok;
            frame.vr.gw=(int)next.imm.guest_width;frame.vr.gh=(int)next.imm.guest_height;
            frame.vr.hud_w=(int)next.surface.width;frame.vr.hud_h=(int)next.surface.height;
        }
        s_frames_in=next.sequence;
        *is_new=1;
    }
    /* A frame from the bridge (the game on its own device) has no image,
     * only its slot's shared buffer: held is what counts. */
    if(frame.gpu.owner) {
        static int logged;
        if(!logged) {logged=1;fprintf(stderr,"[xr] first game frame for the screen: %dx%d, slot %u\n",frame.w,frame.h,frame.gpu.slot);}
    }
    return frame.gpu.owner?&frame:NULL;
}
#endif

static float s_dist = 2.6f, s_height = 1.9f;    /* metres */
/* Screen size "Fill view" (the default): the screen spans the eyes' whole
 * vertical field of view at any distance, as the world does in HOTD4. */
static int s_fill = 1;
#define EYE_DISTANCE 0.064f                       /* metres */

#ifndef CHIHIRO_RENDERER_VULKAN
void vc3_gl_stereo_separation(float ndc);         /* gl_backend.c */

#endif

enum { LASER_ON, LASER_DOT, LASER_OFF };
static const char *const SIGHT_KEY[QO_SIGHT_COUNT] = { "iron", "reddot", "holo" };
static const char *const LASER_KEY[3] = { "laser", "dot", "off" };

static struct {
    int sight;          /* QO_SIGHT_* */
    int laser;          /* LASER_* */
    int show_gun;
    int left_handed;
    int auto_coins;     /* a coin at start-up and with every Start */
    int immersive;      /* the game's 3D around the player; else on the screen */
    int refresh;        /* display Hz asked for: 120 (default), 90, 72 */
    int imm_scale;      /* immersive detail: 0 native (default), else 1x or 2x */
    int screen_native;  /* the virtual screen drawn at its size on the display
                           (default), else at the Resolution setting's scale */
    int show_stats;     /* the stats panel, head-locked top right (default on) */
    int eye;            /* Vulkan: each eye's picture n x n, 0 native (default); at the next start */
} s_vr = { QO_SIGHT_RED_DOT, LASER_DOT, 1, 0, 1, 1, 120, 0, 1, 1, 0 };
static int s_eye_boot;  /* s_vr.eye this run was started with */
/* The rifle (the profile's weapon; Silent Scope, a retail title on an Xbox
 * pad): always immersive, its gun always shown (the lens is its only
 * sight), no sight, laser, coin or arcade-button rows (menu_rows), its own
 * controls (rifle_frame). */
static int is_rifle(void)
{
    return chihiro_profile()->vr.weapon == CH_VR_RIFLE;
}
static float s_zoom = 1.0f;     /* the rifle's zoom, 0.5..4: kept across stages, 1 on Reset game */
/* The display's own pixels per eye (native_init): what "Native" draws. */
static uint32_t s_native_w, s_native_h;
/* The eyes' last immersive frame as drawn, % of their size each way: below
 * 100 when the Vulkan renderer's dynamic resolution has lowered it; 0 none. */
static int s_drawn_pct;

static int key_index(const char *v, const char *const *keys, int n, int dflt)
{
    int i;
    for (i = 0; v && i < n; i++)
        if (!strcmp(v, keys[i]))
            return i;
    return dflt;
}

static void screen_load(void)
{
    const char *v;
    if ((v = vc3_setting_get("vr_distance=")) && atof(v) > 0.5)
        s_dist = (float)atof(v);
    if ((v = vc3_setting_get("vr_height="))) {
        s_fill = strcmp(v, "fill") == 0;
        if (!s_fill && atof(v) > 0.3)
            s_height = (float)atof(v);
    }
    s_vr.sight = key_index(vc3_setting_get("vr_sight="), SIGHT_KEY, QO_SIGHT_COUNT,
                           QO_SIGHT_RED_DOT);
    s_vr.laser = key_index(vc3_setting_get("vr_laser="), LASER_KEY, 3, LASER_DOT);
    if ((v = vc3_setting_get("vr_gun=")))
        s_vr.show_gun = strcmp(v, "off") != 0;
    if ((v = vc3_setting_get("vr_hand=")))
        s_vr.left_handed = !strcmp(v, "left");
    if ((v = vc3_setting_get("vr_coins=")))
        s_vr.auto_coins = strcmp(v, "manual") != 0;
    if ((v = vc3_setting_get("vr_screen=")))
        s_vr.immersive = strcmp(v, "screen") != 0;
    if ((v = vc3_setting_get("vr_refresh=")) && atoi(v) >= 60 && atoi(v) <= 144)
        s_vr.refresh = atoi(v);
    /* vr_res= (vr_detail= before it: 1x-3x, 2x by default, not read, so a
     * settings file from then comes up at Native) */
    if ((v = vc3_setting_get("vr_res=")))
        s_vr.imm_scale = !strcmp(v, "1x") ? 1 : !strcmp(v, "2x") ? 2 : 0;
    if ((v = vc3_setting_get("vr_screen_res=")))
        s_vr.screen_native = strcmp(v, "scale") != 0;
    if ((v = vc3_setting_get("vr_stats=")))
        s_vr.show_stats = strcmp(v, "off") != 0;
    if ((v = vc3_setting_get("vr_eye=")))
        s_vr.eye = atoi(v) >= 256 ? atoi(v) : 0;        /* "native": 0 */
    s_eye_boot = s_vr.eye;
    if (is_rifle()) {           /* and no rows to change them */
        s_vr.immersive = 1;
        s_vr.show_gun = 1;
    }
}

static void screen_save(void)
{
    char b[32];
    snprintf(b, sizeof b, "%.2f", s_dist);
    vc3_setting_put("vr_distance=", b);
    if (s_fill)
        snprintf(b, sizeof b, "fill");
    else
        snprintf(b, sizeof b, "%.2f", s_height);
    vc3_setting_put("vr_height=", b);
}

static void vr_save(void)
{
    vc3_setting_put("vr_sight=", SIGHT_KEY[s_vr.sight]);
    vc3_setting_put("vr_laser=", LASER_KEY[s_vr.laser]);
    vc3_setting_put("vr_gun=", s_vr.show_gun ? "on" : "off");
    vc3_setting_put("vr_hand=", s_vr.left_handed ? "left" : "right");
    vc3_setting_put("vr_coins=", s_vr.auto_coins ? "auto" : "manual");
    vc3_setting_put("vr_screen=", s_vr.immersive ? "immersive" : "screen");
    vc3_setting_put("vr_stats=", s_vr.show_stats ? "on" : "off");
    {
        char b[16];
        snprintf(b, sizeof b, "%d", s_vr.refresh);
        vc3_setting_put("vr_refresh=", b);
        vc3_setting_put("vr_res=", s_vr.imm_scale == 1 ? "1x" : s_vr.imm_scale == 2 ? "2x"
                                                         : "native");
        vc3_setting_put("vr_screen_res=", s_vr.screen_native ? "native" : "scale");
        snprintf(b, sizeof b, "%d", s_vr.eye);
        vc3_setting_put("vr_eye=", s_vr.eye ? b : "native");
    }
    screen_save();
}

/* ── OpenXR ───────────────────────────────────────────────────────────── */

#define XRCHECK(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) { \
    fprintf(stderr, "[xr] %s: %d\n", #x, (int)r_); return 0; } } while (0)

enum { HAND_L, HAND_R };

static XrInstance s_inst;
static XrSystemId s_sys;
static XrSession s_sess;
static XrSpace s_local;
static XrSessionState s_state = XR_SESSION_STATE_UNKNOWN;
static int s_running;
static PFN_xrRequestDisplayRefreshRateFB s_request_rate;
static PFN_xrEnumerateDisplayRefreshRatesFB s_enum_rates;
static PFN_xrGetDisplayRefreshRateFB s_get_rate;
static float s_rate_want;               /* the rate rate_apply asked for, 0 none */
static PFN_xrPerfSettingsSetPerformanceLevelEXT s_perf_level;
#if defined(__ANDROID__)
static PFN_xrSetAndroidApplicationThreadKHR s_set_thread;
#endif
static float s_rates[8];
static int s_nrates;

#ifndef CHIHIRO_RENDERER_VULKAN
static XrSwapchain s_chain;
static uint32_t s_chain_gen;          /* bumped at every (re)creation */
static uint32_t s_chain_n, s_chain_w, s_chain_h;
static XrSwapchainImageOpenGLESKHR s_chain_img[8];
static GLuint s_chain_fbo[8], s_read_fbo;
static int64_t s_chain_fmt;
static int s_srgb_control;

#endif

static XrActionSet s_set;
static XrAction a_aim, a_trigger, a_grip, a_lower, a_upper, a_stick_xy, a_stick_click,
                a_menu, a_haptic;
static XrSpace s_aim_space[2];
static XrPath s_hand[2];
static int s_gun = HAND_R;

static XrPath xr_path(const char *s)
{
    XrPath p = XR_NULL_PATH;
    xrStringToPath(s_inst, s, &p);
    return p;
}

static int xr_action(XrAction *out, const char *name, const char *label, XrActionType type)
{
    XrActionCreateInfo ai = { XR_TYPE_ACTION_CREATE_INFO };
    ai.actionType = type;
    snprintf(ai.actionName, sizeof ai.actionName, "%s", name);
    snprintf(ai.localizedActionName, sizeof ai.localizedActionName, "%s", label);
    ai.countSubactionPaths = 2;
    ai.subactionPaths = s_hand;
    XRCHECK(xrCreateAction(s_set, &ai, out));
    return 1;
}

static int xr_input_init(void)
{
    static const struct { XrAction *a; const char *p; } B[] = {
        { &a_aim, "/user/hand/left/input/aim/pose" }, { &a_aim, "/user/hand/right/input/aim/pose" },
        { &a_trigger, "/user/hand/left/input/trigger/value" },
        { &a_trigger, "/user/hand/right/input/trigger/value" },
        { &a_grip, "/user/hand/left/input/squeeze/value" },
        { &a_grip, "/user/hand/right/input/squeeze/value" },
        { &a_lower, "/user/hand/left/input/x/click" }, { &a_lower, "/user/hand/right/input/a/click" },
        { &a_upper, "/user/hand/left/input/y/click" }, { &a_upper, "/user/hand/right/input/b/click" },
        { &a_stick_xy, "/user/hand/left/input/thumbstick" },
        { &a_stick_xy, "/user/hand/right/input/thumbstick" },
        { &a_stick_click, "/user/hand/left/input/thumbstick/click" },
        { &a_stick_click, "/user/hand/right/input/thumbstick/click" },
        { &a_menu, "/user/hand/left/input/menu/click" },
        { &a_haptic, "/user/hand/left/output/haptic" }, { &a_haptic, "/user/hand/right/output/haptic" },
    };
    XrActionSuggestedBinding sb[sizeof B / sizeof B[0]];
    XrInteractionProfileSuggestedBinding ps = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    XrActionSetCreateInfo si = { XR_TYPE_ACTION_SET_CREATE_INFO };
    XrSessionActionSetsAttachInfo at = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    unsigned i;
    int h;

    s_hand[HAND_L] = xr_path("/user/hand/left");
    s_hand[HAND_R] = xr_path("/user/hand/right");
    strcpy(si.actionSetName, "gun");
    strcpy(si.localizedActionSetName, "Gun");
    XRCHECK(xrCreateActionSet(s_inst, &si, &s_set));
    if (!xr_action(&a_aim, "aim", "Aim", XR_ACTION_TYPE_POSE_INPUT)
        || !xr_action(&a_trigger, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT)
        || !xr_action(&a_grip, "grip", "Grip (reload)", XR_ACTION_TYPE_FLOAT_INPUT)
        || !xr_action(&a_lower, "lower", "A / X (start)", XR_ACTION_TYPE_BOOLEAN_INPUT)
        || !xr_action(&a_upper, "upper", "B / Y (coin)", XR_ACTION_TYPE_BOOLEAN_INPUT)
        || !xr_action(&a_stick_xy, "stick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT)
        || !xr_action(&a_stick_click, "stick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT)
        || !xr_action(&a_menu, "menu", "Menu (test)", XR_ACTION_TYPE_BOOLEAN_INPUT)
        || !xr_action(&a_haptic, "recoil", "Recoil", XR_ACTION_TYPE_VIBRATION_OUTPUT))
        return 0;
    for (i = 0; i < sizeof B / sizeof B[0]; i++) {
        sb[i].action = *B[i].a;
        sb[i].binding = xr_path(B[i].p);
    }
    ps.interactionProfile = xr_path("/interaction_profiles/oculus/touch_controller");
    ps.countSuggestedBindings = sizeof B / sizeof B[0];
    ps.suggestedBindings = sb;
    XRCHECK(xrSuggestInteractionProfileBindings(s_inst, &ps));
    for (h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo ac = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        ac.action = a_aim;
        ac.subactionPath = s_hand[h];
        ac.poseInActionSpace.orientation.w = 1;
        XRCHECK(xrCreateActionSpace(s_sess, &ac, &s_aim_space[h]));
    }
    at.countActionSets = 1;
    at.actionSets = &s_set;
    XRCHECK(xrAttachSessionActionSets(s_sess, &at));
    return 1;
}

/* The display's own pixels per eye, which "Native" draws the game at: the
 * panel of the headset (OpenXR's recommended size is smaller, sized for its
 * lens-distortion pass), from the model; any other headset gets the
 * recommended size. Within the runtime's limits -- an eye's image rect and,
 * for the game's layer holding both eyes side by side, a swapchain image --
 * which are logged. VC3_NATIVE_EYE=WxH: that size instead. */
static void native_init(void)
{
    static const struct { const char *model; uint32_t w, h; } panel[] = {
        { "Quest 3S", 1832, 1920 },
        { "Quest 3", 2064, 2208 },
        { "Quest Pro", 1800, 1920 },
        { "Quest 2", 1832, 1920 },
        { "SteamVR/OpenXR : cv", 2160, 2160 },   /* Steam Frame (SteamVR recommends 1728) */
    };
    XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES };
    XrViewConfigurationView vcv[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW },
                                       { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
#if defined(__ANDROID__)
    char model[PROP_VALUE_MAX] = "";
#else
    char model[XR_MAX_SYSTEM_NAME_SIZE] = "";
#endif
    const char *e = ch_env("NATIVE_EYE");
    uint32_t nv = 0, mw, mh, i;
    unsigned ew, eh;

    xrGetSystemProperties(s_inst, s_sys, &sp);
    if (XR_FAILED(xrEnumerateViewConfigurationViews(s_inst, s_sys,
                                                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                    2, &nv, vcv)) || nv < 1) {
        fprintf(stderr, "[xr] no view configuration: native unknown\n");
        return;
    }
#if defined(__ANDROID__)
    __system_property_get("ro.product.model", model);
#else
    snprintf(model, sizeof model, "%s", sp.systemName);   /* no Quest panel here */
#endif
    s_native_w = vcv[0].recommendedImageRectWidth;
    s_native_h = vcv[0].recommendedImageRectHeight;
    for (i = 0; i < sizeof panel / sizeof panel[0]; i++)
        if (strstr(model, panel[i].model)) {
            s_native_w = panel[i].w;
            s_native_h = panel[i].h;
            break;
        }
    if (e && sscanf(e, "%ux%u", &ew, &eh) == 2 && ew >= 320 && eh >= 240) {
        s_native_w = ew;
        s_native_h = eh;
    }
    mw = vcv[0].maxImageRectWidth;
    mh = vcv[0].maxImageRectHeight;
    if (sp.graphicsProperties.maxSwapchainImageWidth / 2 < mw)
        mw = sp.graphicsProperties.maxSwapchainImageWidth / 2;
    if (sp.graphicsProperties.maxSwapchainImageHeight < mh)
        mh = sp.graphicsProperties.maxSwapchainImageHeight;
    fprintf(stderr, "[xr] %s (model %s): recommended eye %ux%u, max image rect %ux%u, max "
            "swapchain image %ux%u, max layers %u\n", sp.systemName, model,
            vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight,
            vcv[0].maxImageRectWidth, vcv[0].maxImageRectHeight,
            sp.graphicsProperties.maxSwapchainImageWidth,
            sp.graphicsProperties.maxSwapchainImageHeight, sp.graphicsProperties.maxLayerCount);
    if (s_native_w > mw || s_native_h > mh) {
        fprintf(stderr, "[xr] native %ux%u over the limits: %ux%u\n", s_native_w, s_native_h,
                s_native_w > mw ? mw : s_native_w, s_native_h > mh ? mh : s_native_h);
        if (s_native_w > mw)
            s_native_w = mw;
        if (s_native_h > mh)
            s_native_h = mh;
    }
    fprintf(stderr, "[xr] native eye %ux%u\n", s_native_w, s_native_h);
}

static int xr_init(void)
{
#if defined(__ANDROID__)
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    JavaVM *vm = NULL;
    jobject activity = (jobject)SDL_AndroidGetActivity();
    PFN_xrInitializeLoaderKHR init = NULL;
    XrLoaderInitInfoAndroidKHR li = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
    const char *ext[8] = { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
#ifdef CHIHIRO_RENDERER_VULKAN
                           XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
#else
                           XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME };
#endif
    uint32_t next = 2;
    XrInstanceCreateInfoAndroidKHR ai = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
#else
    const char *ext[8] = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
    uint32_t next = 1;
#endif
    int have_perf = 0, have_threads = 0;
    uint32_t n = 0, i;
    XrExtensionProperties *props;
    int have_rate = 0;
    XrInstanceCreateInfo ic = { XR_TYPE_INSTANCE_CREATE_INFO };
    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
#ifndef CHIHIRO_RENDERER_VULKAN
    PFN_xrGetOpenGLESGraphicsRequirementsKHR req_fn = NULL;
    XrGraphicsRequirementsOpenGLESKHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
    XrGraphicsBindingOpenGLESAndroidKHR gb = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
#endif

    XrSessionCreateInfo sc = { XR_TYPE_SESSION_CREATE_INFO };
    XrReferenceSpaceCreateInfo rc = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
#ifndef CHIHIRO_RENDERER_VULKAN
    const char *exts;
#endif

#if defined(__ANDROID__)
    if (!env || !activity)
        return 0;
    (*env)->GetJavaVM(env, &vm);
    XRCHECK(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                  (PFN_xrVoidFunction *)&init));
    li.applicationVM = vm;
    li.applicationContext = activity;
    XRCHECK(init((XrLoaderInitInfoBaseHeaderKHR *)&li));
#endif

    xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL);
    props = (XrExtensionProperties *)calloc(n ? n : 1, sizeof *props);
    if (!props)
        return 0;
    for (i = 0; i < n; i++)
        props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    xrEnumerateInstanceExtensionProperties(NULL, n, &n, props);
    for (i = 0; i < n; i++)
    {
        if (!strcmp(props[i].extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
            have_rate = 1;
        if (!strcmp(props[i].extensionName, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME))
            have_perf = 1;
#if defined(__ANDROID__)
        if (!strcmp(props[i].extensionName, XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME))
            have_threads = 1;
#endif
    }
    free(props);
    if (have_rate)
        ext[next++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
    if (have_perf)
        ext[next++] = XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME;
#if defined(__ANDROID__)
    if (have_threads)
        ext[next++] = XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME;

    ai.applicationVM = vm;
    ai.applicationActivity = activity;
    ic.next = &ai;
#endif
    ic.enabledExtensionCount = next;
    ic.enabledExtensionNames = ext;
    snprintf(ic.applicationInfo.applicationName, sizeof ic.applicationInfo.applicationName,
             "%s", chihiro_profile()->vr.app_name);
    strcpy(ic.applicationInfo.engineName, "xboxrecomp");
    ic.applicationInfo.applicationVersion = 1;
    ic.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    XRCHECK(xrCreateInstance(&ic, &s_inst));
    if (have_rate) {
        xrGetInstanceProcAddr(s_inst, "xrRequestDisplayRefreshRateFB",
                              (PFN_xrVoidFunction *)&s_request_rate);
        xrGetInstanceProcAddr(s_inst, "xrEnumerateDisplayRefreshRatesFB",
                              (PFN_xrVoidFunction *)&s_enum_rates);
        xrGetInstanceProcAddr(s_inst, "xrGetDisplayRefreshRateFB",
                              (PFN_xrVoidFunction *)&s_get_rate);
    }
    if (have_perf)
        xrGetInstanceProcAddr(s_inst, "xrPerfSettingsSetPerformanceLevelEXT",
                              (PFN_xrVoidFunction *)&s_perf_level);
#if defined(__ANDROID__)
    if (have_threads)
        xrGetInstanceProcAddr(s_inst, "xrSetAndroidApplicationThreadKHR",
                              (PFN_xrVoidFunction *)&s_set_thread);
#endif
    fprintf(stderr, "[xr] extensions: refresh rate %s, performance levels %s, thread hints %s\n",
            have_rate ? "yes" : "no", have_perf ? "yes" : "no", have_threads ? "yes" : "no");

    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XRCHECK(xrGetSystem(s_inst, &sgi, &s_sys));
#ifdef CHIHIRO_RENDERER_VULKAN
    VkResult vk_result=VK_SUCCESS;
    /* external memory: the game's own device shares buffers with this one */
    ChVkXrDeviceOptions device_options={.external_memory_fd=1};
    XrResult device_result=ch_vk_xr_device_create_with_options(s_inst,s_sys,xrGetInstanceProcAddr,
        &device_options,&s_vk,&vk_result);
    if(XR_FAILED(device_result) || vk_result!=VK_SUCCESS) {
        fprintf(stderr,"[xr] Vulkan device failed: XrResult=%d VkResult=%d\n",device_result,vk_result);
        return 0;
    }
    sc.next=&s_vk.binding;
#else
    XRCHECK(xrGetInstanceProcAddr(s_inst, "xrGetOpenGLESGraphicsRequirementsKHR",
                                  (PFN_xrVoidFunction *)&req_fn));
    XRCHECK(req_fn(s_inst, s_sys, &req));

    gb.display = s_dpy;
    gb.config = s_cfg;
    gb.context = s_ctx;
    sc.next = &gb;
#endif
    sc.systemId = s_sys;
    XRCHECK(xrCreateSession(s_inst, &sc, &s_sess));
    native_init();

    rc.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rc.poseInReferenceSpace.orientation.w = 1;
    XRCHECK(xrCreateReferenceSpace(s_sess, &rc, &s_local));
    if (!xr_input_init())
        return 0;

#ifdef CHIHIRO_RENDERER_VULKAN
    if(!vk_host_init()) return 0;
    fprintf(stderr,"[xr] native Vulkan session up\n");
#else
    /* The game's colours are already sRGB: an sRGB swapchain written raw
     * (sRGB write conversion off), else plain RGBA8. */
    {
        uint32_t nf = 0;
        int64_t *fmts;
        XRCHECK(xrEnumerateSwapchainFormats(s_sess, 0, &nf, NULL));
        fmts = (int64_t *)calloc(nf ? nf : 1, sizeof *fmts);
        if (!fmts)
            return 0;
        xrEnumerateSwapchainFormats(s_sess, nf, &nf, fmts);
        exts = (const char *)glGetString(GL_EXTENSIONS);
        s_srgb_control = exts && strstr(exts, "GL_EXT_sRGB_write_control") != NULL;
        for (i = 0; i < nf && s_srgb_control; i++)
            if (fmts[i] == GL_SRGB8_ALPHA8)
                s_chain_fmt = GL_SRGB8_ALPHA8;
        for (i = 0; i < nf && !s_chain_fmt; i++)
            if (fmts[i] == GL_RGBA8)
                s_chain_fmt = GL_RGBA8;
        free(fmts);
        if (!s_chain_fmt) {
            fprintf(stderr, "[xr] no 8-bit colour swapchain format\n");
            return 0;
        }
    }
    fprintf(stderr, "[xr] session up (%s swapchain)\n",
            s_chain_fmt == GL_SRGB8_ALPHA8 ? "sRGB" : "RGBA8");
#endif
    return 1;
}

#ifndef CHIHIRO_RENDERER_VULKAN
static int lend_revoke(void);            /* below */

/* A swapchain the size of the frames (they change with the resolution and
 * aspect settings). */
static int chain_fit(uint32_t w, uint32_t h)
{
    XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    uint32_t i;

    if (s_chain && s_chain_w == w && s_chain_h == h)
        return 1;
    if (s_chain && !lend_revoke())
        return 1;                       /* the back end has its image: next frame */
    if (s_chain) {
        glDeleteFramebuffers((GLsizei)s_chain_n, s_chain_fbo);
        xrDestroySwapchain(s_chain);
        s_chain = XR_NULL_HANDLE;
    }
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = s_chain_fmt;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XRCHECK(xrCreateSwapchain(s_sess, &ci, &s_chain));
    s_chain_gen++;
    XRCHECK(xrEnumerateSwapchainImages(s_chain, 0, &s_chain_n, NULL));
    if (s_chain_n > 8)
        s_chain_n = 8;
    for (i = 0; i < s_chain_n; i++)
        s_chain_img[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    XRCHECK(xrEnumerateSwapchainImages(s_chain, s_chain_n, &s_chain_n,
                                       (XrSwapchainImageBaseHeader *)s_chain_img));
    glGenFramebuffers((GLsizei)s_chain_n, s_chain_fbo);
    for (i = 0; i < s_chain_n; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_chain_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               s_chain_img[i].image, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_chain_w = w;
    s_chain_h = h;
    fprintf(stderr, "[xr] screen %ux%u\n", w, h);
    return 1;
}

/* ── Lent images: the back end's blit straight into the swapchain ─────────
 * VC3_XR_DIRECT=1; off by default. On a Quest 3 it held the frame rate and
 * the intro a little better (low10 42.5 against 40.5), but Meta's runtime
 * still objected -- 79 "Missing previous frame submission" and 44 "Failed
 * to retire next frame" in a run: it wants an image acquired, waited and
 * released within one frame, and a lent image spans two (acquired at the
 * end of one, released when the back end's frame is taken). Each
 * frame went through two full-size copies: the back end's into one of the
 * shared textures, this loop's from it into the swapchain. Instead this loop
 * lends the back end a swapchain image: it acquires and waits for one inside
 * its own frame (lend_out, between xrBeginFrame and xrEndFrame), the back
 * end blits the frame it presents into it if the size fits (lend_claim, then
 * lend_filled as it publishes the frame), and the loop releases it when it
 * takes that frame (lend_take), its layers then showing it. Acquire, wait and
 * release all stay on this thread and inside its frame loop, as Meta's
 * runtime wants: the back end acquiring and releasing on its own clock drew
 * "xrWaitSwapchainImage: Missing previous frame submission", then
 * "xrEndFrame frameTransaction failure ... swapchains not marked as
 * used-in-frame" and "Failed to retire next frame", and stalls of a second
 * or two a frame. A frame that finds no image lent, or one of another size,
 * takes the texture path; a filled image a newer frame overtook is released
 * before that frame's copy (lend_drop); a swapchain being replaced waits for
 * the back end to finish with its image (lend_revoke). State under s_lock. */
enum { LEND_NONE, LEND_OUT, LEND_FILLING, LEND_FILLED };
static struct {
    int state;
    uint32_t idx, w, h;
    GLuint tex;
} s_lend;
static int s_overlay_ok = -1;           /* -1 not yet tried (overlay_init) */

static int lend_on(void)
{
    static int on = -1;
    if (on < 0)
        on = ch_env("XR_DIRECT") && *ch_env("XR_DIRECT") == '1' && !ch_env("QUEST_DUMP");
    return on && s_overlay_ok > 0;
}

/* The back end's thread: the lent image for a w x h frame (its texture),
 * now the back end's to fill; 0: none (the texture path). */
static GLuint lend_claim(int w, int h)
{
    GLuint tex = 0;
    if (!lend_on())
        return 0;
    pthread_mutex_lock(&s_lock);
    if (s_lend.state == LEND_OUT && s_lend.w == (uint32_t)w && s_lend.h == (uint32_t)h) {
        s_lend.state = LEND_FILLING;
        tex = s_lend.tex;
    }
    pthread_mutex_unlock(&s_lock);
    return tex;
}

/* The back end's thread, s_lock held, as it publishes the frame it filled
 * the image with (whose fence covers the blit). */
static void lend_filled(void)
{
    if (s_lend.state == LEND_FILLING)
        s_lend.state = LEND_FILLED;
}

static void lend_release(void)
{
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(s_chain, &ri);
}

/* This loop, a new frame taken whose picture is in the lent image (its
 * fence waited on by take_frame): released, the layers show it. */
static int lend_take(void)
{
    int filled;
    pthread_mutex_lock(&s_lock);
    filled = s_lend.state == LEND_FILLED;
    if (filled)
        s_lend.state = LEND_NONE;
    pthread_mutex_unlock(&s_lock);
    if (filled)
        lend_release();
    return filled;
}

/* This loop, a new frame taken that is not in the lent image: a filled one
 * (an older frame's) released first, the new frame's copy goes after it. */
static void lend_drop(void)
{
    int filled;
    pthread_mutex_lock(&s_lock);
    filled = s_lend.state == LEND_FILLED;
    if (filled)
        s_lend.state = LEND_NONE;
    pthread_mutex_unlock(&s_lock);
    if (filled)
        lend_release();
}

/* This loop, before replacing the swapchain: an image lent and unused back
 * (released: the old swapchain goes anyway); 0 while the back end has it. */
static int lend_revoke(void)
{
    int st;
    pthread_mutex_lock(&s_lock);
    st = s_lend.state;
    if (st == LEND_OUT)
        s_lend.state = LEND_NONE;
    pthread_mutex_unlock(&s_lock);
    if (st == LEND_OUT)
        lend_release();
    return st == LEND_OUT || st == LEND_NONE;
}

/* This loop, inside its frame: an image lent for the back end's next frame
 * when none is. */
static void lend_out(void)
{
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    uint32_t idx = 0;
    int none;
    if (!lend_on() || !s_chain)
        return;
    pthread_mutex_lock(&s_lock);
    none = s_lend.state == LEND_NONE;
    pthread_mutex_unlock(&s_lock);
    if (!none || XR_FAILED(xrAcquireSwapchainImage(s_chain, &ai, &idx)) || idx >= s_chain_n)
        return;
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(s_chain, &wi))) {
        fprintf(stderr, "[xr] lending: waiting for the image failed\n");
        return;
    }
    pthread_mutex_lock(&s_lock);
    s_lend.idx = idx;
    s_lend.tex = s_chain_img[idx].image;
    s_lend.w = s_chain_w;
    s_lend.h = s_chain_h;
    s_lend.state = LEND_OUT;
    pthread_mutex_unlock(&s_lock);
}

/* The display rate (the panel's Refresh, vr_refresh=): 120 by default --
 * the game's 60 frames shown exactly twice each, with the cadence lock in
 * xr_frame -- or 90 / 72 when asked; only rates the headset offers. */
#endif

#ifdef CHIHIRO_RENDERER_VULKAN
static int s_overlay_ok = -1;
#endif

static void rate_apply(void)
{
    int i, best = -1;
    if (!s_request_rate)
        return;
    for (i = 0; i < s_nrates; i++)
        if (best < 0 || fabsf(s_rates[i] - (float)s_vr.refresh) < fabsf(s_rates[best] - (float)s_vr.refresh))
            best = i;
    if (best >= 0 && XR_SUCCEEDED(s_request_rate(s_sess, s_rates[best]))) {
        s_rate_want = s_rates[best];
        fprintf(stderr, "[xr] display %.0f Hz\n", s_rates[best]);
    } else if (s_nrates == 0 && XR_SUCCEEDED(s_request_rate(s_sess, (float)s_vr.refresh))) {
        s_rate_want = (float)s_vr.refresh;
        fprintf(stderr, "[xr] display %d Hz\n", s_vr.refresh);
    }
}

/* Once a second while running: the display at the rate asked for. SteamVR
 * (Steam Frame) puts every app at its per-app default, 72 Hz, as it becomes
 * the scene app -- after the request at the session's start -- so the
 * request is made again when the display is found elsewhere; a few times,
 * then left to the runtime. */
static void rate_watch(void)
{
    static uint64_t last;
    static int tries;
    float now_rate = 0;
    uint64_t t = SDL_GetTicks64();
    if (!s_get_rate || !s_request_rate || s_rate_want <= 0
        || !s_running || t - last < 1000)
        return;
    last = t;
    if (XR_FAILED(s_get_rate(s_sess, &now_rate)) || fabsf(now_rate - s_rate_want) < 1.0f) {
        tries = 0;
        return;
    }
    if (tries >= 5) {
        if (tries++ == 5)
            fprintf(stderr, "[xr] display stays at %.0f Hz, not %.0f: left to the runtime\n",
                    now_rate, s_rate_want);
        return;
    }
    tries++;
    fprintf(stderr, "[xr] display at %.0f Hz, not %.0f: asking again (%s)\n", now_rate,
            s_rate_want, XR_SUCCEEDED(s_request_rate(s_sess, s_rate_want)) ? "accepted" : "refused");
}

/* At the session's start: the rates on offer, the display rate, and the CPU
 * and GPU held high (VC3_XR_PERF: sustained (default), boost, or off). */
static void perf_begin(void)
{
    const char *e = ch_env("XR_PERF");
    if (s_enum_rates) {
        uint32_t n = 0;
        if (XR_SUCCEEDED(s_enum_rates(s_sess, 0, &n, NULL)) && n) {
            if (n > 8)
                n = 8;
            if (XR_SUCCEEDED(s_enum_rates(s_sess, n, &n, s_rates)))
                s_nrates = (int)n;
            fprintf(stderr, "[xr] display rates offered:");
            for (int k = 0; k < s_nrates; k++)
                fprintf(stderr, " %.0f", s_rates[k]);
            fputc('\n', stderr);
        }
    }
    rate_apply();
    if (s_perf_level && !(e && !strcmp(e, "off"))) {
        XrPerfSettingsLevelEXT lv = e && !strcmp(e, "boost") ? XR_PERF_SETTINGS_LEVEL_BOOST_EXT
                                                              : XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
        XrResult rc = s_perf_level(s_sess, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, lv);
        XrResult rg = s_perf_level(s_sess, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, lv);
        fprintf(stderr, "[xr] performance %s: CPU %d, GPU %d\n",
                lv == XR_PERF_SETTINGS_LEVEL_BOOST_EXT ? "boost" : "sustained high", (int)rc, (int)rg);
    }
}

/* The fastest cores (highest cpuinfo_max_freq), as a mask; 0 if unknown. */
static unsigned long big_cores(void)
{
    unsigned long mask = 0;
    long best = 0;
    int c;
    for (c = 0; c < 16; c++) {
        char path[96];
        long f = 0;
        FILE *fp;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
        if (!(fp = fopen(path, "r")))
            continue;
        if (fscanf(fp, "%ld", &f) == 1) {
            if (f > best) {
                best = f;
                mask = 0;
            }
            if (f == best)
                mask |= 1ul << c;
        }
        fclose(fp);
    }
    return mask;
}

/* A GL-side worker thread's hint. XR_KHR_android_thread_settings has a
 * renderer-worker type, but Meta's runtime rejects it ("Unsupported
 * threadType; 4" in its log, XR_ERROR_ANDROID_THREAD_SETTINGS_FAILURE_KHR):
 * so the renderer-worker type first (a runtime that has it), then the
 * application-worker one, then, if the runtime takes neither, the thread's
 * niceness raised (setpriority, -10: Android's urgent-display level is -8,
 * display -4). Each attempt logged. */
#if defined(__ANDROID__)
static void hint_worker(pid_t tid, const char *what)
{
    static const struct { XrAndroidThreadTypeKHR type; const char *name; } ty[2] = {
        { XR_ANDROID_THREAD_TYPE_RENDERER_WORKER_KHR, "renderer worker" },
        { XR_ANDROID_THREAD_TYPE_APPLICATION_WORKER_KHR, "application worker" },
    };
    int i;
    for (i = 0; i < 2; i++) {
        XrResult r = s_set_thread(s_sess, ty[i].type, (uint32_t)tid);
        fprintf(stderr, "[xr] thread hint: %s as %s: %s (%d)\n", what, ty[i].name,
                XR_SUCCEEDED(r) ? "accepted" : "rejected", (int)r);
        if (XR_SUCCEEDED(r))
            return;
    }
    {
        static const int nice_try[3] = { -10, -8, -4 };
        int k;
        for (k = 0; k < 3; k++) {
            if (setpriority(PRIO_PROCESS, (id_t)tid, nice_try[k]) == 0) {
                fprintf(stderr, "[xr] thread hint: %s: niceness %d instead\n", what, nice_try[k]);
                return;
            }
        }
        fprintf(stderr, "[xr] thread hint: %s: niceness not raised either (errno %d)\n", what,
                errno);
    }
}
#endif

/* Each headset frame until done: tell the runtime which thread is which
 * once each exists -- the loop (it submits the frames) the renderer's main,
 * the GL back end and the push-buffer decode workers (hint_worker), the game
 * the application's main -- and with VC3_BIGCORES=1 put the game and the
 * back end on the fastest cores. */
static void perf_frame(void)
{
    static int pinned;
#if defined(__ANDROID__)
    static int told_host, told_exec, told_decode, told_guest;
    if (s_set_thread) {
        /* each when its thread exists; logged as the runtime answers */
        if (!told_host && s_tid_host) {
            XrResult r = s_set_thread(s_sess, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR,
                                      (uint32_t)s_tid_host);
            told_host = 1;
            fprintf(stderr, "[xr] thread hint: loop as renderer main: %d\n", (int)r);
        }
        if (!told_exec && s_tid_exec) {
            told_exec = 1;
            hint_worker(s_tid_exec, "GL back end");
        }
        /* the decode thread, when it is not the GL thread: a renderer
         * worker too (it feeds the GL thread every draw of the frame) */
        if (!told_decode && s_tid_decode && s_tid_exec && s_tid_decode != s_tid_exec) {
            told_decode = 1;
            hint_worker(s_tid_decode, "push-buffer decode");
        }
        if (!told_guest && s_tid_guest) {
            XrResult r = s_set_thread(s_sess, XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR,
                                      (uint32_t)s_tid_guest);
            told_guest = 1;
            fprintf(stderr, "[xr] thread hint: game as application main: %d\n", (int)r);
        }
    }
#endif
    if (!pinned && s_tid_exec && s_tid_guest) {
        const char *e = ch_env("BIGCORES");
        pinned = 1;
        if (e && *e == '1') {
            unsigned long m = big_cores();
            cpu_set_t set;
            int c, a, b;
            CPU_ZERO(&set);
            for (c = 0; c < 16; c++)
                if (m & (1ul << c))
                    CPU_SET(c, &set);
            a = m ? sched_setaffinity(s_tid_guest, sizeof set, &set) : -1;
            b = m ? sched_setaffinity(s_tid_exec, sizeof set, &set) : -1;
            if (m && s_tid_decode && s_tid_decode != s_tid_exec)
                sched_setaffinity(s_tid_decode, sizeof set, &set);
            fprintf(stderr, "[xr] game and back end on cores %#lx: %d %d (0 done)\n", m, a, b);
        }
    }
}

static void xr_events(void)
{
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };

    while (xrPollEvent(s_inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            s_state = ((XrEventDataSessionStateChanged *)&ev)->state;
            fprintf(stderr, "[xr] session state %d\n", (int)s_state);
            if (s_state == XR_SESSION_STATE_READY && !s_running) {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(s_sess, &bi))) {
                    s_running = 1;
                    perf_begin();
                }
            } else if (s_state == XR_SESSION_STATE_STOPPING && s_running) {
                xrEndSession(s_sess);
                s_running = 0;
            } else if (s_state == XR_SESSION_STATE_EXITING
                       || s_state == XR_SESSION_STATE_LOSS_PENDING) {
                s_quit = 1;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            s_quit = 1;
        } else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            /* which controllers the runtime bound the actions to, per hand
             * (on a Steam Frame, SteamVR's remap of the Touch bindings) */
            int h;
            for (h = 0; h < 2; h++) {
                XrInteractionProfileState ip = { XR_TYPE_INTERACTION_PROFILE_STATE };
                char name[XR_MAX_PATH_LENGTH] = "none";
                uint32_t len = 0;
                if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(s_sess, s_hand[h], &ip))
                    && ip.interactionProfile != XR_NULL_PATH)
                    xrPathToString(s_inst, ip.interactionProfile, sizeof name, &len, name);
                fprintf(stderr, "[xr] %s hand: %s\n", h == HAND_L ? "left" : "right", name);
            }
        }
        ev = (XrEventDataBuffer){ XR_TYPE_EVENT_DATA_BUFFER };
    }
}

static float act_float(XrAction a, int h)
{
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    XrActionStateFloat st = { XR_TYPE_ACTION_STATE_FLOAT };
    gi.action = a;
    gi.subactionPath = s_hand[h];
    return XR_SUCCEEDED(xrGetActionStateFloat(s_sess, &gi, &st)) && st.isActive
        ? st.currentState : 0.0f;
}

static int act_bool(XrAction a, int h)
{
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    XrActionStateBoolean st = { XR_TYPE_ACTION_STATE_BOOLEAN };
    gi.action = a;
    gi.subactionPath = s_hand[h];
    return XR_SUCCEEDED(xrGetActionStateBoolean(s_sess, &gi, &st)) && st.isActive
        && st.currentState;
}

static XrVector2f act_vec2(XrAction a, int h)
{
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    XrActionStateVector2f st = { XR_TYPE_ACTION_STATE_VECTOR2F };
    XrVector2f z = { 0, 0 };
    gi.action = a;
    gi.subactionPath = s_hand[h];
    return XR_SUCCEEDED(xrGetActionStateVector2f(s_sess, &gi, &st)) && st.isActive
        ? st.currentState : z;
}

static void rumble(int h, float amp, int ms)
{
    XrHapticVibration v = { XR_TYPE_HAPTIC_VIBRATION };
    XrHapticActionInfo hi = { XR_TYPE_HAPTIC_ACTION_INFO };
    v.amplitude = amp;
    v.duration = (XrDuration)ms * 1000000;
    v.frequency = XR_FREQUENCY_UNSPECIFIED;
    hi.action = a_haptic;
    hi.subactionPath = s_hand[h];
    xrApplyHapticFeedback(s_sess, &hi, (XrHapticBaseHeader *)&v);
}

/* q applied to v */
static void qrot(XrQuaternionf q, const float v[3], float o[3])
{
    float tx = 2 * (q.y * v[2] - q.z * v[1]);
    float ty = 2 * (q.z * v[0] - q.x * v[2]);
    float tz = 2 * (q.x * v[1] - q.y * v[0]);
    o[0] = v[0] + q.w * tx + (q.y * tz - q.z * ty);
    o[1] = v[1] + q.w * ty + (q.z * tx - q.x * tz);
    o[2] = v[2] + q.w * tz + (q.x * ty - q.y * tx);
}

/* ── Views, the anchor, the overlay layer ─────────────────────────────── */

static XrView s_views[2];
static int s_views_ok;
static void screen_fill_apply(void)
{
    float up = 0, down = 0;
    int e;
    if (!s_fill || !s_views_ok)
        return;
    for (e = 0; e < 2; e++) {
        float u = tanf(s_views[e].fov.angleUp), d = -tanf(s_views[e].fov.angleDown);
        if (u > up) up = u;
        if (d > down) down = d;
    }
    if (up + down > 0.2f && up + down < 20.0f)
        s_height = s_dist * (up + down);
}

/* Where the screen hangs: straight ahead of the anchor, s_dist away, its
 * centre at the anchor's height. The anchor is the LOCAL origin until
 * "Recentre view" puts it where the head is, facing where it faces. */
static float s_anchor_p[3], s_anchor_yaw;

static void yaw_rot(float yaw, const float v[3], float o[3])
{
    float c = cosf(yaw), s = sinf(yaw);
    o[0] = c * v[0] + s * v[2];
    o[1] = v[1];
    o[2] = -s * v[0] + c * v[2];
}

static void head_pose(float p[3], float *yaw)
{
    static const float back[3] = { 0, 0, -1 };
    float f[3];
    int i;
    for (i = 0; i < 3; i++)
        p[i] = 0.5f * ((&s_views[0].pose.position.x)[i] + (&s_views[1].pose.position.x)[i]);
    qrot(s_views[0].pose.orientation, back, f);
    *yaw = atan2f(-f[0], -f[2]);
}

static void recentre(void)
{
    if (!s_views_ok)
        return;
    head_pose(s_anchor_p, &s_anchor_yaw);
    fprintf(stderr, "[xr] recentred: %.2f %.2f %.2f, yaw %.0f deg\n", s_anchor_p[0],
            s_anchor_p[1], s_anchor_p[2], s_anchor_yaw * 57.2958f);
}

/* ── The immersive view (vr_view.h) ───────────────────────────────────────
 * The back end draws each frame through the newest head pose it can get:
 * this, called on its thread at each flip, hands it the eyes relative to
 * the anchor -- the game's camera -- as matrices. The headset loop updates
 * the pose every display frame (view_publish). The frame then carries the
 * pose it was drawn with, and the headset's layer says so, so the
 * compositor turns it to where the head is when it is shown. */
#define IMM_NEAR 0.05f                  /* metres */
#define IMM_FAR 2000.0f
#define HUD_DIST 5.0f                   /* the HUD panel, metres from the anchor */
/* The longest lens the HUD panel follows, in 640x480 pixels: the profile's
 * lens_k (VC3: its usual 45 x 34.5 degree view, 320 / tan 22.5 degrees). */
#define HUD_K_MAX (chihiro_profile()->vr.lens_k > 0.0f ? chihiro_profile()->vr.lens_k : 772.5f)
/* Immersive aim: how far from the screen's centre the gun still shoots, in
 * screens each way (1.5: 3 screens wide and high -- at VC3's usual lens 51
 * degrees either side, 43 up and down); beyond, the gun is off the screen
 * (a pull reloads). The game's own reading stops at the screen's edge; the
 * point beyond goes to the game directly (the profile's vr.aim_screen). */
#define AIM_WIDE 1.5
static pthread_mutex_t s_view_lock = PTHREAD_MUTEX_INITIALIZER;
static XrView s_pub_views[2];
static int s_pub_ok;
static float s_pub_anchor_p[3], s_pub_anchor_yaw;
static uint64_t s_pub_id;
static XrFovf s_pub_fov;                /* the left eye's, for vc3_vr_screen_px */
static int s_pub_fov_ok;

static float imm_units(void)
{
    static float u = -1;
    if (u < 0) {
        const char *e = ch_env("IMM_UNITS");     /* game units per metre */
        float d = chihiro_profile()->gpu.units_per_m;
        u = e && atof(e) > 0 ? (float)atof(e) : d > 0.0f ? d : 1.0f;
    }
    return u;
}

static void view_publish(void)
{
    pthread_mutex_lock(&s_view_lock);
    s_pub_ok = s_views_ok && s_vr.immersive && s_running;
    s_pub_fov_ok = s_views_ok;
    if (s_views_ok)
        s_pub_fov = s_views[0].fov;
    if (s_pub_ok) {
        memcpy(s_pub_views, s_views, sizeof s_pub_views);
        memcpy(s_pub_anchor_p, s_anchor_p, sizeof s_pub_anchor_p);
        s_pub_anchor_yaw = s_anchor_yaw;
        s_pub_id++;
    }
    pthread_mutex_unlock(&s_view_lock);
}

/* The game's CPU culling while immersive (the profile's vr.cull): wide
 * enough for the head to turn. VC3_IMM_CULL_FOV: the vertical angle, degrees. */
static void imm_cull(int on)
{
    static float half = -1;
    if (half < 0) {
        const char *e = ch_env("IMM_CULL_FOV");
        float d = e ? (float)atof(e) : 140.0f;
        half = d >= 10.0f && d <= 175.0f ? d * 0.5f * 3.14159265f / 180.0f : 0.0f;
    }
    if (chihiro_profile()->vr.cull)
        chihiro_profile()->vr.cull(on ? half : 0.0f);
}

int vc3_vr_view_latch(Vc3VrView *v)
{
    XrView views[2];
    float ap[3], ay, qa_inv[4];
    int ok, e, k;

    pthread_mutex_lock(&s_view_lock);
    ok = s_pub_ok;
    if (ok) {
        memcpy(views, s_pub_views, sizeof views);
        memcpy(ap, s_pub_anchor_p, sizeof ap);
        ay = s_pub_anchor_yaw;
        v->id = s_pub_id;
    }
    pthread_mutex_unlock(&s_view_lock);
    imm_cull(ok);
    if (!ok)
        return v->on = 0;
    qa_inv[0] = 0;
    qa_inv[1] = -sinf(ay * 0.5f);
    qa_inv[2] = 0;
    qa_inv[3] = cosf(ay * 0.5f);
    for (e = 0; e < 2; e++) {
        const float qe[4] = { views[e].pose.orientation.x, views[e].pose.orientation.y,
                              views[e].pose.orientation.z, views[e].pose.orientation.w };
        float rel[3];
        for (k = 0; k < 3; k++)
            rel[k] = (&views[e].pose.position.x)[k] - ap[k];
        vr_qmul(qa_inv, qe, v->pose_q[e]);
        yaw_rot(-ay, rel, v->pose_p[e]);
        v->fov[e][0] = views[e].fov.angleLeft;
        v->fov[e][1] = views[e].fov.angleRight;
        v->fov[e][2] = views[e].fov.angleUp;
        v->fov[e][3] = views[e].fov.angleDown;
        for (k = 0; k < 4; k++)
            v->tan_fov[e][k] = tanf(v->fov[e][k]);
        vr_eye_matrix(v->pose_q[e], v->pose_p[e], v->tan_fov[e], imm_units(), IMM_NEAR, IMM_FAR,
                      v->vp[e]);
    }
    v->scale = s_vr.imm_scale;
    v->eye_w = (int)s_native_w;
    v->eye_h = (int)s_native_h;
    return v->on = 1;
}

/* The virtual screen on the display: its angular size (height s_height at
 * s_dist, the picture's aspect) in the display's pixels per tangent at the
 * middle of the newest view. */
int vc3_vr_screen_px(int *w, int *h)
{
    XrView v = { XR_TYPE_VIEW };
    float tx, ty, aspect = vc3_wide_get() ? 16.0f / 9.0f : 4.0f / 3.0f;
    int ok;
    if (!s_vr.screen_native || !s_native_w || !s_native_h)
        return 0;
    pthread_mutex_lock(&s_view_lock);
    ok = s_pub_fov_ok;
    v.fov = s_pub_fov;
    pthread_mutex_unlock(&s_view_lock);
    if (!ok)
        return 0;
    tx = tanf(v.fov.angleRight) - tanf(v.fov.angleLeft);
    ty = tanf(v.fov.angleUp) - tanf(v.fov.angleDown);
    if (!(tx > 0.1f) || !(ty > 0.1f) || !(s_dist > 0.1f))
        return 0;
    *h = (int)((float)s_native_h / ty * s_height / s_dist + 0.5f);
    *w = (int)((float)s_native_w / tx * s_height * aspect / s_dist + 0.5f);
    return *w > 0 && *h > 0;
}

/* An eye of a frame's view, back in LOCAL (with the anchor as it is now). */
static void view_to_local(const Vc3VrView *v, int e, XrPosef *pose)
{
    float qa[4] = { 0, sinf(s_anchor_yaw * 0.5f), 0, cosf(s_anchor_yaw * 0.5f) }, q[4], p[3];
    vr_qmul(qa, v->pose_q[e], q);
    yaw_rot(s_anchor_yaw, v->pose_p[e], p);
    pose->orientation.x = q[0];
    pose->orientation.y = q[1];
    pose->orientation.z = q[2];
    pose->orientation.w = q[3];
    pose->position.x = p[0] + s_anchor_p[0];
    pose->position.y = p[1] + s_anchor_p[1];
    pose->position.z = p[2] + s_anchor_p[2];
}

/* The HUD panel of an immersive frame, in the anchor's frame: the game's
 * own view at HUD_DIST, so its marks lie over what they mark. */
static int hud_panel(const Frame *f, float c[3], float *w, float *h)
{
    const Vc3VrFrame *vf = &f->vr;
    VrGameProj g;
    float u = imm_units(), v[3];
    if (!vf->gp_ok || vf->gw <= 0 || vf->gh <= 0)
        return 0;
    g.kx = vf->gp[0]; g.ky = vf->gp[1]; g.cx = vf->gp[2]; g.cy = vf->gp[3]; g.inv_pz = vf->gp[4];
    if (!(fabsf(g.kx) > 1e-3f) || !(fabsf(g.ky) > 1e-3f) || !(fabsf(g.inv_pz) > 1e-9f))
        return 0;
    vr_unproject_gl(&g, vf->gw * 0.5f, vf->gh * 0.5f, HUD_DIST * u / fabsf(g.inv_pz), v);
    c[0] = v[0] / u;
    c[1] = v[1] / u;
    c[2] = v[2] / u;
    /* the HUD is drawn as wide as the picture (16:9: the middle 4:3 is the
     * game's view), sized by the frame's projection -- its marks over what
     * they mark. With the world following game zooms (the Vulkan
     * renderer's default) that projection's lens is never longer than the
     * usual 45 degrees; without (CHIHIRO_VULKAN_IMMERSIVE_ZOOM=0) the panel
     * keeps that size when the game's is longer: sized by the close-up's
     * ~14 degrees, "CONTINUE?" shrank to a small patch. */
    {
        float kx = fminf(fabsf(g.kx), HUD_K_MAX * vf->gw / 640.0f);
        float ky = fminf(fabsf(g.ky), HUD_K_MAX * vf->gh / 480.0f);
        *w = HUD_DIST * (float)f->lw / kx;
        *h = HUD_DIST * (float)vf->gh / ky;
    }
    return 1;
}

/* Shared GL/Vulkan display policy: preserve the last image and render pose
 * between half-rate overlay updates at refresh rates above 83 Hz. */
static int overlay_reuse_frame(int have_last,int full,int menu_open,uint32_t frame,XrDuration period)
{
    return have_last && !full && !menu_open && period>0 && period<12000000 && (frame&1);
}

#ifndef CHIHIRO_RENDERER_VULKAN
/* The overlay's layer: one swapchain per eye, multisampled where the GPU
 * resolves it on chip (EXT_multisampled_render_to_texture, free on Adreno). */
typedef struct {
    XrSwapchain chain;
    uint32_t n, w, h;
    XrSwapchainImageOpenGLESKHR img[8];
    GLuint fbo[8], depth;
} Eye;
static Eye s_eye[2];

static int overlay_init(void)
{
    XrViewConfigurationView vcv[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW },
                                       { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC fb_ms = NULL;
    PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC rb_ms = NULL;
    const char *exts = (const char *)glGetString(GL_EXTENSIONS);
    uint32_t nv = 0, i;
    int e, samples = 4;

    if (!qo_init())
        return 0;
    XRCHECK(xrEnumerateViewConfigurationViews(s_inst, s_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                              2, &nv, vcv));
    if (exts && strstr(exts, "GL_EXT_multisampled_render_to_texture")) {
        fb_ms = (PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC)
            eglGetProcAddress("glFramebufferTexture2DMultisampleEXT");
        rb_ms = (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC)
            eglGetProcAddress("glRenderbufferStorageMultisampleEXT");
    }
    if (!fb_ms || !rb_ms)
        samples = 1;
    for (e = 0; e < 2; e++) {
        Eye *y = &s_eye[e];
        XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        /* the display's own size (VC3_OVERLAY_SCALE=0.5 etc.: that part of
         * it): the gun and the panel as sharp as the world behind them */
        {
            const char *sc = ch_env("OVERLAY_SCALE");
            float f = sc && atof(sc) > 0.2 && atof(sc) <= 1.0 ? (float)atof(sc) : 1.0f;
            uint32_t nw = s_native_w ? s_native_w : vcv[e].recommendedImageRectWidth;
            uint32_t nh = s_native_h ? s_native_h : vcv[e].recommendedImageRectHeight;
            y->w = (uint32_t)(nw * f);
            y->h = (uint32_t)(nh * f);
        }
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        ci.format = s_chain_fmt;
        ci.sampleCount = 1;
        ci.width = y->w;
        ci.height = y->h;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        XRCHECK(xrCreateSwapchain(s_sess, &ci, &y->chain));
        XRCHECK(xrEnumerateSwapchainImages(y->chain, 0, &y->n, NULL));
        if (y->n > 8)
            y->n = 8;
        for (i = 0; i < y->n; i++)
            y->img[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        XRCHECK(xrEnumerateSwapchainImages(y->chain, y->n, &y->n,
                                           (XrSwapchainImageBaseHeader *)y->img));
        glGenRenderbuffers(1, &y->depth);
        glBindRenderbuffer(GL_RENDERBUFFER, y->depth);
        if (samples > 1)
            rb_ms(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, (GLsizei)y->w, (GLsizei)y->h);
        else
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (GLsizei)y->w, (GLsizei)y->h);
        glGenFramebuffers((GLsizei)y->n, y->fbo);
        for (i = 0; i < y->n; i++) {
            glBindFramebuffer(GL_FRAMEBUFFER, y->fbo[i]);
            if (samples > 1)
                fb_ms(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, y->img[i].image, 0, samples);
            else
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                       y->img[i].image, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, y->depth);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                fprintf(stderr, "[xr] overlay framebuffer incomplete\n");
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                return 0;
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    fprintf(stderr, "[xr] overlay %ux%u per eye, %dx MSAA\n", s_eye[0].w, s_eye[0].h, samples);
    return 1;
}

#endif

/* ── The settings panel ───────────────────────────────────────────────────
 * The left controller's menu button opens it to the player's left, turned
 * towards them, the game's screen still in view (as in The House of the
 * Dead 4 VR). Point the gun at a row and pull the trigger; the game gets no
 * input meanwhile. Holding the other hand's upper button (B / Y) for a
 * second opens and closes it too. */
enum { M_SCREEN, M_DETAIL, M_REFRESH, M_SIGHT, M_LASER, M_GUN, M_HAND, M_STEREO, M_ASPECT, M_RES, M_DIST, M_SIZE,
       M_COINS, M_COIN, M_RECENTRE, M_TEST, M_SERVICE, M_RESET, M_CLOSE, M_STATS, M_EYE, M_CONTROLS, M_COUNT };
/* The rows shown, in The House of the Dead 4 VR's order. The Vulkan
 * renderer has the immersive view (Screen) and HOTD4's Resolution per eye
 * but no stereo screen, render scale or 16:9, so its panel has only the
 * items that act -- and room for Controls, a page of what each button does. */
#ifdef CHIHIRO_RENDERER_VULKAN
static const int s_rows[] = { M_SCREEN, M_SIGHT, M_LASER, M_GUN, M_HAND, M_STATS, M_DIST, M_SIZE, M_REFRESH,
                              M_EYE, M_COINS, M_COIN, M_RECENTRE, M_TEST, M_SERVICE, M_RESET, M_CONTROLS,
                              M_CLOSE };
#else
static const int s_rows[] = { M_SCREEN, M_DETAIL, M_REFRESH, M_SIGHT, M_LASER, M_GUN, M_HAND, M_STEREO,
                              M_ASPECT, M_RES, M_DIST, M_SIZE, M_COINS, M_COIN, M_RECENTRE, M_TEST,
                              M_SERVICE, M_RESET, M_CLOSE };
#endif
#define MENU_ROWS ((int)(sizeof s_rows / sizeof s_rows[0]))
/* The rows shown, n of them: the rifle's without Screen, Sight, Laser, Gun
 * model, the coins and the arcade buttons. */
static int menu_rows(int rows[MENU_ROWS])
{
    int i, n = 0;
    for (i = 0; i < MENU_ROWS; i++) {
        int m = s_rows[i];
        if (is_rifle() && (m == M_SCREEN || m == M_SIGHT || m == M_LASER || m == M_GUN || m == M_COINS
                           || m == M_COIN || m == M_TEST || m == M_SERVICE))
            continue;
        rows[n++] = m;
    }
    return n;
}
static struct {
    int open, hover, dirty;
    float c[3], right[3], up[3], n[3], hw, hh;  /* where it hangs, LOCAL metres */
    int test_frames, service_frames, coin;
    int reset_armed;            /* "Reset game" chosen once: a second time resets */
    int page;                   /* 0 the settings, 1 the controls */
} s_menu;

/* The controls page: what each button does, named for the hand the gun is
 * in (A / B on the right controller, X / Y on the left); its last row,
 * Back, returns to the settings. */
#define CTRL_ROWS 12
static void controls_items(XmItem *it)
{
    static char held[2][24];
    const int right = s_gun == HAND_R;
    static const char *const label[CTRL_ROWS] = {
        "Gun trigger", "Gun grip", NULL, NULL, "Stick click (either)", "Left menu button", "Other hand trigger",
        "Other hand grip", NULL, NULL, "Other hand stick", "Back" };
    static const char *const value[CTRL_ROWS] = {
        "Fire; off the screen, reload", "Reload", "Foot pedal (E.S. mode)", "CHANGE button", NULL,
        "Settings panel", "Gun to that hand", "Foot pedal", "Recentre view", "Settings panel",
        "Screen distance, size", "" };
    int i;
    snprintf(held[0], sizeof held[0], "Other hand %s, held", right ? "X" : "A");
    snprintf(held[1], sizeof held[1], "Other hand %s, held", right ? "Y" : "B");
    for (i = 0; i < CTRL_ROWS; i++) {
        it[i].label = label[i];
        snprintf(it[i].value, 40, "%s", value[i] ? value[i] : "");
    }
    it[2].label = right ? "Gun hand A" : "Gun hand X";
    it[3].label = right ? "Gun hand B" : "Gun hand Y";
    snprintf(it[4].value, 40, "%s", s_vr.auto_coins ? "Start (with a coin)" : "Start");
    it[8].label = held[0];
    it[9].label = held[1];
}

/* The rifle's controls page (rifle_frame): the gun hand fires, zooms and
 * answers the game's menus; the other hand's buttons are as above. */
#define RIFLE_CTRL_ROWS 10
static void rifle_controls_items(XmItem *it)
{
    static char held[2][24];
    const int right = s_gun == HAND_R;
    static const char *const label[RIFLE_CTRL_ROWS] = {
        "Trigger", "Thumbstick up/down", NULL, "Stick click (either)", "Left menu button",
        "Other hand trigger", NULL, NULL, "Other hand stick", "Back" };
    static const char *const value[RIFLE_CTRL_ROWS] = {
        "Fire", "Zoom (menus: move)", "Confirm / Back", "Start", "Settings panel",
        "Gun to that hand", "Recentre view", "Settings panel", "Screen distance, size", "" };
    int i;
    snprintf(held[0], sizeof held[0], "Other hand %s, held", right ? "X" : "A");
    snprintf(held[1], sizeof held[1], "Other hand %s, held", right ? "Y" : "B");
    for (i = 0; i < RIFLE_CTRL_ROWS; i++) {
        it[i].label = label[i];
        snprintf(it[i].value, 40, "%s", value[i]);
    }
    it[2].label = right ? "A / B" : "X / Y";
    it[6].label = held[0];
    it[7].label = held[1];
}
static int ctrl_rows(void)
{
    return is_rifle() ? RIFLE_CTRL_ROWS : CTRL_ROWS;
}

static void menu_items(XmItem *it)
{
    static const char *const sight[] = { "Iron sights", "Red dot", "Holographic" };
    static const char *const laser[] = { "Laser and dot", "Dot only", "Off" };
    static const char *const labels[M_COUNT] = {
        "Screen", "Immersive detail", "Display rate", "Sight", "Laser", "Gun model", "Gun hand", "Stereo 3D", "Aspect", "Resolution",
        "Screen distance", "Screen size", "Coins", "Insert a coin", "Recentre view",
        "Arcade Test button (select)", "Arcade Service button (next)", "Reset game", "Close", "Show stats",
        "Resolution per eye", "Controls" };
    int i;
    for (i = 0; i < M_COUNT; i++) {
        it[i].label = labels[i];
        it[i].value[0] = 0;
    }
    snprintf(it[M_SCREEN].value, 40, "%s", s_vr.immersive ? "Immersive" : "Virtual screen");
    /* Native: the display's own pixels; 1x / 2x: the game's 640 across an
     * eye spanning the whole view, as a share of them */
    if (!s_vr.imm_scale && s_drawn_pct > 0 && s_drawn_pct < 100)
        snprintf(it[M_DETAIL].value, 40, "Native, now at %d%%", s_drawn_pct);
    else if (!s_vr.imm_scale)
        snprintf(it[M_DETAIL].value, 40, "Native (%ux%u)", s_native_w, s_native_h);
    else if (s_native_w)
        snprintf(it[M_DETAIL].value, 40, "%dx (%u%% of native)", s_vr.imm_scale,
                 (unsigned)(640u * (unsigned)s_vr.imm_scale * 100u / s_native_w));
    else
        snprintf(it[M_DETAIL].value, 40, "%dx", s_vr.imm_scale);
    snprintf(it[M_REFRESH].value, 40, "%d Hz%s", s_vr.refresh,
             s_vr.refresh == 120 ? " (smoothest)" : "");
    snprintf(it[M_SIGHT].value, 40, "%s", sight[s_vr.sight]);
    snprintf(it[M_LASER].value, 40, "%s", laser[s_vr.laser]);
    snprintf(it[M_GUN].value, 40, "%s", s_vr.show_gun ? "Shown" : "Hidden");
    snprintf(it[M_HAND].value, 40, "%s", s_gun == HAND_L ? "Left" : "Right");
    snprintf(it[M_STATS].value, 40, "%s", s_vr.show_stats ? "On" : "Off");
    snprintf(it[M_STEREO].value, 40, "%s", s_vr.immersive ? "On (immersive)"
             : vc3_stereo_get() ? "On" : "Off");
    snprintf(it[M_ASPECT].value, 40, "%s", vc3_wide_get() ? "16:9" : "4:3");
    if (s_vr.screen_native)
        snprintf(it[M_RES].value, 40, "Native");
    else
        snprintf(it[M_RES].value, 40, "%dx%s", vc3_scale_get(),
                 vc3_scale_get() > 1 && vc3_stereo_get() ? " (slow in 3D)" : "");
    snprintf(it[M_DIST].value, 40, "%.1f m", s_dist);
    if (s_fill)
        snprintf(it[M_SIZE].value, 40, "Fill view");
    else
        snprintf(it[M_SIZE].value, 40, "%.1f m tall", s_height);
    snprintf(it[M_COINS].value, 40, "%s", s_vr.auto_coins ? "Automatic" : "By menu");
    if (!s_vr.eye && s_eye_boot)        /* the eyes are made at start-up */
        snprintf(it[M_EYE].value, 40, "Native (on restart)");
    else if (!s_vr.eye)
        snprintf(it[M_EYE].value, 40, "Native (%u)", s_native_w);
    else
        snprintf(it[M_EYE].value, 40, "%d%s", s_vr.eye, s_vr.eye == s_eye_boot ? "" : " (on restart)");
    if (s_menu.reset_armed)
        snprintf(it[M_RESET].value, 40, "Pull again to reset");
}

/* VC3_XR_DUMP=<dir>: the panel's picture (panel.ppm) and, once, the left
 * eye's overlay (overlay.ppm), for checking them without a headset. */
static void dump_ppm(const char *name, const unsigned char *rgba, int w, int h)
{
    const char *dir = ch_env("XR_DUMP");
    char path[512];
    FILE *fp;
    int i;
    if (!dir || !*dir)
        return;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (!(fp = fopen(path, "wb")))
        return;
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    for (i = 0; i < w * h; i++) {       /* over mid grey, so alpha shows */
        unsigned a = rgba[i * 4 + 3], k;
        for (k = 0; k < 3; k++) {
            unsigned c = rgba[i * 4 + k] + (128u * (255u - a)) / 255u;
            fputc((int)(c > 255 ? 255 : c), fp);
        }
    }
    fclose(fp);
    fprintf(stderr, "[xr] dumped %s\n", path);
}
static void menu_paint(void)
{
    static unsigned char img[XM_W * XM_H * 4];
    XmItem it[M_COUNT], shown[M_COUNT];
    int i;
    if (s_menu.page) {
        if (is_rifle())
            rifle_controls_items(shown);
        else
            controls_items(shown);
        xm_paint(img, "Controls", shown, ctrl_rows(), s_menu.hover, "Point at Back, pull the trigger.");
    } else {
        int rows[MENU_ROWS], n = menu_rows(rows);
        menu_items(it);
        for (i = 0; i < n; i++)
            shown[i] = it[rows[i]];
        xm_paint(img, chihiro_profile()->vr.panel_title, shown, n, s_menu.hover,
                 "Point, pull the trigger. Menu button: close.");
    }
    dump_ppm("panel.ppm", img, XM_W, XM_H);
#ifdef CHIHIRO_RENDERER_VULKAN
    vk_menu_upload(img);
#else
    qo_panel_upload(img, XM_W, XM_H);
#endif
    s_menu.dirty = 0;
}

static void menu_place(void)
{
    float head[3], yaw, fwd[3];
    int i;
    head_pose(head, &yaw);
    yaw += 0.55f;                               /* about 30 degrees to the left */
    fwd[0] = -sinf(yaw);
    fwd[1] = 0;
    fwd[2] = -cosf(yaw);
    for (i = 0; i < 3; i++) {
        s_menu.c[i] = head[i] + fwd[i] * 0.95f;
        s_menu.n[i] = -fwd[i];
    }
    s_menu.c[1] = head[1] - 0.12f;
    s_menu.up[0] = 0; s_menu.up[1] = 1; s_menu.up[2] = 0;
    s_menu.right[0] = s_menu.up[1] * s_menu.n[2] - s_menu.up[2] * s_menu.n[1];
    s_menu.right[1] = s_menu.up[2] * s_menu.n[0] - s_menu.up[0] * s_menu.n[2];
    s_menu.right[2] = s_menu.up[0] * s_menu.n[1] - s_menu.up[1] * s_menu.n[0];
    s_menu.hw = 0.33f;
    s_menu.hh = s_menu.hw * XM_H / XM_W;
}

static void menu_toggle(const char *why)
{
    s_menu.open = !s_menu.open;
    s_menu.reset_armed = 0;
    s_menu.page = 0;
    if (s_menu.open) {
        menu_place();
        s_menu.hover = -1;
        s_menu.dirty = 1;
    }
    fprintf(stderr, "[xr] settings panel %s (%s)\n", s_menu.open ? "open" : "closed", why);
}

/* where the ray meets the panel: 0..1 from its top left; 0 if it misses */
static int menu_hit(const float p[3], const float d[3], float *u, float *v, float hit[3])
{
    float dn = d[0] * s_menu.n[0] + d[1] * s_menu.n[1] + d[2] * s_menu.n[2], t, r[3], x, y;
    int i;
    if (dn > -1e-4f)
        return 0;
    t = ((s_menu.c[0] - p[0]) * s_menu.n[0] + (s_menu.c[1] - p[1]) * s_menu.n[1]
         + (s_menu.c[2] - p[2]) * s_menu.n[2]) / dn;
    if (t <= 0)
        return 0;
    for (i = 0; i < 3; i++) {
        hit[i] = p[i] + d[i] * t;
        r[i] = hit[i] - s_menu.c[i];
    }
    x = (r[0] * s_menu.right[0] + r[1] * s_menu.right[1] + r[2] * s_menu.right[2]) / s_menu.hw;
    y = (r[0] * s_menu.up[0] + r[1] * s_menu.up[1] + r[2] * s_menu.up[2]) / s_menu.hh;
    *u = (x + 1) * 0.5f;
    *v = (1 - y) * 0.5f;
    return x >= -1 && x <= 1 && y >= -1 && y <= 1;
}

static void cycle(float *v, const float *opts, int n)
{
    int k = 0;
    while (k < n && opts[k] <= *v + 0.01f)
        k++;
    *v = opts[k % n];
}

static void menu_choose(int item)
{
    static const float dists[] = { 1.8f, 2.2f, 2.6f, 3.2f, 4.0f, 5.0f };
    static const float sizes[] = { 1.4f, 1.9f, 2.4f, 3.0f, 3.8f };
    int armed = s_menu.reset_armed;

    s_menu.reset_armed = 0;             /* anything else chosen in between: no reset */
    switch (item) {
    case M_SCREEN: s_vr.immersive = !s_vr.immersive; break;
    case M_DETAIL: s_vr.imm_scale = (s_vr.imm_scale + 1) % 3; break;   /* native, 1x, 2x */
    case M_REFRESH:
        s_vr.refresh = s_vr.refresh >= 120 ? 72 : s_vr.refresh >= 90 ? 120 : 90;
        rate_apply();
        break;
    case M_SIGHT: s_vr.sight = (s_vr.sight + 1) % QO_SIGHT_COUNT; break;
    case M_LASER: s_vr.laser = (s_vr.laser + 1) % 3; break;
    case M_GUN: s_vr.show_gun = !s_vr.show_gun; break;
    case M_STATS: s_vr.show_stats = !s_vr.show_stats; break;
    case M_HAND:
        s_gun = 1 - s_gun;
        s_vr.left_handed = s_gun == HAND_L;
        break;
    case M_STEREO: vc3_stereo_set(!vc3_stereo_get()); break;
    case M_ASPECT: vc3_wide_set(!vc3_wide_get()); break;
    case M_RES:         /* native, 1x .. VC3_SCALE_MAX */
        if (s_vr.screen_native) {
            s_vr.screen_native = 0;
            vc3_scale_set(1);
        } else if (vc3_scale_get() >= VC3_SCALE_MAX) {
            s_vr.screen_native = 1;
        } else {
            vc3_scale_set(vc3_scale_get() + 1);
        }
        break;
    case M_DIST: cycle(&s_dist, dists, 6); break;
    case M_SIZE:        /* the fixed sizes, then Fill view */
        if (s_fill) {
            s_fill = 0;
            s_height = sizes[0];
        } else if (s_height >= sizes[4] - 0.01f) {
            s_fill = 1;
        } else {
            cycle(&s_height, sizes, 5);
        }
        break;
    case M_EYE: {       /* HOTD4's sizes, then the panel's own (native) */
        static const int eyes[] = { 1216, 1440, 1680, 1920, 0 };
        int k = 0;
        while (s_vr.eye && k < 4 && eyes[k] <= s_vr.eye)
            k++;
        s_vr.eye = eyes[k];
        break;
    }
    case M_CONTROLS: s_menu.page = 1; s_menu.hover = -1; break;
    case M_COINS: s_vr.auto_coins = !s_vr.auto_coins; break;
    case M_COIN: s_menu.coin = 1; break;
    case M_RECENTRE: recentre(); break;
    case M_TEST: s_menu.test_frames = 12; break;
    case M_SERVICE: s_menu.service_frames = 12; break;
    case M_RESET:
        if (!armed) {
            s_menu.reset_armed = 1;
            break;
        }
        s_zoom = 1.0f;
        vr_save();
        vc3_quest_restart();
        break;
    case M_CLOSE: s_menu.open = 0; break;
    }
    vr_save();
    s_menu.dirty = 1;
}

/* ── The gun ──────────────────────────────────────────────────────────────
 * As in The House of the Dead 4 VR: trigger fires (pulled with the gun off
 * the screen it reloads, as pointing a light gun away does); grip reloads;
 * A / X on the gun hand is the cabinet's foot pedal (the other hand's grip
 * is too), B / Y on the gun hand its CHANGE button; a thumbstick click is
 * Start (with automatic coins, a coin with it); the left menu button opens
 * the settings. A trigger on the other hand moves the gun there. The other
 * hand's thumbstick moves the screen (up/down: nearer/farther; left/right:
 * smaller/larger); its A / X held for a second recentres the view. */
static float s_hit_u = -1, s_hit_v = -1;
static QoFrame s_qo;
static XrTime s_shot_t;

/* ── The rifle ────────────────────────────────────────────────────────────
 * The profile's weapon (Silent Scope: a retail title on an Xbox pad). Its
 * shot leaves the game's camera, so only the rifle's direction counts: in
 * the anchor's frame, which the immersive view makes the game camera's GL
 * view space, steadied, to the game as yaw and pitch every frame (the
 * profile's vr.rifle; off while the panel is open). The gun hand's trigger
 * is the pad's right trigger (fire), its A / B (X / Y) the pad's A / B, a
 * stick click either hand Start; its thumbstick up/down zooms while the
 * scope picture is up (the newest immersive frame brought one), else is
 * the d-pad, as left/right always is (retail_pad.c). The grip does
 * nothing: the game reloads by itself. The other hand is as for VC3
 * (gun_frame). d: the rifle's direction (LOCAL), NULL when not located. */
static void rifle_frame(XrTime t, const Frame *f, const float *d, int now[2][5], int was[2][5])
{
    static VrSteady steady;
    static XrTime last_t;
    ChVrRifle r = { 0 };
    RetailPadKeys keys;
    float sticks[2][2], st[2], dt = (float)((double)(t - last_t) / 1e9);
    int h, buttons = 0;
    const int scope_on = f && f->vr.imm && s_qo.lens_fresh;
    if (!last_t || !(dt > 0.0f))
        dt = 1.0f / 72.0f;      /* the first frame: a display frame's worth */
    if (dt > 0.1f)
        dt = 0.1f;              /* back from a pause: no leap in the zoom */
    last_t = t;
    for (h = 0; h < 2; h++) {
        XrVector2f v = act_vec2(a_stick_xy, h);
        sticks[h][0] = v.x;
        sticks[h][1] = v.y;
    }
    retail_pad_stick(s_gun, sticks, st);
    if (scope_on && !s_menu.open)
        s_zoom = vr_zoom_step(s_zoom, st[1], dt);
    if (d) {
        float ld[3], aim[3];
        yaw_rot(-s_anchor_yaw, d, ld);
        vr_steady(&steady, ld, dt, s_zoom, aim);
        vr_aim_angles(aim, &r.yaw, &r.pitch);
        r.on = !s_menu.open;
    }
    r.zoom = s_zoom;
    if (chihiro_profile()->vr.rifle)
        chihiro_profile()->vr.rifle(&r);

    if (now[s_gun][0])
        buttons |= RETAIL_PAD_TRIGGER;
    if (now[s_gun][1])
        buttons |= RETAIL_PAD_A;
    if (now[s_gun][2])
        buttons |= RETAIL_PAD_B;
    if (now[0][3] || now[1][3])
        buttons |= RETAIL_PAD_START;
    if (s_menu.open)
        buttons |= RETAIL_PAD_PANEL;
    retail_pad_map(buttons, st[0], st[1], scope_on, &keys);
    retail_pad_apply(&keys);
    if (keys.rt && !was[s_gun][0])
        rumble(s_gun, 0.8f, 25);
}

/* No controllers to read (the session not focused, or the sync failed):
 * every pad key released and the rifle off, its zoom kept for when they
 * come back -- the game runs on meanwhile. */
static void rifle_release(void)
{
    static const RetailPadKeys none = { 0 };
    ChVrRifle r = { 0 };
    r.zoom = s_zoom;
    retail_pad_apply(&none);
    if (chihiro_profile()->vr.rifle)
        chihiro_profile()->vr.rifle(&r);
}

static void gun_frame(XrTime t, const Frame *f)
{
    static int was[2][5], was_menu, was_start;
    static float stick_cool;
    static XrTime panel_held, recentre_held;
    static unsigned frames;
    XrActiveActionSet as = { 0 };
    XrActionsSyncInfo sy = { XR_TYPE_ACTIONS_SYNC_INFO };
    double ux = -1, uy = -1;
    int aim_on = 0, aim_x = 0, aim_y = 0;   /* immersive: the game's aim, pixels from its centre, y up */
    int buttons = 0, h, off, now[2][5], menu_btn, start, coin, on_screen = 0;
    float o[3], d[3];
    int located = 0;
    const int rifle = is_rifle();

    static int fake_gun = -1;   /* VC3_XR_FAKE_GUN=1: test runs, nobody holding anything */
    if (fake_gun < 0)
        fake_gun = ch_env("XR_FAKE_GUN") && *ch_env("XR_FAKE_GUN") == '1';
    as.actionSet = s_set;
    sy.countActiveActionSets = 1;
    sy.activeActionSets = &as;
    /* (with nobody wearing the headset the sync fails: the fake gun goes on, no buttons) */
    if (XR_FAILED(xrSyncActions(s_sess, &sy)) && !fake_gun) {
        if (rifle)
            rifle_release();
        return;
    }
    for (h = 0; h < 2; h++) {
        now[h][0] = act_float(a_trigger, h) > 0.55f;
        now[h][1] = act_bool(a_lower, h);
        now[h][2] = act_bool(a_upper, h);
        now[h][3] = act_bool(a_stick_click, h);
        now[h][4] = act_float(a_grip, h) > 0.6f;
    }
    /* The hand that pulls its trigger is the gun hand. */
    off = 1 - s_gun;
    if (now[off][0] && !was[off][0] && !s_menu.open) {
        s_gun = off;
        off = 1 - s_gun;
        s_vr.left_handed = s_gun == HAND_L;
        vr_save();
    }
    menu_btn = act_bool(a_menu, HAND_L);
    {   /* VC3_XR_FAKE_MENU=1: the panel opens by itself, for a test */
        static int fake_menu = -1;
        if (fake_menu < 0)
            fake_menu = ch_env("XR_FAKE_MENU") && *ch_env("XR_FAKE_MENU") == '1';
        if (fake_menu == 1 && s_views_ok && frames > 600) {
            fake_menu = 2;
            menu_toggle("test");
        }
    }
    if (menu_btn && !was_menu)
        menu_toggle("menu button");
    was_menu = menu_btn;
    if (now[off][2] && !was[off][2])
        panel_held = t;
    if (now[off][2] && panel_held && t - panel_held >= 1000000000) {
        panel_held = 0;
        menu_toggle("held button");
        rumble(off, 0.3f, 20);
    }
    if (now[off][1] && !was[off][1])    /* its lower button (A / X) held: Recentre view */
        recentre_held = t;
    if (now[off][1] && recentre_held && t - recentre_held >= 1000000000) {
        recentre_held = 0;
        recentre();
        rumble(off, 0.3f, 20);
    }

    s_hit_u = s_hit_v = -1;
    s_qo.gun_valid = 0;
    {
        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        int got = XR_SUCCEEDED(xrLocateSpace(s_aim_space[s_gun], s_local, t, &loc))
            && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
            && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        /* VC3_XR_FAKE_GUN=1, for a headset nobody holds: a gun in front of
         * the eyes, raised to aim through its sight, sweeping slowly */
        static int fake = -1;
        static float fake_yaw;          /* VC3_XR_FAKE_GUN_YAW=degrees: aimed that far left */
        if (fake < 0) {
            fake = ch_env("XR_FAKE_GUN") && *ch_env("XR_FAKE_GUN") == '1';
            fake_yaw = ch_env("XR_FAKE_GUN_YAW") ? (float)atof(ch_env("XR_FAKE_GUN_YAW")) * 0.0174533f : 0.0f;
        }
        if (!got && fake) {
            double sec = (double)t / 1e9;
            float yaw = fake_yaw + (float)(0.06 * sin(sec * 0.5)), pitch = 0.0f;
            float sh = qo_sight_height(s_vr.sight);
            loc.pose.orientation = (XrQuaternionf){ 0, sinf(yaw / 2), 0, cosf(yaw / 2) };
            (void)pitch;
            loc.pose.position = (XrVector3f){ 0.0f, -sh - 0.002f, -0.30f };
            got = 1;
        }
        if (got) {  /* a unit quaternion, finite: the overlay (vk_overlay.c) refuses others */
            XrQuaternionf *q = &loc.pose.orientation;
            float n = sqrtf(q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w);
            if (n > 1e-3f && isfinite(n) && isfinite(loc.pose.position.x)
                && isfinite(loc.pose.position.y) && isfinite(loc.pose.position.z)) {
                q->x /= n;
                q->y /= n;
                q->z /= n;
                q->w /= n;
            } else {
                got = 0;
            }
        }
        if (got) {
            static const float fwd[3] = { 0, 0, -1 };
            float up[3] = { 0, qo_sight_height(s_vr.sight), 0 }, lift[3];
            int i;
            /* the aim is the sight's line: the bore raised to it */
            qrot(loc.pose.orientation, fwd, d);
            qrot(loc.pose.orientation, up, lift);
            o[0] = loc.pose.position.x + lift[0];
            o[1] = loc.pose.position.y + lift[1];
            o[2] = loc.pose.position.z + lift[2];
            located = 1;
            s_qo.gun_valid = 1;
            s_qo.gun_q[0] = loc.pose.orientation.x;
            s_qo.gun_q[1] = loc.pose.orientation.y;
            s_qo.gun_q[2] = loc.pose.orientation.z;
            s_qo.gun_q[3] = loc.pose.orientation.w;
            s_qo.gun_p[0] = loc.pose.position.x;
            s_qo.gun_p[1] = loc.pose.position.y;
            s_qo.gun_p[2] = loc.pose.position.z;
            for (i = 0; i < 3; i++)
                s_qo.end[i] = o[i] + d[i] * 5.0f;
        }
    }
    if (located && f) {
        /* into the anchor's frame, where the screen lies at z = -s_dist */
        float rel[3] = { o[0] - s_anchor_p[0], o[1] - s_anchor_p[1], o[2] - s_anchor_p[2] };
        float lo[3], ld[3];
        float hw = s_height * (float)f->lw / (float)f->lh * 0.5f, hh = s_height * 0.5f;
        yaw_rot(-s_anchor_yaw, rel, lo);
        yaw_rot(-s_anchor_yaw, d, ld);
        if (ld[2] < -1e-4f) {
            float tt = (-s_dist - lo[2]) / ld[2];
            if (tt > 0) {
                float x = lo[0] + tt * ld[0], y = lo[1] + tt * ld[1];
                int i;
                s_hit_u = (x + hw) / (2 * hw);
                s_hit_v = (hh - y) / (2 * hh);
                if (s_hit_u >= -0.5f && s_hit_u <= 1.5f && s_hit_v >= -0.5f && s_hit_v <= 1.5f)
                    for (i = 0; i < 3; i++)
                        s_qo.end[i] = o[i] + d[i] * tt;
            }
        }
    }
    if (located && f && f->vr.imm) {
        /* Immersive: the gun's ray meets the HUD panel's plane (the game's
         * view at HUD_DIST); the game shoots from its camera through that
         * point, so its screen position is the point through the game's
         * own projection. */
        float rel[3] = { o[0] - s_anchor_p[0], o[1] - s_anchor_p[1], o[2] - s_anchor_p[2] };
        float lo[3], ld[3], pc[3], pw_, ph_;
        s_hit_u = s_hit_v = -1;
        yaw_rot(-s_anchor_yaw, rel, lo);
        yaw_rot(-s_anchor_yaw, d, ld);
        if (hud_panel(f, pc, &pw_, &ph_) && ld[2] < -1e-4f) {
            float tt = (pc[2] - lo[2]) / ld[2];
            if (tt > 0) {
                const Vc3VrFrame *vf = &f->vr;
                VrGameProj g = { vf->gp[0], vf->gp[1], vf->gp[2], vf->gp[3], vf->gp[4] };
                float u = imm_units(), v[3], sx, sy;
                int i;
                for (i = 0; i < 3; i++) {
                    v[i] = (lo[i] + ld[i] * tt) * u;
                    s_qo.end[i] = o[i] + d[i] * tt;
                }
                if (vr_project_gl(&g, v, &sx, &sy)) {
                    double gx = sx / vf->gw, gy = sy / vf->gh;
                    if (fabs(gx - 0.5) <= AIM_WIDE && fabs(gy - 0.5) <= AIM_WIDE) {
                        /* the gun's reading at the screen's nearest point; the
                         * game's aim the true one, beyond the screen too */
                        ux = gx < 0 ? 0 : gx > 1 ? 1 : gx;
                        uy = gy < 0 ? 0 : gy > 1 ? 1 : gy;
                        on_screen = 1;
                        aim_on = 1;
                        aim_x = (int)lround((gx - 0.5) * 640.0);
                        aim_y = (int)lround((0.5 - gy) * 480.0);
                    }
                }
            }
        }
    } else if (f && s_hit_u >= 0 && s_hit_u <= 1 && s_hit_v >= 0 && s_hit_v <= 1) {
        double gw = f->lh * 4.0 / 3.0, gx = s_hit_u * f->lw - (f->lw - gw) / 2.0;
        on_screen = 1;
        if (gx >= 0 && gx < gw) {
            ux = gx / gw;
            uy = s_hit_v;
        }
    }
    s_qo.beam = s_vr.laser == LASER_ON;
    /* the laser's dot on the screen; with an optic on, only with the beam
     * (the optic's own reticle shows where the shot goes) */
    s_qo.dot = on_screen && (s_vr.laser == LASER_ON
                             || (s_vr.laser == LASER_DOT && s_vr.sight == QO_SIGHT_IRON));
    s_qo.sight = s_vr.sight;
    s_qo.show_gun = s_vr.show_gun;
    if (rifle)                  /* the lens is its sight */
        s_qo.beam = s_qo.dot = 0;

    if (s_menu.open) {          /* the gun points at the panel; the game sees nothing */
        float u, v, hit[3];
        int rows[MENU_ROWS], nrows = menu_rows(rows);
        int on = located && menu_hit(o, d, &u, &v, hit);
        int row = on ? xm_row_at(u, v, s_menu.page ? ctrl_rows() : nrows) : -1;
        if (row != s_menu.hover) {
            s_menu.hover = row;
            s_menu.dirty = 1;
        }
        if (on)
            memcpy(s_qo.end, hit, sizeof hit);
        s_qo.beam = 1;
        s_qo.dot = on;
        if (now[s_gun][0] && !was[s_gun][0] && s_menu.page && row == ctrl_rows() - 1) {
            s_menu.page = 0;            /* Back */
            s_menu.hover = -1;
            s_menu.dirty = 1;
            rumble(s_gun, 0.25f, 15);
        } else if (now[s_gun][0] && !was[s_gun][0] && !s_menu.page && row >= 0) {
            menu_choose(rows[row]);
            rumble(s_gun, 0.25f, 15);
        }
        if (s_menu.dirty)
            menu_paint();
        ux = uy = -1;
    } else if (!rifle) {
        /* Trigger: a shot; with the gun off the screen, a reload. Grip too. */
        if (now[s_gun][0])
            buttons |= ux < 0 ? VC3_VR_RELOAD : VC3_VR_FIRE;
        if (now[s_gun][4])
            buttons |= VC3_VR_RELOAD;
        if (now[s_gun][1] || now[off][4])       /* the foot pedal */
            buttons |= VC3_VR_PEDAL;
        if (now[s_gun][2])
            buttons |= VC3_VR_CHANGE;
        if (now[s_gun][0] && !was[s_gun][0]) {
            rumble(s_gun, ux < 0 ? 0.3f : 0.8f, ux < 0 ? 40 : 25);
            if (ux >= 0)
                s_shot_t = t;
        }
    }
    /* the kick and the flash of a shot */
    {
        double ms = s_shot_t ? (double)(t - s_shot_t) / 1e6 : 1e9;
        s_qo.kick = ms >= 0 && ms < 140 ? (float)(1.0 - ms / 140.0) : 0.0f;
        s_qo.flash = ms < 40;
    }

    if (s_menu.test_frames) {
        s_menu.test_frames--;
        buttons |= VC3_VR_TEST;
    }
    if (s_menu.service_frames) {
        s_menu.service_frames--;
        buttons |= VC3_VR_SERVICE;
    }
    start = (now[0][3] || now[1][3]) && !s_menu.open;
    if (start)
        buttons |= VC3_VR_START;
    /* Coins: from the panel, and with automatic coins one at start-up and
     * one with every press of Start -- Start alone plays and continues. */
    coin = s_menu.coin;
    s_menu.coin = 0;
    if (s_vr.auto_coins && (++frames == 240 || (start && !was_start)))
        coin = 1;
    was_start = start;
    if (coin)
        buttons |= VC3_VR_COIN;
    /* A test run's scripted aim (VC3_AUTOAIM, host_input.c) over the fake
     * gun's: the fake gun stays for its drawing, but a gun held still in
     * front hits nothing and the run never leaves its first scene. */
    if (fake_gun && ch_env("AUTOAIM")) {
        ux = uy = -1;
        aim_on = 0;
    }
    if (chihiro_profile()->vr.aim_screen)
        chihiro_profile()->vr.aim_screen(aim_on && ux >= 0, aim_x, aim_y);   /* off with the menu open (ux -1) */
    {
        static int aim_log = -1;
        static unsigned n;
        if (aim_log < 0)
            aim_log = ch_env("AIM_LOG") && atoi(ch_env("AIM_LOG")) > 0;
        if (aim_log && ++n % 120 == 0)
            fprintf(stderr, "[aim host] located %d imm %d aim %d %d,%d ux %.3f uy %.3f\n", located,
                    f && f->vr.imm, aim_on, aim_x, aim_y, ux, uy);
    }
    if (rifle)
        rifle_frame(t, f, located ? d : NULL, now, was);
    else
        vc3_input_vr(ux, uy, buttons);

    /* The other hand's thumbstick moves the screen. */
    {
        XrVector2f st = act_vec2(a_stick_xy, off);
        if (!s_menu.open && (fabsf(st.y) > 0.3f || fabsf(st.x) > 0.3f)) {
            s_dist -= st.y * 0.02f;
            if (st.x != 0.0f)
                s_fill = 0;
            s_height *= 1.0f + st.x * 0.01f;
            if (s_dist < 0.8f) s_dist = 0.8f;
            if (s_dist > 8.0f) s_dist = 8.0f;
            if (s_height < 0.5f) s_height = 0.5f;
            if (s_height > 6.0f) s_height = 6.0f;
            stick_cool = 1.0f;
        } else if (stick_cool > 0) {
            stick_cool = 0;
            screen_save();
        }
    }
#ifndef CHIHIRO_RENDERER_VULKAN
    /* Infinity one eye distance apart on the screen: 2 * eye / width in NDC. */
    if (f)
        vc3_gl_stereo_separation(2.0f * EYE_DISTANCE
                                 / (s_height * (float)f->lw / (float)f->lh));
#endif

    memcpy(was, now, sizeof was);
}

#ifndef CHIHIRO_RENDERER_VULKAN
/* The frame -- whole, or with eyes side by side as they are -- into the
 * swapchain image, upright (the surfaces hold the picture top row first,
 * GL's bottom); with dot, the aim dot in each eye's half (on the screen's
 * plane: no disparity), for when there is no overlay to draw it. */
static void draw_frame(GLuint dst_fbo, const Frame *f, GLsizei w, GLsizei h, int dot)
{
    int e, ew = w / f->eyes;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_read_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_fbo);
    glDisable(GL_SCISSOR_TEST);
    if (s_srgb_control)
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    glBlitFramebuffer(0, 0, f->w, f->h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    for (e = 0; e < f->eyes; e++)
    if (dot && s_hit_u >= 0 && s_hit_u <= 1 && s_hit_v >= 0 && s_hit_v <= 1) {
        int cx = e * ew + (int)(s_hit_u * ew), cy = (int)((1 - s_hit_v) * h);
        int r = h / 120 + 2;
        glEnable(GL_SCISSOR_TEST);
        glScissor(cx - r - 1, cy - r - 1, 2 * r + 2, 2 * r + 2);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glScissor(cx - r, cy - r, 2 * r, 2 * r);
        glClearColor(1.0f, 0.15f, 0.1f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

#endif

static void mat4_mul(float *o, const float *a, const float *b)
{
    float r[16];
    int c, k;
    for (c = 0; c < 4; c++)
        for (k = 0; k < 4; k++)
            r[c * 4 + k] = a[k] * b[c * 4] + a[4 + k] * b[c * 4 + 1] + a[8 + k] * b[c * 4 + 2]
                         + a[12 + k] * b[c * 4 + 3];
    memcpy(o, r, 64);
}

/* This eye's view-projection, LOCAL metres to clip space. */
static void eye_viewproj(const XrView *v, float vp[16])
{
    XrQuaternionf q = v->pose.orientation, qi = { -q.x, -q.y, -q.z, q.w };
    static const float ex[3] = { 1, 0, 0 }, ey[3] = { 0, 1, 0 }, ez[3] = { 0, 0, 1 };
    float view[16], proj[16], c0[3], c1[3], c2[3], t[3], rt[3];
    float l = tanf(v->fov.angleLeft), r = tanf(v->fov.angleRight);
    float u = tanf(v->fov.angleUp), d = tanf(v->fov.angleDown), n = 0.02f, fa = 100.0f;

    qrot(qi, ex, c0);
    qrot(qi, ey, c1);
    qrot(qi, ez, c2);
    t[0] = -v->pose.position.x;
    t[1] = -v->pose.position.y;
    t[2] = -v->pose.position.z;
    qrot(qi, t, rt);
    memset(view, 0, sizeof view);
    view[0] = c0[0]; view[1] = c0[1]; view[2] = c0[2];
    view[4] = c1[0]; view[5] = c1[1]; view[6] = c1[2];
    view[8] = c2[0]; view[9] = c2[1]; view[10] = c2[2];
    view[12] = rt[0]; view[13] = rt[1]; view[14] = rt[2]; view[15] = 1;
    memset(proj, 0, sizeof proj);
    proj[0] = 2 / (r - l);
    proj[5] = 2 / (u - d);
    proj[8] = (r + l) / (r - l);
    proj[9] = (u + d) / (u - d);
    proj[10] = -(fa + n) / (fa - n);
    proj[11] = -1;
    proj[14] = -2 * fa * n / (fa - n);
    mat4_mul(vp, proj, view);
}

#ifndef CHIHIRO_RENDERER_VULKAN
/* The gun, the laser and the panel into each eye's overlay image. */
static int draw_overlay(XrCompositionLayerProjectionView pv[2])
{
    int e;
    for (e = 0; e < 2; e++) {
        Eye *y = &s_eye[e];
        XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        uint32_t idx = 0;
        float vp[16], eye[3];

        wi.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(xrAcquireSwapchainImage(y->chain, &ai, &idx)))
            return 0;
        xrWaitSwapchainImage(y->chain, &wi);
        glBindFramebuffer(GL_FRAMEBUFFER, y->fbo[idx]);
        glViewport(0, 0, (GLsizei)y->w, (GLsizei)y->h);
        glDisable(GL_SCISSOR_TEST);
        if (s_srgb_control)
            glDisable(GL_FRAMEBUFFER_SRGB_EXT);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glClearColor(0, 0, 0, 0);
        glClearDepthf(1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        eye_viewproj(&s_views[e], vp);
        eye[0] = s_views[e].pose.position.x;
        eye[1] = s_views[e].pose.position.y;
        eye[2] = s_views[e].pose.position.z;
        if (s_menu.open) {
            float pm[16], m[16];
            int k;
            memset(pm, 0, sizeof pm);
            for (k = 0; k < 3; k++) {
                pm[k] = s_menu.right[k] * s_menu.hw;
                pm[4 + k] = s_menu.up[k] * s_menu.hh;
                pm[8 + k] = s_menu.n[k];
                pm[12 + k] = s_menu.c[k];
            }
            pm[15] = 1;
            mat4_mul(m, vp, pm);
            qo_draw_panel(m);
        }
        qo_draw(&s_qo, vp, eye);
        {   /* only the colour is kept */
            static const GLenum dis[1] = { GL_DEPTH_ATTACHMENT };
            glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, dis);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        xrReleaseSwapchainImage(y->chain, &ri);

        pv[e] = (XrCompositionLayerProjectionView){ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
        pv[e].pose = s_views[e].pose;
        pv[e].fov = s_views[e].fov;
        pv[e].subImage.swapchain = y->chain;
        pv[e].subImage.imageRect.extent.width = (int32_t)y->w;
        pv[e].subImage.imageRect.extent.height = (int32_t)y->h;
    }
    return 1;
}

/* The HUD panel's swapchain, the size of the HUD picture. */
static XrSwapchain s_hud_chain;
static uint32_t s_hud_gen;            /* bumped at every (re)creation */
static uint32_t s_hud_n, s_hud_cw, s_hud_ch;
static XrSwapchainImageOpenGLESKHR s_hud_img[8];
static GLuint s_hud_fbo[8];

static int hud_chain_fit(uint32_t w, uint32_t h)
{
    XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    uint32_t i;
    if (s_hud_chain && s_hud_cw == w && s_hud_ch == h)
        return 1;
    if (s_hud_chain) {
        glDeleteFramebuffers((GLsizei)s_hud_n, s_hud_fbo);
        xrDestroySwapchain(s_hud_chain);
        s_hud_chain = XR_NULL_HANDLE;
    }
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = s_chain_fmt;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XRCHECK(xrCreateSwapchain(s_sess, &ci, &s_hud_chain));
    s_hud_gen++;
    XRCHECK(xrEnumerateSwapchainImages(s_hud_chain, 0, &s_hud_n, NULL));
    if (s_hud_n > 8)
        s_hud_n = 8;
    for (i = 0; i < s_hud_n; i++)
        s_hud_img[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    XRCHECK(xrEnumerateSwapchainImages(s_hud_chain, s_hud_n, &s_hud_n,
                                       (XrSwapchainImageBaseHeader *)s_hud_img));
    glGenFramebuffers((GLsizei)s_hud_n, s_hud_fbo);
    for (i = 0; i < s_hud_n; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_hud_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               s_hud_img[i].image, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_hud_cw = w;
    s_hud_ch = h;
    fprintf(stderr, "[xr] HUD panel %ux%u\n", w, h);
    return 1;
}

/* The frame's HUD picture into the panel's swapchain, upright; the quad. */
static int hud_layer(const Frame *f, XrCompositionLayerQuad *q, int copy)
{
    static uint32_t drawn;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrSwapchainImageWaitInfo wti = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    uint32_t idx = 0;
    float pc[3], pw, ph, c[3];
    if (!f->has_hud || !hud_panel(f, pc, &pw, &ph)
        || !hud_chain_fit((uint32_t)f->hud_w, (uint32_t)f->hud_h))
        return 0;
    /* the picture already in the panel's swapchain stays (see xr_frame) */
    if (copy || drawn != s_hud_gen) {
        wti.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(xrAcquireSwapchainImage(s_hud_chain, &ai, &idx))
            || XR_FAILED(xrWaitSwapchainImage(s_hud_chain, &wti)))
            return 0;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_read_fbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->hud, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_hud_fbo[idx]);
        glDisable(GL_SCISSOR_TEST);
        if (s_srgb_control)
            glDisable(GL_FRAMEBUFFER_SRGB_EXT);
        glBlitFramebuffer(0, 0, f->hud_w, f->hud_h, 0, f->hud_h, f->hud_w, 0, GL_COLOR_BUFFER_BIT,
                          GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        xrReleaseSwapchainImage(s_hud_chain, &ri);
        drawn = s_hud_gen;
    }
    yaw_rot(s_anchor_yaw, pc, c);
    memset(q, 0, sizeof *q);
    q->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    q->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  /* premultiplied */
    q->space = s_local;
    q->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q->subImage.swapchain = s_hud_chain;
    q->subImage.imageRect.extent.width = (int32_t)s_hud_cw;
    q->subImage.imageRect.extent.height = (int32_t)s_hud_ch;
    q->pose.orientation.y = sinf(s_anchor_yaw * 0.5f);
    q->pose.orientation.w = cosf(s_anchor_yaw * 0.5f);
    q->pose.position.x = c[0] + s_anchor_p[0];
    q->pose.position.y = c[1] + s_anchor_p[1];
    q->pose.position.z = c[2] + s_anchor_p[2];
    q->size.width = pw;
    q->size.height = ph;
    return 1;
}

/* An immersive frame's two eyes as a projection layer: each half of the
 * frame as that eye's view, with the pose and field of view it was drawn
 * with (the compositor turns it to where the head is now). */
static void world_views(const Frame *f, XrCompositionLayerProjectionView wv[2],
                        XrSwapchain chain, uint32_t cw, uint32_t ch)
{
    int e;
    for (e = 0; e < 2; e++) {
        memset(&wv[e], 0, sizeof wv[e]);
        wv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        view_to_local(&f->vr.view, e, &wv[e].pose);
        wv[e].fov.angleLeft = f->vr.view.fov[e][0];
        wv[e].fov.angleRight = f->vr.view.fov[e][1];
        wv[e].fov.angleUp = f->vr.view.fov[e][2];
        wv[e].fov.angleDown = f->vr.view.fov[e][3];
        wv[e].subImage.swapchain = chain;
        wv[e].subImage.imageRect.offset.x = (int32_t)((uint32_t)e * cw / 2);
        wv[e].subImage.imageRect.extent.width = (int32_t)(cw / 2);
        wv[e].subImage.imageRect.extent.height = (int32_t)ch;
    }
}

/* VC3_GPU_TIME=1: the GPU time of this loop's own drawing per display frame
 * -- the copy into the swapchain (when not direct), the HUD panel's, the
 * gun overlay -- logged once a second as [XRGPU] (the back end's passes are
 * its [GPUTIME]; the compositor's are not the app's to time). */
#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#define XRGT_RING 16
static int s_xrgt_on = -1;
static GLuint s_xrgt_q[XRGT_RING];
static unsigned s_xrgt_head, s_xrgt_tail;
static double s_xrgt_ms;
static uint32_t s_xrgt_n;

static void xrgt_begin(void)
{
    if (s_xrgt_on < 0) {
        s_xrgt_on = ch_env("GPU_TIME") && *ch_env("GPU_TIME") == '1';
        if (s_xrgt_on)
            glGenQueries(XRGT_RING, s_xrgt_q);
    }
    if (s_xrgt_on && s_xrgt_head - s_xrgt_tail < XRGT_RING - 1)
        glBeginQuery(GL_TIME_ELAPSED_EXT, s_xrgt_q[s_xrgt_head % XRGT_RING]);
    else if (s_xrgt_on)
        s_xrgt_on = 2;                  /* this frame not timed */
}

static void xrgt_end(void)
{
    static uint64_t t0;
    uint64_t now;
    if (s_xrgt_on <= 0)
        return;
    if (s_xrgt_on == 1) {
        glEndQuery(GL_TIME_ELAPSED_EXT);
        s_xrgt_head++;
    }
    s_xrgt_on = 1;
    while (s_xrgt_tail != s_xrgt_head) {
        GLuint avail = 0, ns = 0;
        GLuint q = s_xrgt_q[s_xrgt_tail % XRGT_RING];
        glGetQueryObjectuiv(q, GL_QUERY_RESULT_AVAILABLE, &avail);
        if (!avail)
            break;
        glGetQueryObjectuiv(q, GL_QUERY_RESULT, &ns);
        s_xrgt_ms += ns / 1e6;
        s_xrgt_n++;
        s_xrgt_tail++;
    }
    now = pace_now_us();
    if (!t0)
        t0 = now;
    if (now - t0 >= 1000000 && s_xrgt_n) {
        fprintf(stderr, "[XRGPU] %.2f ms per display frame over %u (swapchain copy, HUD, overlay)\n",
                s_xrgt_ms / s_xrgt_n, s_xrgt_n);
        s_xrgt_ms = 0;
        s_xrgt_n = 0;
        t0 = now;
    }
}

#endif

#ifdef CHIHIRO_RENDERER_VULKAN
#include "quest_vulkan.inc"
#endif

/* VC3_XR_LOOP_LOG=1: once a second, this loop's rate, the time it spent
 * blocked in xrWaitFrame, and its work from there to xrEndFrame -- the
 * runtime throttles an app whose frames come late. */
static uint64_t s_lt_sleep, s_lt_sleeps, s_lt_begin, s_lt_end, s_lt_period, s_lt_step, s_lt_steps, s_lt_step_max;
static void xr_loop_note(uint64_t t0, uint64_t t1, int render)
{
    static int on = -1;
    static uint64_t last, n, rendered, wait_sum, wait_max, work_sum, work_max, gap_max, prev;
    uint64_t now = pace_now_us();
    if (on < 0)
        on = ch_env("XR_LOOP_LOG") && *ch_env("XR_LOOP_LOG") == '1';
    if (!on)
        return;
    n++;
    rendered += render != 0;
    wait_sum += t1 - t0;
    if (t1 - t0 > wait_max)
        wait_max = t1 - t0;
    work_sum += now - t1;
    if (now - t1 > work_max)
        work_max = now - t1;
    if (prev && t0 - prev > gap_max)
        gap_max = t0 - prev;
    prev = now;
    if (!last)
        last = now;
    if (now - last >= 1000000) {
        fprintf(stderr, "[xr loop] %llu loops (%llu rendering): wait avg %.2f max %.2f ms, work avg %.2f max %.2f ms, between max %.2f ms\n",
                (unsigned long long)n, (unsigned long long)rendered, wait_sum / 1000.0 / n, wait_max / 1000.0,
                work_sum / 1000.0 / n, work_max / 1000.0, gap_max / 1000.0);
#ifdef CHIHIRO_RENDERER_VULKAN
        fprintf(stderr, "[xr loop]   ms a loop: world %.2f clear %.2f hud %.2f gun %.2f swapchain %.2f settle %.2f\n",
                s_vk_seg[0] / 1000.0 / n, s_vk_seg[1] / 1000.0 / n, s_vk_seg[2] / 1000.0 / n,
                s_vk_seg[3] / 1000.0 / n, s_vk_seg[4] / 1000.0 / n, s_vk_seg[5] / 1000.0 / n);
        fprintf(stderr, "[xr loop]   world: acquire %.2f copy %.2f HUD %.2f release %.2f; overlay: acquire %.2f copy %.2f\n",
                s_vk_seg[6] / 1000.0 / n, s_vk_seg[7] / 1000.0 / n, s_vk_seg[8] / 1000.0 / n,
                s_vk_seg[9] / 1000.0 / n, s_vk_seg[10] / 1000.0 / n, s_vk_seg[11] / 1000.0 / n);
        memset(s_vk_seg, 0, sizeof s_vk_seg);
#endif
        fprintf(stderr, "[xr loop]   self-pace sleeps %llu (%.2f ms a loop), begin %.2f ms, end %.2f ms a loop; period %.2f ms, display step avg %.2f max %.2f ms\n",
                (unsigned long long)s_lt_sleeps, s_lt_sleep / 1000.0 / n, s_lt_begin / 1000.0 / n, s_lt_end / 1000.0 / n,
                s_lt_period / 1e6, s_lt_steps ? s_lt_step / 1e6 / s_lt_steps : 0.0, s_lt_step_max / 1e6);
        s_lt_sleep = s_lt_sleeps = s_lt_begin = s_lt_end = s_lt_step = s_lt_steps = s_lt_step_max = 0;
        last = now;
        n = rendered = wait_sum = wait_max = work_sum = work_max = gap_max = 0;
    }
}

static void xr_frame(void)
{
    XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { XR_TYPE_FRAME_STATE };
    XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
#ifndef CHIHIRO_RENDERER_VULKAN
    XrCompositionLayerQuad quad[2], hudq;
    XrCompositionLayerProjection proj = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerProjection world = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerProjectionView pv[2], wv[2];
#endif
    const XrCompositionLayerBaseHeader *layers[16];   /* Vulkan: world, 10 HUD pieces, overlay, stats, menu */
    uint32_t nl = 0;
    Frame *f;
    int is_new, allow = 1;

    uint64_t loop_t0 = pace_now_us();
    if (XR_FAILED(xrWaitFrame(s_sess, &wi, &fs)))
        return;
    uint64_t loop_t1 = pace_now_us();
    {
        /* xrWaitFrame paces this loop to the display -- except when the
         * runtime stops blocking (seen with SteamVR and the headset off or
         * asleep: ~400 loops a second), which drew the overlay for nothing
         * and took CPU and GPU from the game. Then the loop paces itself to
         * the display period -- but only once the runtime has clearly
         * stopped pacing (16 loops in a row under half a period), and not a
         * moment longer than it blocks again. Sleeping whenever one
         * xrWaitFrame came back early (as SteamVR may) started that frame
         * late; SteamVR saw a missed frame, throttled the app to 60, then
         * the sleep (on this loop's clock, not the runtime's) kept every
         * frame late: 40, 30, 24 Hz, back to 120 and down again every ~20 s
         * in play, 3/4 of frames reprojected. VC3_XR_SELF_PACE=0: never. */
        static uint64_t last_us;
        static int self_pace = -1, engaged, fast;
        uint64_t period_us = fs.predictedDisplayPeriod > 0 ? (uint64_t)fs.predictedDisplayPeriod / 1000u : 8333u;
        uint64_t now = pace_now_us();
        if (self_pace < 0)
            self_pace = !(ch_env("XR_SELF_PACE") && *ch_env("XR_SELF_PACE") == '0');
        {
            static XrTime prev_disp;
            s_lt_period = (uint64_t)fs.predictedDisplayPeriod;
            if (prev_disp && fs.predictedDisplayTime > prev_disp) {
                uint64_t st = (uint64_t)(fs.predictedDisplayTime - prev_disp);
                s_lt_step += st; s_lt_steps++;
                if (st > s_lt_step_max) s_lt_step_max = st;
            }
            prev_disp = fs.predictedDisplayTime;
        }
        if (self_pace && !engaged) {
            fast = last_us && now > last_us && now - last_us < period_us / 2 ? fast + 1 : 0;
            if (fast >= 16) {
                engaged = 1;
                fprintf(stderr, "[xr] the runtime is not pacing frames: paced to %.2f ms here\n", period_us / 1000.0);
            }
        } else if (engaged && loop_t1 - loop_t0 > 1000) {
            engaged = fast = 0;   /* xrWaitFrame blocks again: the runtime paces */
            fprintf(stderr, "[xr] the runtime paces frames again\n");
        }
        if (engaged && last_us && now > last_us && now - last_us < period_us * 3 / 4) {
            uint64_t wait = last_us + period_us - now;
            s_lt_sleeps++; s_lt_sleep += wait;
            struct timespec ts = { (time_t)(wait / 1000000u), (long)(wait % 1000000u) * 1000L };
            nanosleep(&ts, NULL);
            now = pace_now_us();
        }
        last_us = now;
    }
    uint64_t bt0 = pace_now_us();
    if (XR_FAILED(xrBeginFrame(s_sess, &bi)))
        return;
    s_lt_begin += pace_now_us() - bt0;
    {
        /* The display's cadence: at 120 Hz a game frame every second
         * display frame, so each is shown exactly twice, and the game's
         * 60 Hz vblank aligned to those frames' starts (xbox_VblankAlign),
         * so the frame it paces is done by the one that takes it. Without
         * it game frames came 1, 2 or 3 display frames apart -- judder.
         * VC3_XR_CADENCE=0: take the newest frame every display frame. */
        static int cad_on = -1;
        static uint32_t xr_n;
        double per = (double)fs.predictedDisplayPeriod / 1e6, ratio = per > 0 ? 16.6667 / per : 0;
        int k = (int)(ratio + 0.5);
        int locked;
        if (cad_on < 0)
            cad_on = !(ch_env("XR_CADENCE") && *ch_env("XR_CADENCE") == '0');
        locked = cad_on && k >= 2 && fabs(ratio - k) < 0.05;
        xr_n++;
        allow = !locked || xr_n % (uint32_t)k == 0;
        if (locked && allow)
            xbox_VblankAlign(xbox_HostNowNs());
    }
#ifndef CHIHIRO_RENDERER_VULKAN
    xrgt_begin();
#endif
#ifdef CHIHIRO_RENDERER_VULKAN
    /* the last world copy, done before take_frame can give its source back */
    SEG(5, vk_host_world_settle());
#endif
    f = take_frame(&is_new, allow);
    if (s_pace > 0) {   /* how many headset frames each game frame showed for */
        static int shown, hist[5], xr_n;
        static uint64_t t0;
        uint64_t now = pace_now_us();
        if (is_new) {
            if (shown)
                hist[shown > 4 ? 4 : shown]++;
            shown = 0;
        }
        shown++;
        xr_n++;
        if (!t0)
            t0 = now;
        if (now - t0 >= 1000000) {
            fprintf(stderr, "[pace] headset %d frames: game frames shown 1x %d, 2x %d, 3x %d, 4+x %d\n",
                    xr_n, hist[1], hist[2], hist[3], hist[4]);
            memset(hist, 0, sizeof hist);
            xr_n = 0;
            t0 = now;
        }
    }
    {
        XrViewLocateInfo vl = { XR_TYPE_VIEW_LOCATE_INFO };
        XrViewState vs = { XR_TYPE_VIEW_STATE };
        uint32_t nv = 0;
        vl.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vl.displayTime = fs.predictedDisplayTime;
        vl.space = s_local;
        s_views[0] = (XrView){ XR_TYPE_VIEW };
        s_views[1] = (XrView){ XR_TYPE_VIEW };
        s_views_ok = XR_SUCCEEDED(xrLocateViews(s_sess, &vl, &vs, 2, &nv, s_views)) && nv == 2
            && (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)
            && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
        if (s_views_ok) {
            /* VC3_XR_FAKE_HEAD=1, for a headset nobody wears (test runs): the
             * eyes level at the anchor, facing its yaw -- what a player looking
             * straight ahead sees, not wherever a headset on a table points */
            static int fake_head = -1;
            static float fake_yaw;      /* VC3_XR_FAKE_YAW=degrees: turned that far left */
            if (fake_head < 0) {
                fake_head = ch_env("XR_FAKE_HEAD") && *ch_env("XR_FAKE_HEAD") == '1';
                fake_yaw = ch_env("XR_FAKE_YAW") ? (float)atof(ch_env("XR_FAKE_YAW")) * 0.0174533f : 0.0f;
            }
            if (fake_head) {
                const XrVector3f *a = &s_views[0].pose.position, *b = &s_views[1].pose.position;
                const float right[3] = { 1, 0, 0 }, yaw = s_anchor_yaw + fake_yaw;
                float half = 0.5f * sqrtf((b->x - a->x) * (b->x - a->x) + (b->y - a->y) * (b->y - a->y)
                                          + (b->z - a->z) * (b->z - a->z)), r[3];
                yaw_rot(yaw, right, r);
                for (int e = 0; e < 2; e++) {
                    const float side = e ? half : -half;
                    s_views[e].pose.orientation = (XrQuaternionf){ 0, sinf(yaw * .5f), 0, cosf(yaw * .5f) };
                    s_views[e].pose.position = (XrVector3f){ s_anchor_p[0] + side * r[0], s_anchor_p[1],
                                                             s_anchor_p[2] + side * r[2] };
                }
            }
#ifdef CHIHIRO_RENDERER_VULKAN
            {   /* each eye's field of view, for the foveated shading-rate images */
                void ch_vk_target_set_foveation_view(const float tan_fov[2][4]);
                float t[2][4];
                for (int e = 0; e < 2; e++) {
                    t[e][0] = tanf(s_views[e].fov.angleLeft);
                    t[e][1] = tanf(s_views[e].fov.angleRight);
                    t[e][2] = tanf(s_views[e].fov.angleUp);
                    t[e][3] = tanf(s_views[e].fov.angleDown);
                }
                ch_vk_target_set_foveation_view(t);
            }
#endif
            /* Once a session, where the runtime puts each eye in the head's
             * frame: view 0 must be the left eye (x < 0). */
            static int logged;
            if (!logged) {
                XrQuaternionf q = s_views[0].pose.orientation, qi = { -q.x, -q.y, -q.z, q.w };
                float d[3], h[3];
                int k;
                logged = 1;
                for (k = 0; k < 3; k++)
                    d[k] = (&s_views[1].pose.position.x)[k] - (&s_views[0].pose.position.x)[k];
                qrot(qi, d, h);
                fprintf(stderr, "[xr] eyes: view1-view0 in head frame %+.4f %+.4f %+.4f m; "
                        "fov0 L%.1f R%.1f fov1 L%.1f R%.1f deg\n", h[0], h[1], h[2],
                        s_views[0].fov.angleLeft * 57.2958f, s_views[0].fov.angleRight * 57.2958f,
                        s_views[1].fov.angleLeft * 57.2958f, s_views[1].fov.angleRight * 57.2958f);
            }
        }
        view_publish();
        screen_fill_apply();
    }
    perf_frame();
    rate_watch();
    if (s_overlay_ok < 0) {
#ifdef CHIHIRO_RENDERER_VULKAN
        s_overlay_ok = ch_env("XR_NO_OVERLAY") && *ch_env("XR_NO_OVERLAY") == '1' ? 0 : vk_overlay_init();
#else
        s_overlay_ok = overlay_init();
#endif
        if (!s_overlay_ok)
            fprintf(stderr, "[xr] no overlay: the aim dot is drawn into the picture\n");
    }
    static int fake_gun = -1;   /* VC3_XR_FAKE_GUN=1: test runs, whose session is only VISIBLE */
    if (fake_gun < 0)
        fake_gun = ch_env("XR_FAKE_GUN") && *ch_env("XR_FAKE_GUN") == '1';
    if (s_state == XR_SESSION_STATE_FOCUSED || (fake_gun && s_state == XR_SESSION_STATE_VISIBLE))
        gun_frame(fs.predictedDisplayTime, f);
    else {
        s_qo.gun_valid = 0;
        if (is_rifle())
            rifle_release();
    }
#ifdef CHIHIRO_RENDERER_VULKAN
    vk_test_dump(f);
#endif

    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
#ifdef CHIHIRO_RENDERER_VULKAN
    pthread_mutex_lock(&s_vk_queue);
    if(s_vk_profile.enabled) s_vk_profile.host_begin=pace_now_us();
    vk_stats_sample();
    if(fs.shouldRender) nl=vk_present(f,layers,fs.predictedDisplayPeriod);
    else vk_overlay_skip_frame();
#else
    if (fs.shouldRender && f && chain_fit((uint32_t)f->w, (uint32_t)f->h)) {
        XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        XrSwapchainImageWaitInfo wti = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        uint32_t idx = 0;

        /* A frame already in the swapchain is not copied again: the
         * layers use the image released last. Only a new game frame, a new
         * swapchain, or the aim dot (drawn into the picture when there is no
         * overlay, and moving every frame) needs the acquire and the blit.
         * VC3_XR_REBLIT=1: copy every display frame, as before. */
        static uint32_t drawn_chain;
        static const Frame *drawn_f;
        static int reblit = -1;
        int dot = s_overlay_ok <= 0 && !f->vr.imm;
        int copy;
        if (reblit < 0)
            reblit = ch_env("XR_REBLIT") && *ch_env("XR_REBLIT") == '1';
        /* a frame in a lent image is in the swapchain already: released now
         * (only its HUD is copied); a filled image a newer frame overtook,
         * released before that frame's copy goes in after it */
        XrSwapchain chain = s_chain;
        uint32_t cw = s_chain_w, ch = s_chain_h;
        int lent = is_new && f->direct && lend_take();
        if (is_new && !lent)
            lend_drop();
        copy = !lent && !f->direct     /* a lent frame's picture is not in tex */
               && (reblit || is_new || dot || drawn_chain != s_chain_gen || drawn_f != f);

        wti.timeout = XR_INFINITE_DURATION;
        if (lent || !copy
            || (XR_SUCCEEDED(xrAcquireSwapchainImage(s_chain, &ai, &idx))
                && XR_SUCCEEDED(xrWaitSwapchainImage(s_chain, &wti)))) {
            uint32_t e;
            float c[3], yq = s_anchor_yaw * 0.5f;

            if (lent) {
                drawn_chain = s_chain_gen;
                drawn_f = f;
            } else if (copy) {
                draw_frame(s_chain_fbo[idx], f, (GLsizei)s_chain_w, (GLsizei)s_chain_h, dot);
                xrReleaseSwapchainImage(s_chain, &ri);
                drawn_chain = s_chain_gen;
                drawn_f = f;
            }
            if (f->vr.imm && f->eyes == 2) {
                /* Immersive: the world in each eye, the HUD on its panel. */
                world_views(f, wv, chain, cw, ch);
                world.space = s_local;
                world.viewCount = 2;
                world.views = wv;
                layers[nl++] = (const XrCompositionLayerBaseHeader *)&world;
                if (hud_layer(f, &hudq, copy || lent))   /* the HUD: a new frame's */
                    layers[nl++] = (const XrCompositionLayerBaseHeader *)&hudq;
                if (copy || lent)
                    frame_read_done(f);
            } else {
            if (copy || lent)
                frame_read_done(f);
            {
                float fwd[3] = { 0, 0, -s_dist };
                yaw_rot(s_anchor_yaw, fwd, c);
                c[0] += s_anchor_p[0];
                c[1] += s_anchor_p[1];
                c[2] += s_anchor_p[2];
            }
            for (e = 0; e < (uint32_t)f->eyes; e++) {
                XrCompositionLayerQuad *q = &quad[e];
                *q = (XrCompositionLayerQuad){ XR_TYPE_COMPOSITION_LAYER_QUAD };
                q->space = s_local;            /* opaque: no blend flags */
                q->eyeVisibility = f->eyes == 1 ? XR_EYE_VISIBILITY_BOTH
                                 : e == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
                q->subImage.swapchain = chain;
                q->subImage.imageRect.offset.x = (int32_t)(e * cw / (uint32_t)f->eyes);
                q->subImage.imageRect.extent.width = (int32_t)(cw / (uint32_t)f->eyes);
                q->subImage.imageRect.extent.height = (int32_t)ch;
                q->pose.orientation.y = sinf(yq);
                q->pose.orientation.w = cosf(yq);
                q->pose.position.x = c[0];
                q->pose.position.y = c[1];
                q->pose.position.z = c[2];
                q->size.height = s_height;
                q->size.width = s_height * (float)f->lw / (float)f->lh;
                layers[nl++] = (const XrCompositionLayerBaseHeader *)q;
            }
            }
        }
    }
    /* the gun and the panel, over the screen. At 90 Hz and up the gun is
     * drawn every second display frame, the last image submitted again in
     * between with the pose it was drawn for (the compositor turns it to
     * the head's); with the panel open, every frame. VC3_OVERLAY_FULLRATE=1:
     * every frame. */
    {
        static XrCompositionLayerProjectionView last_pv[2];
        static int have_last, full = -1;
        static uint32_t n;
        int want = fs.shouldRender && s_overlay_ok > 0 && s_views_ok
                   && (s_qo.gun_valid || s_menu.open);
        if (full < 0)
            full = ch_env("OVERLAY_FULLRATE") && *ch_env("OVERLAY_FULLRATE") == '1';
        n++;
        if (!want) {
            have_last = 0;
        } else if (overlay_reuse_frame(have_last,full,s_menu.open,n,fs.predictedDisplayPeriod)) {
            memcpy(pv, last_pv, sizeof last_pv);   /* the image drawn last frame */
            proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            proj.space = s_local;
            proj.viewCount = 2;
            proj.views = pv;
            layers[nl++] = (const XrCompositionLayerBaseHeader *)&proj;
        } else if (draw_overlay(pv)) {
            memcpy(last_pv, pv, sizeof last_pv);
            have_last = 1;
            proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            proj.space = s_local;
            proj.viewCount = 2;
            proj.views = pv;
            layers[nl++] = (const XrCompositionLayerBaseHeader *)&proj;
        }
    }
#endif
    ei.layerCount = nl;
    ei.layers = layers;
#ifndef CHIHIRO_RENDERER_VULKAN
    lend_out();                         /* the image for the back end's next frame */
    xrgt_end();
#endif
#ifdef CHIHIRO_RENDERER_VULKAN
    uint64_t et0 = pace_now_us();
    vk_xr_check(xrEndFrame(s_sess, &ei),"end frame");
    s_lt_end += pace_now_us() - et0;
    static int first_layer;
    if(nl && !first_layer) {
        fprintf(stderr,"[xr] native Vulkan first layer submitted: %u layers, guest sequence %llu\n",
            nl,(unsigned long long)s_frames_in);
        first_layer=1;
    }
#else
    xrEndFrame(s_sess, &ei);
#endif
#ifdef CHIHIRO_RENDERER_VULKAN
    vk_profile_host_end();
    pthread_mutex_unlock(&s_vk_queue);
    xr_loop_note(loop_t0, loop_t1, fs.shouldRender);
#endif
}

/* ── Without a headset: the SDL window ────────────────────────────────── */

#ifndef CHIHIRO_RENDERER_VULKAN
static void flat_frame(void)
{
    int is_new, ww = 0, wh = 0;
    Frame *f = take_frame(&is_new, 1);

    if (!f || !is_new) {
        SDL_Delay(4);
        return;
    }
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    {
        /* letterboxed at the picture's shape */
        double pa = (double)f->lw / f->lh;
        int dw = ww, dh = (int)(ww / pa);
        if (dh > wh) {
            dh = wh;
            dw = (int)(wh * pa);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_read_fbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->tex, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        /* the left eye of a stereo pair */
        glBlitFramebuffer(0, 0, f->w / f->eyes, f->h, (ww - dw) / 2, (wh + dh) / 2,
                          (ww + dw) / 2, (wh - dh) / 2, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        frame_read_done(f);
    }
    SDL_GL_SwapWindow(s_win);
}

/* ── The guest thread, and the loop ───────────────────────────────────── */

#endif

static int (*s_guest_main)(void);
static volatile int s_guest_rc, s_guest_done;

static void *guest_thread(void *arg)
{
    (void)arg;
    s_tid_guest = (pid_t)syscall(SYS_gettid);
    s_guest_rc = s_guest_main();
    s_guest_done = 1;
    return NULL;
}

/* main.c's vc3_relaunch: the activity starts a fresh process (Restart). */
void vc3_quest_restart(void)
{
#if defined(__ANDROID__)
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (env && activity) {
        jclass cls = (*env)->GetObjectClass(env, activity);
        jmethodID m = (*env)->GetMethodID(env, cls, "restartGame", "()V");
        if (m)
            (*env)->CallVoidMethod(env, activity, m);
    }
#else
    /* desktop: the same program again, with its arguments */
    {
        static char buf[8192], *argv[64];
        char exe[4096];
        ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
        FILE *f = fopen("/proc/self/cmdline", "rb");
        size_t got = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
        int argc = 0;
        size_t o = 0;
        if (f)
            fclose(f);
        while (o < got && argc < 63) {
            argv[argc++] = buf + o;
            o += strlen(buf + o) + 1;
        }
        argv[argc] = NULL;
        if (len > 0 && argc) {
            exe[len] = 0;
            fflush(stderr);
            execv(exe, argv);
        }
        perror("[quest] restart");
    }
#endif
    fflush(stderr);
    _exit(0);                           /* as at close: no handlers under the game */
}

int vc3_quest_run(int (*guest_main)(void))
{
    pthread_attr_t attr;
    pthread_t th;
    int xr = 0, frames_logged = 0;
    uint64_t t_log = 0, in_log = 0;

    /* SDL owns the activity's surface; its GLES 3.2 context is the one the
     * headset is given, and the back end's shares with it. */
    s_tid_host = (pid_t)syscall(SYS_gettid);
    g_xbox_thread_named = thread_named;   /* before the toolkit's threads start */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[quest] SDL: %s\n", SDL_GetError());
        return 1;
    }
#ifdef CHIHIRO_RENDERER_VULKAN
#if defined(__ANDROID__)
    s_win=SDL_CreateWindow(chihiro_profile()->title,0,0,1280,720,SDL_WINDOW_FULLSCREEN);
    if(!s_win) {fprintf(stderr,"[quest] SDL window: %s\n",SDL_GetError());return 1;}
#else
    (void)s_win;                        /* the headset only: no window */
#endif
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    s_win = SDL_CreateWindow(chihiro_profile()->title, 0, 0, 1280, 720,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
    {
        SDL_GLContext c = s_win ? SDL_GL_CreateContext(s_win) : NULL;
        /* Lesser GLES (the Android emulator has 3.0): the back end's
         * shaders are written for any 3.x. */
        while (s_win && !c && s_es_minor > 0) {
            s_es_minor--;
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, s_es_minor);
            c = SDL_GL_CreateContext(s_win);
        }
        if (!c) {
            fprintf(stderr, "[quest] no GLES 3 window: %s\n", SDL_GetError());
            return 1;
        }
    }
    SDL_GL_SetSwapInterval(1);
    s_dpy = eglGetCurrentDisplay();
    s_ctx = eglGetCurrentContext();
    {
        EGLint id = 0, n = 0;
        EGLint attrs[] = { EGL_CONFIG_ID, 0, EGL_NONE };
        eglQueryContext(s_dpy, s_ctx, EGL_CONFIG_ID, &id);
        attrs[1] = id;
        eglChooseConfig(s_dpy, attrs, &s_cfg, 1, &n);
    }
    fprintf(stderr, "[quest] %s, %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    glGenFramebuffers(1, &s_read_fbo);  /* the game's frames are read through it */

#endif

    screen_load();
    if (ch_env("NO_XR") && *ch_env("NO_XR") == '1')
        fprintf(stderr, "[quest] VC3_NO_XR=1: the window, not the headset\n");
    else
        xr = xr_init();
    if (!xr) {
#ifdef CHIHIRO_RENDERER_VULKAN
        fprintf(stderr,"[quest] native Vulkan requires OpenXR; guest not started\n");
        return 1;
#else
        fprintf(stderr, "[quest] no OpenXR headset: showing the game in the window\n");
#endif
    }

    /* The guest on a thread of its own, with a big stack: lifted code
     * recurses on the native stack. */
    s_guest_main = guest_main;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    if (pthread_create(&th, &attr, guest_thread, NULL) != 0) {
        fprintf(stderr, "[quest] cannot start the guest thread\n");
        return 1;
    }
    s_guest_clock_ok = pthread_getcpuclockid(th, &s_guest_clock) == 0;
    pthread_attr_destroy(&attr);
    pthread_detach(th);

    while (!s_quit && !s_guest_done) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_QUIT)
                s_quit = 1;
        if (xr) {
            xr_events();
            if (s_running)
                xr_frame();
            else
                SDL_Delay(10);
        } else {
#ifndef CHIHIRO_RENDERER_VULKAN
            flat_frame();
#endif
        }
        /* Once a second for the first minute: frames in from the game. */
        {
            uint64_t now = SDL_GetTicks64();
            if (now - t_log >= 1000 && frames_logged < 60) {
                uint64_t n = s_frames_in;
                if (t_log)
                    fprintf(stderr, "[quest] %llu game frames/s\n",
                            (unsigned long long)(n - in_log));
                t_log = now;
                in_log = n;
                frames_logged++;
            }
        }
    }
    fprintf(stderr, "[quest] %s\n", s_guest_done ? "game returned" : "closing");
    /* _exit, not exit: the game's thread is still running lifted code, and
     * exit's handlers (static destructors, the runtime's) pulled its memory
     * from under it -- a fault with every register zero at each close. */
    fflush(stderr);
    fflush(stdout);
    _exit(s_guest_done ? s_guest_rc : 0);
}

#endif /* __ANDROID__ || CHIHIRO_XR_DESKTOP */
