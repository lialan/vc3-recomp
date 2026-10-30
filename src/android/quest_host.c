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
 * newest, waits on its fence on the GPU, and copies it into the quad
 * layer's swapchain. Nothing is read back to the CPU, and neither thread
 * ever waits for the other.
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
 */
#if defined(__ANDROID__)

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <jni.h>
#include <SDL.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "display_settings.h"
#include "host_input.h"
#include "quest_menu.h"
#include "quest_overlay.h"

void vc3_quest_restart(void);       /* below: a fresh process */

#ifndef GL_FRAMEBUFFER_SRGB_EXT
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9
#endif

/* ── EGL: the host's context and the back end's ───────────────────────── */

static SDL_Window *s_win;
static EGLDisplay s_dpy = EGL_NO_DISPLAY;
static EGLContext s_ctx = EGL_NO_CONTEXT;
static EGLConfig  s_cfg;
static int s_es_minor = 2;          /* 3.2 on the Quest; 3.0 in the emulator */
static volatile int s_quit;

/* Called once on the executor thread by gl_backend.c's gl_ready: a context
 * sharing the host's objects, current on that thread from here on. */
int vc3_quest_gl_context(void)
{
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

/* ── Frames from the back end: three shared textures ──────────────────── */

typedef struct {
    GLuint tex;
    GLsizei w, h;            /* as drawn: both eyes, side by side, in stereo */
    int eyes;                /* 1, or 2 for a stereo pair (each w / 2 wide) */
    int lw, lh;              /* the picture's shape (guest pixels) */
    GLsync fence;            /* the back end's copy into it is done */
    GLsync read_done;        /* the host's last copy out of it is done */
} Frame;

static Frame s_frame[3];
static int s_back = 0, s_ready = 1, s_front = 2;
static uint64_t s_ready_seq, s_shown_seq;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t s_frames_in;   /* for the log */

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
        s_pace = getenv("VC3_PACE_LOG") && *getenv("VC3_PACE_LOG") == '1';
    if (!s_pace)
        return;
    if (last && n < 512)
        dt[n++] = (uint32_t)(now - last);
    last = now;
    if (!t0)
        t0 = now;
    if (now - t0 >= 1000000 && n > 0) {
        int over = 0, i;
        qsort(dt, (size_t)n, sizeof dt[0], cmp_u32);
        for (i = 0; i < n; i++)
            over += dt[i] > 20000;
        fprintf(stderr, "[pace] game %d frames: ms p50 %.1f p95 %.1f max %.1f, >20 ms %d\n", n,
                dt[n / 2] / 1000.0, dt[n * 95 / 100] / 1000.0, dt[n - 1] / 1000.0, over);
        n = 0;
        t0 = now;
    }
}

