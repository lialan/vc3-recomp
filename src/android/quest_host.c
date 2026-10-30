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
 * The gun is the controller: its aim ray meets the screen, and that point in
 * the game's 4:3 picture is where the gun reads (host_input.c), with a dot
 * drawn there. Trigger fires; pulled with the gun off the screen, or the
 * grip, reloads, as pointing a light gun away does. The other hand's grip
 * is the cabinet's foot pedal. A / X: coin and start;
 * B / Y: coin; left menu button: the cabinet's test switch. The other
 * hand's thumbstick moves the screen (up/down: nearer/farther; left/right:
 * smaller/larger); thumbstick clicks: aspect 4:3/16:9 (left), internal
 * resolution 1x-4x (right). All of it is saved in the settings file.
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
    GLsizei w, h;            /* as drawn */
    int lw, lh;              /* the picture's shape (guest pixels) */
    GLsync fence;
} Frame;

static Frame s_frame[3];
static int s_back = 0, s_ready = 1, s_front = 2;
static uint64_t s_ready_seq, s_shown_seq;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t s_frames_in;   /* for the log */

void vc3_quest_frame(GLuint fbo, int pw, int ph, int lw, int lh)
{
    static GLuint dst_fbo;
    Frame *f = &s_frame[s_back];        /* this thread alone touches s_back */
    int t;

    if (!dst_fbo)
        glGenFramebuffers(1, &dst_fbo);
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
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    f->lw = lw;
    f->lh = lh;
    f->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();                          /* the fence must reach the GPU */

    pthread_mutex_lock(&s_lock);
    t = s_ready;
    s_ready = s_back;
    s_back = t;
    s_ready_seq++;
    s_frames_in++;
    pthread_mutex_unlock(&s_lock);
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
    if (*is_new && f->fence)
        glWaitSync(f->fence, 0, GL_TIMEOUT_IGNORED);
    return f;
}

/* ── Settings: where the screen is ────────────────────────────────────── */

static float s_dist = 2.6f, s_height = 1.9f;    /* metres */

static void screen_load(void)
{
    const char *v;
    if ((v = vc3_setting_get("vr_distance=")) && atof(v) > 0.5)
        s_dist = (float)atof(v);
    if ((v = vc3_setting_get("vr_height=")) && atof(v) > 0.3)
        s_height = (float)atof(v);
}