void vc3_quest_frame(GLuint fbo, int pw, int ph, int eyes, int lw, int lh)
{
    static GLuint dst_fbo;
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
    if (!f->tex || f->w != pw || f->h != ph) {
        if (f->tex)
            glDeleteTextures(1, &f->tex);
        glGenTextures(1, &f->tex);
        glBindTexture(GL_TEXTURE_2D, f->tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, pw, ph);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        f->w = pw;
        f->h = ph;
    }
    if (f->fence) {
        glDeleteSync(f->fence);
        f->fence = 0;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->tex, 0);
    glBlitFramebuffer(0, 0, pw, ph, 0, 0, pw, ph, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    {   /* VC3_QUEST_DUMP=first,count: those game frames (both eyes) as PPM
         * files beside the game, to see frame-to-frame changes (stalls) */
        static long first = -2, count, n;
        if (first == -2) {
            const char *v = getenv("VC3_QUEST_DUMP");
            first = -1;
            if (v && sscanf(v, "%ld,%ld", &first, &count) != 2)
                first = -1;
        }
        if (first >= 0 && n >= first && n < first + count) {
            static unsigned char *px;
            char path[256];
            const char *dir = "/sdcard/VirtuaCop3/dump";
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
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    f->lw = lw;
    f->lh = lh;
    f->eyes = eyes;
    f->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();                          /* the fence must reach the GPU */

    pace_game_frame();
    pthread_mutex_lock(&s_lock);
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

/* The newest frame, on the host's thread; NULL before the first. */
static Frame *take_frame(int *is_new)
{
    Frame *f;

    pthread_mutex_lock(&s_lock);
    *is_new = s_ready_seq != s_shown_seq;
    if (*is_new) {
        int t = s_front;
        s_front = s_ready;
        s_ready = t;
        s_shown_seq = s_ready_seq;
    }
    pthread_mutex_unlock(&s_lock);
    f = &s_frame[s_front];
    if (!f->tex)
        return NULL;
    /* (the read_done fence of the frame given up stays: the back end waits
     * on it before writing that frame again) */
    if (*is_new && f->fence)
        glWaitSync(f->fence, 0, GL_TIMEOUT_IGNORED);
    return f;
}

/* ── Settings: the screen, the gun ────────────────────────────────────── */

static float s_dist = 2.6f, s_height = 1.9f;    /* metres */
#define EYE_DISTANCE 0.064f                       /* metres */

void vc3_gl_stereo_separation(float ndc);         /* gl_backend.c */

enum { LASER_ON, LASER_DOT, LASER_OFF };
static const char *const SIGHT_KEY[QO_SIGHT_COUNT] = { "iron", "reddot", "holo" };
static const char *const LASER_KEY[3] = { "laser", "dot", "off" };

static struct {
    int sight;          /* QO_SIGHT_* */
    int laser;          /* LASER_* */
    int show_gun;
    int left_handed;
    int auto_coins;     /* a coin at start-up and with every Start */
} s_vr = { QO_SIGHT_IRON, LASER_DOT, 1, 0, 1 };

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
    if ((v = vc3_setting_get("vr_height=")) && atof(v) > 0.3)
        s_height = (float)atof(v);
    s_vr.sight = key_index(vc3_setting_get("vr_sight="), SIGHT_KEY, QO_SIGHT_COUNT,
                           QO_SIGHT_IRON);
    s_vr.laser = key_index(vc3_setting_get("vr_laser="), LASER_KEY, 3, LASER_DOT);
    if ((v = vc3_setting_get("vr_gun=")))
        s_vr.show_gun = strcmp(v, "off") != 0;
    if ((v = vc3_setting_get("vr_hand=")))
        s_vr.left_handed = !strcmp(v, "left");
    if ((v = vc3_setting_get("vr_coins=")))
        s_vr.auto_coins = strcmp(v, "manual") != 0;
}

static void screen_save(void)
{
    char b[32];
    snprintf(b, sizeof b, "%.2f", s_dist);
    vc3_setting_put("vr_distance=", b);
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

static XrSwapchain s_chain;
static uint32_t s_chain_n, s_chain_w, s_chain_h;
static XrSwapchainImageOpenGLESKHR s_chain_img[8];
static GLuint s_chain_fbo[8], s_read_fbo;
static int64_t s_chain_fmt;
static int s_srgb_control;

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

static int xr_init(void)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    JavaVM *vm = NULL;
    jobject activity = (jobject)SDL_AndroidGetActivity();
    PFN_xrInitializeLoaderKHR init = NULL;
    XrLoaderInitInfoAndroidKHR li = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
    const char *ext[4] = { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
                           XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME };
    uint32_t next = 2, n = 0, i;
    XrExtensionProperties *props;
    int have_rate = 0;
    XrInstanceCreateInfoAndroidKHR ai = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
    XrInstanceCreateInfo ic = { XR_TYPE_INSTANCE_CREATE_INFO };
    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    PFN_xrGetOpenGLESGraphicsRequirementsKHR req_fn = NULL;
    XrGraphicsRequirementsOpenGLESKHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
    XrGraphicsBindingOpenGLESAndroidKHR gb = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
    XrSessionCreateInfo sc = { XR_TYPE_SESSION_CREATE_INFO };
    XrReferenceSpaceCreateInfo rc = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    const char *exts;

    if (!env || !activity)
        return 0;
    (*env)->GetJavaVM(env, &vm);
    XRCHECK(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                  (PFN_xrVoidFunction *)&init));
    li.applicationVM = vm;
    li.applicationContext = activity;
    XRCHECK(init((XrLoaderInitInfoBaseHeaderKHR *)&li));

    xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL);
    props = (XrExtensionProperties *)calloc(n ? n : 1, sizeof *props);
    if (!props)
        return 0;
    for (i = 0; i < n; i++)
        props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    xrEnumerateInstanceExtensionProperties(NULL, n, &n, props);
    for (i = 0; i < n; i++)
        if (!strcmp(props[i].extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
            have_rate = 1;
    free(props);
    if (have_rate)
        ext[next++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;

    ai.applicationVM = vm;
    ai.applicationActivity = activity;
    ic.next = &ai;
    ic.enabledExtensionCount = next;
    ic.enabledExtensionNames = ext;
    strcpy(ic.applicationInfo.applicationName, "Virtua Cop 3 VR");
    strcpy(ic.applicationInfo.engineName, "xboxrecomp");
    ic.applicationInfo.applicationVersion = 1;
    ic.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    XRCHECK(xrCreateInstance(&ic, &s_inst));
    if (have_rate)
        xrGetInstanceProcAddr(s_inst, "xrRequestDisplayRefreshRateFB",
                              (PFN_xrVoidFunction *)&s_request_rate);

    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XRCHECK(xrGetSystem(s_inst, &sgi, &s_sys));
    XRCHECK(xrGetInstanceProcAddr(s_inst, "xrGetOpenGLESGraphicsRequirementsKHR",
                                  (PFN_xrVoidFunction *)&req_fn));
    XRCHECK(req_fn(s_inst, s_sys, &req));

    gb.display = s_dpy;
    gb.config = s_cfg;
    gb.context = s_ctx;
    sc.next = &gb;
    sc.systemId = s_sys;
    XRCHECK(xrCreateSession(s_inst, &sc, &s_sess));

    rc.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rc.poseInReferenceSpace.orientation.w = 1;
    XRCHECK(xrCreateReferenceSpace(s_sess, &rc, &s_local));
    if (!xr_input_init())
        return 0;

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
    return 1;
}

/* A swapchain the size of the frames (they change with the resolution and
 * aspect settings). */
static int chain_fit(uint32_t w, uint32_t h)
{
    XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    uint32_t i;

    if (s_chain && s_chain_w == w && s_chain_h == h)
        return 1;
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
                    if (s_request_rate)
                        s_request_rate(s_sess, 120.0f);
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

/* The overlay's layer: one swapchain per eye, multisampled where the GPU
 * resolves it on chip (EXT_multisampled_render_to_texture, free on Adreno). */
typedef struct {
    XrSwapchain chain;
    uint32_t n, w, h;
    XrSwapchainImageOpenGLESKHR img[8];
    GLuint fbo[8], depth;
} Eye;
static Eye s_eye[2];
static int s_overlay_ok = -1;           /* -1 not yet tried */

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
        /* three quarters of the headset's own size: the gun is small and
         * the samples smooth it; the layer is cleared and drawn each frame */
        y->w = vcv[e].recommendedImageRectWidth * 3 / 4;
        y->h = vcv[e].recommendedImageRectHeight * 3 / 4;
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

/* ── The settings panel ───────────────────────────────────────────────────
 * The left controller's menu button opens it to the player's left, turned
 * towards them, the game's screen still in view (as in The House of the
 * Dead 4 VR). Point the gun at a row and pull the trigger; the game gets no
 * input meanwhile. Holding the other hand's upper button (B / Y) for a
 * second opens and closes it too. */
enum { M_SIGHT, M_LASER, M_GUN, M_HAND, M_STEREO, M_ASPECT, M_RES, M_DIST, M_SIZE,
       M_COINS, M_COIN, M_RECENTRE, M_TEST, M_SERVICE, M_RESET, M_CLOSE, M_COUNT };
static struct {
    int open, hover, dirty;
    float c[3], right[3], up[3], n[3], hw, hh;  /* where it hangs, LOCAL metres */
    int test_frames, service_frames, coin;
    int reset_armed;            /* "Reset game" chosen once: a second time resets */
} s_menu;

static void menu_items(XmItem *it)
{
    static const char *const sight[] = { "Iron sights", "Red dot", "Holographic" };
    static const char *const laser[] = { "Laser and dot", "Dot only", "Off" };
    static const char *const labels[M_COUNT] = {
        "Sight", "Laser", "Gun model", "Gun hand", "Stereo 3D", "Aspect", "Resolution",
        "Screen distance", "Screen size", "Coins", "Insert a coin", "Recentre view",
        "Arcade Test button", "Arcade Service button", "Reset game", "Close" };
    int i;
    for (i = 0; i < M_COUNT; i++) {
        it[i].label = labels[i];
        it[i].value[0] = 0;
    }
    snprintf(it[M_SIGHT].value, 40, "%s", sight[s_vr.sight]);
    snprintf(it[M_LASER].value, 40, "%s", laser[s_vr.laser]);
    snprintf(it[M_GUN].value, 40, "%s", s_vr.show_gun ? "Shown" : "Hidden");
    snprintf(it[M_HAND].value, 40, "%s", s_gun == HAND_L ? "Left" : "Right");
    snprintf(it[M_STEREO].value, 40, "%s", vc3_stereo_get() ? "On" : "Off");
    snprintf(it[M_ASPECT].value, 40, "%s", vc3_wide_get() ? "16:9" : "4:3");
    snprintf(it[M_RES].value, 40, "%dx%s", vc3_scale_get(),
             vc3_scale_get() > 1 && vc3_stereo_get() ? " (slow in 3D)" : "");
    snprintf(it[M_DIST].value, 40, "%.1f m", s_dist);
    snprintf(it[M_SIZE].value, 40, "%.1f m tall", s_height);
    snprintf(it[M_COINS].value, 40, "%s", s_vr.auto_coins ? "Automatic" : "By menu");
    if (s_menu.reset_armed)
        snprintf(it[M_RESET].value, 40, "Pull again to reset");
}

static void menu_paint(void)
{
    static unsigned char img[XM_W * XM_H * 4];
    XmItem it[M_COUNT];
    menu_items(it);
    xm_paint(img, "Virtua Cop 3 - settings", it, M_COUNT, s_menu.hover,
             "Point, pull the trigger. Menu button: close");
    qo_panel_upload(img, XM_W, XM_H);
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
    case M_SIGHT: s_vr.sight = (s_vr.sight + 1) % QO_SIGHT_COUNT; break;
    case M_LASER: s_vr.laser = (s_vr.laser + 1) % 3; break;
    case M_GUN: s_vr.show_gun = !s_vr.show_gun; break;
    case M_HAND:
        s_gun = 1 - s_gun;
        s_vr.left_handed = s_gun == HAND_L;
        break;
    case M_STEREO: vc3_stereo_set(!vc3_stereo_get()); break;
    case M_ASPECT: vc3_wide_set(!vc3_wide_get()); break;
    case M_RES: vc3_scale_set(vc3_scale_get() % VC3_SCALE_MAX + 1); break;
    case M_DIST: cycle(&s_dist, dists, 6); break;
    case M_SIZE: cycle(&s_height, sizes, 5); break;
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
 * smaller/larger). */
static float s_hit_u = -1, s_hit_v = -1;
static QoFrame s_qo;
static XrTime s_shot_t;

static void gun_frame(XrTime t, const Frame *f)
{
    static int was[2][5], was_menu, was_start;
    static float stick_cool;
    static XrTime panel_held;
    static unsigned frames;
    XrActiveActionSet as = { 0 };
    XrActionsSyncInfo sy = { XR_TYPE_ACTIONS_SYNC_INFO };
    double ux = -1, uy = -1;
    int buttons = 0, h, off, now[2][5], menu_btn, start, coin, on_screen = 0;
    float o[3], d[3];
    int located = 0;

    as.actionSet = s_set;
    sy.countActiveActionSets = 1;
    sy.activeActionSets = &as;
    if (XR_FAILED(xrSyncActions(s_sess, &sy)))
        return;
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
            fake_menu = getenv("VC3_XR_FAKE_MENU") && *getenv("VC3_XR_FAKE_MENU") == '1';
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
        if (fake < 0)
            fake = getenv("VC3_XR_FAKE_GUN") && *getenv("VC3_XR_FAKE_GUN") == '1';
        if (!got && fake) {
            double sec = (double)t / 1e9;
            float yaw = (float)(0.06 * sin(sec * 0.5)), pitch = 0.0f;
            float sh = qo_sight_height(s_vr.sight);
            loc.pose.orientation = (XrQuaternionf){ 0, sinf(yaw / 2), 0, cosf(yaw / 2) };
            (void)pitch;
            loc.pose.position = (XrVector3f){ 0.0f, -sh - 0.002f, -0.30f };
            got = 1;
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
    if (f && s_hit_u >= 0 && s_hit_u <= 1 && s_hit_v >= 0 && s_hit_v <= 1) {
        double gw = f->lh * 4.0 / 3.0, gx = s_hit_u * f->lw - (f->lw - gw) / 2.0;
        on_screen = 1;
        if (gx >= 0 && gx < gw) {
            ux = gx / gw;
            uy = s_hit_v;
        }
    }
    s_qo.beam = s_vr.laser == LASER_ON;
    s_qo.dot = on_screen && s_vr.laser != LASER_OFF;
    s_qo.sight = s_vr.sight;
    s_qo.show_gun = s_vr.show_gun;

    if (s_menu.open) {          /* the gun points at the panel; the game sees nothing */
        float u, v, hit[3];
        int on = located && menu_hit(o, d, &u, &v, hit);
        int row = on ? xm_row_at(u, v, M_COUNT) : -1;
        if (row != s_menu.hover) {
            s_menu.hover = row;
            s_menu.dirty = 1;
        }
        if (on)
            memcpy(s_qo.end, hit, sizeof hit);
        s_qo.beam = 1;
        s_qo.dot = on;
        if (now[s_gun][0] && !was[s_gun][0] && row >= 0) {
            menu_choose(row);
            rumble(s_gun, 0.25f, 15);
        }
        if (s_menu.dirty)
            menu_paint();
        ux = uy = -1;
    } else {
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
        s_qo.kick = ms < 140 ? (float)(1.0 - ms / 140.0) : 0.0f;
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
    vc3_input_vr(ux, uy, buttons);

    /* The other hand's thumbstick moves the screen. */
    {
        XrVector2f st = act_vec2(a_stick_xy, off);
        if (!s_menu.open && (fabsf(st.y) > 0.3f || fabsf(st.x) > 0.3f)) {
            s_dist -= st.y * 0.02f;
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
    /* Infinity one eye distance apart on the screen: 2 * eye / width in NDC. */
    if (f)
        vc3_gl_stereo_separation(2.0f * EYE_DISTANCE
                                 / (s_height * (float)f->lw / (float)f->lh));
    memcpy(was, now, sizeof was);
}

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

static void xr_frame(void)
{
    XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { XR_TYPE_FRAME_STATE };
    XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
    XrCompositionLayerQuad quad[2];
    XrCompositionLayerProjection proj = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerProjectionView pv[2];
    const XrCompositionLayerBaseHeader *layers[3];
    uint32_t nl = 0;
    Frame *f;
    int is_new;

    if (XR_FAILED(xrWaitFrame(s_sess, &wi, &fs)))
        return;
    if (XR_FAILED(xrBeginFrame(s_sess, &bi)))
        return;
    f = take_frame(&is_new);
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
    }
    if (s_overlay_ok < 0) {
        s_overlay_ok = overlay_init();
        if (!s_overlay_ok)
            fprintf(stderr, "[xr] no overlay: the aim dot is drawn into the picture\n");
    }
    if (s_state == XR_SESSION_STATE_FOCUSED)
        gun_frame(fs.predictedDisplayTime, f);
    else
        s_qo.gun_valid = 0;

    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (fs.shouldRender && f && chain_fit((uint32_t)f->w, (uint32_t)f->h)) {
        XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        XrSwapchainImageWaitInfo wti = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        uint32_t idx = 0;

        wti.timeout = XR_INFINITE_DURATION;
        if (XR_SUCCEEDED(xrAcquireSwapchainImage(s_chain, &ai, &idx))
            && XR_SUCCEEDED(xrWaitSwapchainImage(s_chain, &wti))) {
            uint32_t e;
            float c[3], yq = s_anchor_yaw * 0.5f;

            draw_frame(s_chain_fbo[idx], f, (GLsizei)s_chain_w, (GLsizei)s_chain_h,
                       s_overlay_ok <= 0);
            frame_read_done(f);
            xrReleaseSwapchainImage(s_chain, &ri);
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
                q->subImage.swapchain = s_chain;
                q->subImage.imageRect.offset.x = (int32_t)(e * s_chain_w / (uint32_t)f->eyes);
                q->subImage.imageRect.extent.width = (int32_t)(s_chain_w / (uint32_t)f->eyes);
                q->subImage.imageRect.extent.height = (int32_t)s_chain_h;
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
    /* the gun and the panel, over the screen */
    if (fs.shouldRender && s_overlay_ok > 0 && s_views_ok && (s_qo.gun_valid || s_menu.open)
        && draw_overlay(pv)) {
        proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        proj.space = s_local;
        proj.viewCount = 2;
        proj.views = pv;
        layers[nl++] = (const XrCompositionLayerBaseHeader *)&proj;
    }
    ei.layerCount = nl;
    ei.layers = layers;
    xrEndFrame(s_sess, &ei);
}

/* ── Without a headset: the SDL window ────────────────────────────────── */

static void flat_frame(void)
{
    int is_new, ww = 0, wh = 0;
    Frame *f = take_frame(&is_new);

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

static int (*s_guest_main)(void);
static volatile int s_guest_rc, s_guest_done;

static void *guest_thread(void *arg)
{
    (void)arg;
    s_guest_rc = s_guest_main();
    s_guest_done = 1;
    return NULL;
}

/* main.c's vc3_relaunch: the activity starts a fresh process (Restart). */
void vc3_quest_restart(void)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (env && activity) {
        jclass cls = (*env)->GetObjectClass(env, activity);
        jmethodID m = (*env)->GetMethodID(env, cls, "restartGame", "()V");
        if (m)
            (*env)->CallVoidMethod(env, activity, m);
    }
    exit(0);
}

int vc3_quest_run(int (*guest_main)(void))
{
    pthread_attr_t attr;
    pthread_t th;
    int xr = 0, frames_logged = 0;
    uint64_t t_log = 0, in_log = 0;

    /* SDL owns the activity's surface; its GLES 3.2 context is the one the
     * headset is given, and the back end's shares with it. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[quest] SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    s_win = SDL_CreateWindow("Virtua Cop 3", 0, 0, 1280, 720,
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

    screen_load();
    if (getenv("VC3_NO_XR") && *getenv("VC3_NO_XR") == '1')
        fprintf(stderr, "[quest] VC3_NO_XR=1: the window, not the headset\n");
    else
        xr = xr_init();
    if (!xr)
        fprintf(stderr, "[quest] no OpenXR headset: showing the game in the window\n");

    /* The guest on a thread of its own, with a big stack: lifted code
     * recurses on the native stack. */
    s_guest_main = guest_main;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    if (pthread_create(&th, &attr, guest_thread, NULL) != 0) {
        fprintf(stderr, "[quest] cannot start the guest thread\n");
        return 1;
    }
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
            flat_frame();
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
    exit(s_guest_done ? s_guest_rc : 0);
}

#endif /* __ANDROID__ */