static void screen_save(void)
{
    char b[32];
    snprintf(b, sizeof b, "%.2f", s_dist);
    vc3_setting_put("vr_distance=", b);
    snprintf(b, sizeof b, "%.2f", s_height);
    vc3_setting_put("vr_height=", b);
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

/* Where the gun points: on the picture (u, v in 0..1 from its top-left),
 * and in the game's 4:3 part of it (ux, uy; negative when off it). */
static float s_hit_u = -1, s_hit_v = -1;

static void gun_frame(XrTime t, const Frame *f)
{
    static int was[2][4];
    static float stick_cool;
    XrActiveActionSet as = { 0 };
    XrActionsSyncInfo sy = { XR_TYPE_ACTIONS_SYNC_INFO };
    double ux = -1, uy = -1;
    int buttons = 0, h, off = 1 - s_gun, now[2][4];
    float trig[2];

    as.actionSet = s_set;
    sy.countActiveActionSets = 1;
    sy.activeActionSets = &as;
    if (XR_FAILED(xrSyncActions(s_sess, &sy)))
        return;
    for (h = 0; h < 2; h++) {
        trig[h] = act_float(a_trigger, h);
        now[h][0] = trig[h] > 0.5f;
        now[h][1] = act_bool(a_lower, h);
        now[h][2] = act_bool(a_upper, h);
        now[h][3] = act_bool(a_stick_click, h);
    }
    /* The hand that pulls its trigger is the gun hand. */
    if (now[off][0] && !was[off][0]) {
        s_gun = off;
        off = 1 - s_gun;
    }

    s_hit_u = s_hit_v = -1;
    {
        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        if (XR_SUCCEEDED(xrLocateSpace(s_aim_space[s_gun], s_local, t, &loc))
            && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
            && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && f) {
            static const float fwd[3] = { 0, 0, -1 };
            float d[3], hw = s_height * (float)f->lw / (float)f->lh * 0.5f, hh = s_height * 0.5f;
            const XrVector3f *o = &loc.pose.position;
            qrot(loc.pose.orientation, fwd, d);
            if (d[2] < -1e-4f) {
                float tt = (-s_dist - o->z) / d[2];
                float x = o->x + tt * d[0], y = o->y + tt * d[1];
                if (tt > 0) {
                    s_hit_u = (x + hw) / (2 * hw);
                    s_hit_v = (hh - y) / (2 * hh);
                }
            }
        }
    }
    if (f && s_hit_u >= 0 && s_hit_u <= 1 && s_hit_v >= 0 && s_hit_v <= 1) {
        double gw = f->lh * 4.0 / 3.0, gx = s_hit_u * f->lw - (f->lw - gw) / 2.0;
        if (gx >= 0 && gx < gw) {
            ux = gx / gw;
            uy = s_hit_v;
        }
    }

    /* Trigger: a shot; with the gun off the screen, a reload. Grip too. */
    if (now[s_gun][0])
        buttons |= ux < 0 ? VC3_VR_RELOAD : VC3_VR_FIRE;
    if (act_float(a_grip, s_gun) > 0.6f)
        buttons |= VC3_VR_RELOAD;
    if (act_float(a_grip, off) > 0.6f)          /* the foot pedal */
        buttons |= VC3_VR_PEDAL;
    if (now[s_gun][0] && !was[s_gun][0])
        rumble(s_gun, ux < 0 ? 0.3f : 0.8f, ux < 0 ? 40 : 25);
    /* A / X: a coin and start (a credit is then always there); B / Y: coin. */
    if (now[0][1] || now[1][1])
        buttons |= VC3_VR_START | VC3_VR_COIN;
    if (now[0][2] || now[1][2])
        buttons |= VC3_VR_COIN;
    if (act_bool(a_menu, HAND_L))
        buttons |= VC3_VR_TEST;
    vc3_input_vr(ux, uy, buttons);

    /* The other hand moves the screen; thumbstick clicks change aspect
     * (left) and internal resolution (right). */
    {
        XrVector2f st = act_vec2(a_stick_xy, off);
        if (fabsf(st.y) > 0.3f || fabsf(st.x) > 0.3f) {
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
    if (now[HAND_L][3] && !was[HAND_L][3])
        vc3_wide_set(!vc3_wide_get());
    if (now[HAND_R][3] && !was[HAND_R][3])
        vc3_scale_set(vc3_scale_get() % VC3_SCALE_MAX + 1);
    memcpy(was, now, sizeof was);
}

/* The frame into the swapchain image, upright (the surfaces hold the picture
 * top row first, GL's bottom), with the aim dot. */
static void draw_frame(GLuint dst_fbo, const Frame *f, GLsizei w, GLsizei h, int dot)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_read_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_fbo);
    glDisable(GL_SCISSOR_TEST);
    if (s_srgb_control)
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    glBlitFramebuffer(0, 0, f->w, f->h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    if (dot && s_hit_u >= 0 && s_hit_u <= 1 && s_hit_v >= 0 && s_hit_v <= 1) {
        int cx = (int)(s_hit_u * w), cy = (int)((1 - s_hit_v) * h);
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

static void xr_frame(void)
{
    XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { XR_TYPE_FRAME_STATE };
    XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
    XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    const XrCompositionLayerBaseHeader *layers[1];
    Frame *f;
    int is_new;

    if (XR_FAILED(xrWaitFrame(s_sess, &wi, &fs)))
        return;
    if (XR_FAILED(xrBeginFrame(s_sess, &bi)))
        return;
    f = take_frame(&is_new);
    if (s_state == XR_SESSION_STATE_FOCUSED)
        gun_frame(fs.predictedDisplayTime, f);

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
            draw_frame(s_chain_fbo[idx], f, (GLsizei)s_chain_w, (GLsizei)s_chain_h, 1);
            xrReleaseSwapchainImage(s_chain, &ri);

            quad.space = s_local;               /* opaque: no blend flags */
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = s_chain;
            quad.subImage.imageRect.extent.width = (int32_t)s_chain_w;
            quad.subImage.imageRect.extent.height = (int32_t)s_chain_h;
            quad.pose.orientation.w = 1;
            quad.pose.position.z = -s_dist;
            quad.size.height = s_height;
            quad.size.width = s_height * (float)f->lw / (float)f->lh;
            layers[0] = (const XrCompositionLayerBaseHeader *)&quad;
            ei.layerCount = 1;
            ei.layers = layers;
        }
    }
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
        glBlitFramebuffer(0, 0, f->w, f->h, (ww - dw) / 2, (wh + dh) / 2, (ww + dw) / 2,
                          (wh - dh) / 2, GL_COLOR_BUFFER_BIT, GL_LINEAR);
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
