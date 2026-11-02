/*
 * Host controls for the emulated JVS I/O board.
 *
 * The toolkit's window (xbox_HostWindowMain) pumps SDL events on the main
 * thread; an event watch keeps the mouse and keyboard state from them, and
 * the guest thread reads it when the game polls the board.
 *
 *   mouse            player 1 gun: position in the 4:3 picture
 *   left button      trigger
 *   right button     reload (fires off-screen)
 *   middle button    change (the gun's second button; JVS push 3)
 *   Space            pedal (JVS push 4: the cabinet's foot pedal)
 *   5 / 6            coin, slot 1 / 2
 *   1 / 2            start, player 1 / 2
 *   F2               test       F1   service
 *   F10              display 60 <-> 120 fps (display_settings.c; not the board)
 *   (none)           fire / reload from the keyboard, if bound
 *
 * The keys are the defaults: Controls in the menu bar (macos_menu.m) binds
 * each action to another key, saved as key_<action>= in the settings file.
 *
 * VC3_AUTOKEYS scripts presses for unattended runs: "t:key,t:key", with t in
 * seconds since the first poll and key one of test, service, coin, start,
 * fire, reload (each held for half a second), e.g.
 * VC3_AUTOKEYS="3:test,8:coin,9:start".
 * AUTOKEYS_CLOCK=flips measures that timeline in executor flips at 60 Hz
 * instead of wall time, for comparisons between differently paced renderers.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

/* A gamepad as the gun: the flat desktop game only. With a headset
 * (Android, CHIHIRO_XR_DESKTOP) the controllers come through OpenXR. */
#if !defined(__ANDROID__) && !defined(CHIHIRO_XR_DESKTOP)
#define VC3_HOST_GAMEPAD 1
#else
#define VC3_HOST_GAMEPAD 0
#endif

#include "chihiro/chihiro.h"
#include "display_settings.h"
#include "host_input.h"
#include "chihiro/ch_env.h"
#include "chihiro/chihiro_profile.h"
#include "nv2a_backend.h"

#define AUTO_MAX     512   /* at 32 the E.S. scenarios' later pedal presses were dropped */
#define AUTO_HOLD_MS 500   /* > one poll at the current frame rate */

typedef struct { uint32_t at_ms; char key[12]; } auto_t;

static auto_t s_auto[AUTO_MAX];
static int s_nauto = -1;
static uint64_t s_t0;
static uint64_t s_flip0;
static int s_auto_flip_clock;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void auto_parse(void)
{
    const char *s = ch_env("AUTOKEYS");
    s_nauto = 0;
    while (s && *s && s_nauto < AUTO_MAX) {
        double t;
        char key[12];
        int n = 0;
        if (sscanf(s, "%lf:%11[a-z]%n", &t, key, &n) != 2)
            break;
        s_auto[s_nauto].at_ms = (uint32_t)(t * 1000.0);
        snprintf(s_auto[s_nauto].key, sizeof(s_auto[0].key), "%s", key);
        s_nauto++;
        s += n;
        if (*s == ',')
            s++;
    }
}

/* How long a scripted press is held: AUTO_HOLD_MS, or for fire
 * VC3_AUTOFIRE_MS (a real click is ~80 ms; for testing short ones). */
static uint32_t auto_hold_ms(const char *key)
{
    static int fire_ms = -1;
    if (fire_ms < 0) {
        const char *e = ch_env("AUTOFIRE_MS");
        fire_ms = e ? atoi(e) : 0;
    }
    return !strcmp(key, "fire") && fire_ms > 0 ? (uint32_t)fire_ms : AUTO_HOLD_MS;
}

/* Is the scripted key held right now? */
static int auto_held(const char *key, uint32_t t)
{
    static unsigned char logged[AUTO_MAX];   /* per script entry */
    int i;
    for (i = 0; i < s_nauto; i++)
        if (!strcmp(s_auto[i].key, key) && t >= s_auto[i].at_ms
                && t < s_auto[i].at_ms + auto_hold_ms(key)) {
            if (!logged[i]) {
                logged[i] = 1;
                fprintf(stderr, "[INPUT] auto %s at %.1fs\n", key, t / 1000.0);
            }
            return 1;
        }
    return 0;
}

/* Input state, kept from the SDL events themselves (SDL_AddEventWatch), so
 * whatever enters SDL's queue -- the OS's events or VC3_INPUT_SELFTEST's --
 * reaches the board the same way. Written on the thread that pumps events,
 * read on the guest thread; single words, and a torn read costs a frame. */
static volatile int s_mx = -1, s_my = -1;
static volatile Uint32 s_mwin;               /* window under the mouse, 0 none */
static volatile Uint32 s_buttons;
static volatile Uint8 s_keys[SDL_NUM_SCANCODES];

/* Key bindings: one scancode per action (0: none), the defaults above
 * unless the settings file says otherwise. Written on the thread that pumps
 * events (the menu, a key learnt), read when the board is polled. */
static const struct {
    const char *id, *name;
    SDL_Scancode def;
} k_action[VC3_KEY_COUNT] = {
    { "coin1",   "Coin (player 1)",  SDL_SCANCODE_5 },
    { "coin2",   "Coin (player 2)",  SDL_SCANCODE_6 },
    { "start1",  "Start (player 1)", SDL_SCANCODE_1 },
    { "start2",  "Start (player 2)", SDL_SCANCODE_2 },
    { "fire",    "Fire (keyboard)",  SDL_SCANCODE_UNKNOWN },
    { "reload",  "Reload (keyboard)", SDL_SCANCODE_UNKNOWN },
    { "pedal",   "Pedal",            SDL_SCANCODE_SPACE },
    { "change",  "Change (keyboard)", SDL_SCANCODE_UNKNOWN },
    { "test",    "Test",             SDL_SCANCODE_F2 },
    { "service", "Service",          SDL_SCANCODE_F1 },
    { "fps",     "Toggle 60/120 fps", SDL_SCANCODE_F10 },
};
static volatile int s_bind[VC3_KEY_COUNT];
static volatile int s_bind_loaded;
static volatile int s_learn = -1;       /* action waiting for a key, -1 none */

static void bind_save(int a)
{
    char key[32];
    snprintf(key, sizeof key, "key_%s=", k_action[a].id);
    vc3_setting_put(key, s_bind[a] ? SDL_GetScancodeName((SDL_Scancode)s_bind[a]) : "none");
}

static void bind_load(void)
{
    int a;
    if (s_bind_loaded)
        return;
    for (a = 0; a < VC3_KEY_COUNT; a++) {
        char key[32];
        const char *v;
        snprintf(key, sizeof key, "key_%s=", k_action[a].id);
        v = vc3_setting_get(key);
        if (!v)
            s_bind[a] = k_action[a].def;
        else if (!strcmp(v, "none"))
            s_bind[a] = 0;
        else {
            SDL_Scancode sc = SDL_GetScancodeFromName(v);
            s_bind[a] = sc != SDL_SCANCODE_UNKNOWN ? (int)sc : (int)k_action[a].def;
        }
    }
    /* One action per key here too: a key the player chose beats another
     * action's default. A fire key saved as Space before the pedal had
     * Space as its default held the trigger down with the pedal, so mouse
     * clicks made no new shots. */
    for (a = 0; a < VC3_KEY_COUNT; a++) {
        char key[32];
        int b;
        snprintf(key, sizeof key, "key_%s=", k_action[a].id);
        if (!s_bind[a] || vc3_setting_get(key))
            continue;                       /* unbound, or chosen */
        for (b = 0; b < VC3_KEY_COUNT; b++)
            if (b != a && s_bind[b] == s_bind[a]) {
                fprintf(stderr, "[INPUT] %s: %s is %s's; %s has no key\n",
                        k_action[a].name, SDL_GetScancodeName((SDL_Scancode)s_bind[a]),
                        k_action[b].name, k_action[a].name);
                s_bind[a] = 0;
                break;
            }
    }
    s_bind_loaded = 1;
}

const char *vc3_key_action_name(int a)
{
    return a >= 0 && a < VC3_KEY_COUNT ? k_action[a].name : "";
}

/* The key bound to an action, as SDL names it ("" when none). */
const char *vc3_key_name(int a)
{
    bind_load();
    if (a < 0 || a >= VC3_KEY_COUNT || !s_bind[a])
        return "";
    return SDL_GetScancodeName((SDL_Scancode)s_bind[a]);
}

/* The next key pressed becomes action a's (Escape: no key). */
void vc3_key_learn(int a)
{
    bind_load();
    s_learn = a >= 0 && a < VC3_KEY_COUNT ? a : -1;
    if (s_learn >= 0)
        fprintf(stderr, "[INPUT] press a key for %s (Escape: none)\n", k_action[a].name);
}

int vc3_key_learning(void)
{
    return s_learn;
}

void vc3_key_defaults(void)
{
    int a;
    bind_load();
    s_learn = -1;
    for (a = 0; a < VC3_KEY_COUNT; a++) {
        s_bind[a] = k_action[a].def;
        bind_save(a);
    }
    fprintf(stderr, "[INPUT] keys back to the defaults\n");
}

static int bound(const volatile Uint8 *k, int a)
{
    return k && s_bind[a] && k[s_bind[a]];
}

#if VC3_HOST_GAMEPAD
/* A gamepad (Steam Input's virtual pad on a Steam Frame or Deck, or any
 * SDL game controller) as the gun: either stick moves the aim over the 4:3
 * picture like a mouse, a crosshair shows where (the presenter draws it).
 *
 *   right / left stick   aim          right trigger, A     fire
 *   left trigger, B      reload       X                    change
 *   bumpers, Y           pedal        Start                coin, then start
 *   left stick click     coin         right stick click    start
 *   Back                 coin         (the mouse takes the gun back)
 *
 * The main thread's event pump updates the pad's state; the guest
 * thread's poll reads it and moves the aim. */
static SDL_GameController *volatile s_pad;
static volatile SDL_JoystickID s_pad_id = -1;
static volatile int s_pad_mouse;
static int s_pad_on;

extern void fb_present_set_aim(float ux, float uy) __attribute__((weak));

static void pad_added(int index)
{
    SDL_GameController *c;
    if (s_pad)
        return;
    c = SDL_GameControllerOpen(index);
    if (!c)
        return;
    s_pad_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
    s_pad = c;
    fprintf(stderr, "[INPUT] gamepad: %s\n", SDL_GameControllerName(c));
}

static void pad_removed(SDL_JoystickID id)
{
    if (!s_pad || id != s_pad_id)
        return;
    /* Not closed: the guest thread may be reading it this moment (a small
     * handle kept per unplug). */
    s_pad = NULL;
    s_pad_id = -1;
    fprintf(stderr, "[INPUT] gamepad gone\n");
}

/* A stick axis past its dead zone, -1..1, on a curve for fine aim. */
static double pad_stick(SDL_GameController *c, int axis)
{
    double v = SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)axis) / 32767.0, a = fabs(v);
    const double dz = 0.15;
    if (a <= dz)
        return 0.0;
    a = (a - dz) / (1.0 - dz);
    if (a > 1.0)
        a = 1.0;
    a = pow(a, 1.6);
    return v < 0 ? -a : a;
}

/* The gamepad's gun for this poll: 1 and the aim (0..1 over the 4:3
 * picture) and VC3_VR_* buttons while it is the gun, else 0. */
static int pad_poll(double *ux, double *uy, int *buttons)
{
    static double x = 0.5, y = 0.5;
    static uint64_t last, start_at;
    static int start_was;
    uint64_t now = now_ms();
    double dt = last ? (now - last) / 1000.0 : 0.0, sx, sy;
    int b = 0, k;
    last = now;
    if (dt > 0.05)
        dt = 0.05;
    SDL_GameController *c = s_pad;
    if (!c) {
        s_pad_on = 0;
        return 0;
    }
    sx = pad_stick(c, SDL_CONTROLLER_AXIS_RIGHTX) + pad_stick(c, SDL_CONTROLLER_AXIS_LEFTX);
    sy = pad_stick(c, SDL_CONTROLLER_AXIS_RIGHTY) + pad_stick(c, SDL_CONTROLLER_AXIS_LEFTY);
    if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000 || SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A))
        b |= VC3_VR_FIRE;
    if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000 || SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B))
        b |= VC3_VR_RELOAD;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X))
        b |= VC3_VR_CHANGE;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) || SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
        || SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y))
        b |= VC3_VR_PEDAL;
    /* Coin and start also on the stick clicks: a Steam Frame's controllers
     * have no Back or Start that reaches the game. */
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK)
        || SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK))
        b |= VC3_VR_COIN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK))
        b |= VC3_VR_START;
    /* Start: a coin, then start a moment later, so one press plays. */
    k = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START);
    if (k && !start_was)
        start_at = now;
    start_was = k;
    if (start_at && now - start_at < 150)
        b |= VC3_VR_COIN;
    else if (start_at && now - start_at < 450 && now - start_at >= 300)
        b |= VC3_VR_START;
    else if (start_at && now - start_at >= 450)
        start_at = 0;
    /* The pad becomes the gun when it is used, and gives it back to the
     * mouse when that moves. */
    if (s_pad_mouse) {
        s_pad_mouse = 0;
        s_pad_on = 0;
    }
    if (!s_pad_on && (b || sx != 0.0 || sy != 0.0))
        s_pad_on = 1;
    if (!s_pad_on)
        return 0;
    /* Across the picture in about 0.8 s at full tilt; the same speed in
     * both directions on the screen (the picture is 4:3). */
    x += sx * 1.25 * dt;
    y += sy * 1.25 * dt * 4.0 / 3.0;
    x = x < 0.0 ? 0.0 : x > 1.0 ? 1.0 : x;
    y = y < 0.0 ? 0.0 : y > 1.0 ? 1.0 : y;
    *ux = x;
    *uy = y;
    *buttons = b;
    return 1;
}
#endif

static int SDLCALL input_watch(void *ud, SDL_Event *e)
{
    (void)ud;
    switch (e->type) {
    case SDL_MOUSEMOTION:
        s_mx = e->motion.x;
        s_my = e->motion.y;
        s_mwin = e->motion.windowID;
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        s_mx = e->button.x;
        s_my = e->button.y;
        s_mwin = e->button.windowID;
        if (e->type == SDL_MOUSEBUTTONDOWN)
            s_buttons |= SDL_BUTTON(e->button.button);
        else
            s_buttons &= ~SDL_BUTTON(e->button.button);
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        if (e->type == SDL_KEYDOWN && !e->key.repeat && s_learn >= 0) {
            /* Learning: this key is the action's now, and does nothing else. */
            int a = s_learn;
            SDL_Scancode sc = e->key.keysym.scancode;
            int i;
            s_learn = -1;
            s_bind[a] = sc == SDL_SCANCODE_ESCAPE ? 0 : (int)sc;
            for (i = 0; i < VC3_KEY_COUNT; i++)     /* one action per key */
                if (i != a && s_bind[i] == s_bind[a] && s_bind[a]) {
                    s_bind[i] = 0;
                    bind_save(i);
                }
            bind_save(a);
            fprintf(stderr, "[INPUT] %s: %s\n", k_action[a].name,
                    s_bind[a] ? SDL_GetScancodeName(sc) : "none");
            break;
        }
        if (e->key.keysym.scancode < SDL_NUM_SCANCODES)
            s_keys[e->key.keysym.scancode] = e->type == SDL_KEYDOWN;
        if (e->type == SDL_KEYDOWN && !e->key.repeat && s_bind[VC3_KEY_FPS]
                && (int)e->key.keysym.scancode == s_bind[VC3_KEY_FPS])
            vc3_fps_toggle();
        break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_LEAVE)
            s_mwin = 0;
        break;
#if VC3_HOST_GAMEPAD
    case SDL_CONTROLLERDEVICEADDED:
        pad_added(e->cdevice.which);
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        pad_removed(e->cdevice.which);
        break;
#endif
    default:
        break;
    }
#if VC3_HOST_GAMEPAD
    if (e->type == SDL_MOUSEMOTION || e->type == SDL_MOUSEBUTTONDOWN)
        s_pad_mouse = 1;                 /* the mouse takes the gun back */
#endif
    return 0;
}

/* The Quest's controllers (src/android/quest_host.c), set by the headset
 * loop and read at the next poll: where the gun points in the game's 4:3
 * picture (0..1; negative off it) and VC3_VR_* buttons held. */
static volatile int s_vr_on, s_vr_buttons;
static volatile double s_vr_ux = -1.0, s_vr_uy = -1.0;

void vc3_input_vr(double ux, double uy, int buttons)
{
    s_vr_ux = ux;
    s_vr_uy = uy;
    s_vr_buttons = buttons;
    s_vr_on = 1;
}

/* VC3_INPUT_SELFTEST=1: play through the input stack without a person or OS
 * permissions -- SDL events pushed into SDL's queue at the game window, in
 * window pixels, reaching the board through input_watch like real ones:
 * coin, start, a shot at Mission Select, then shots across the screen with
 * a reload now and then. Times are seconds from the first board poll. */
static SDL_Window *game_window(void)
{
    Uint32 id;
    for (id = 1; id < 16; id++) {
        SDL_Window *w = SDL_GetWindowFromID(id);
        if (w)
            return w;
    }
    return NULL;
}

static void push_key(SDL_Window *w, SDL_Scancode sc, int down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.windowID = SDL_GetWindowID(w);
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = sc;
    e.key.keysym.sym = SDL_GetKeyFromScancode(sc);
    SDL_PushEvent(&e);
}

static void push_mouse(SDL_Window *w, double fx, double fy, int button, int down)
{
    SDL_Event e;
    int ww = 0, wh = 0;
    SDL_GetWindowSize(w, &ww, &wh);
    memset(&e, 0, sizeof e);
    if (button) {
        e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        e.button.windowID = SDL_GetWindowID(w);
        e.button.button = (Uint8)button;
        e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
        e.button.clicks = 1;
        e.button.x = (int)(fx * ww);
        e.button.y = (int)(fy * wh);
    } else {
        e.type = SDL_MOUSEMOTION;
        e.motion.windowID = SDL_GetWindowID(w);
        e.motion.x = (int)(fx * ww);
        e.motion.y = (int)(fy * wh);
    }
    SDL_PushEvent(&e);
}

static int SDLCALL selftest_thread(void *ud)
{
    static const struct { double t; char what; double x, y; } steps[] = {
        {30, 'c'}, {33, 's'}, {35, 's'},
        {50, 'f', 0.5, 0.55},                 /* Mission Select: middle file */
        {60, 's'}, {62, 's'},                 /* skip the briefing */
        {66, 'f', 0.35, 0.45}, {67, 'f', 0.65, 0.45}, {68, 'f', 0.5, 0.4},
        {69, 'f', 0.3, 0.55}, {70, 'f', 0.7, 0.55}, {71, 'r', 0.5, 0.5},
        {72, 'f', 0.45, 0.5}, {73, 'f', 0.55, 0.45}, {74, 'f', 0.4, 0.42},
        {75, 'f', 0.6, 0.5}, {76, 'f', 0.5, 0.48}, {77, 'r', 0.5, 0.5},
        {78, 'f', 0.38, 0.5}, {79, 'f', 0.62, 0.46}, {80, 'f', 0.5, 0.5},
    };
    uint64_t t0 = *(uint64_t *)ud;
    SDL_Window *w = NULL;
    size_t i;

    for (i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        uint64_t at = t0 + (uint64_t)(steps[i].t * 1000.0);
        while (now_ms() < at)
            SDL_Delay(10);
        if (!w && !(w = game_window())) {
            fprintf(stderr, "[INPUT] selftest: no window\n");
            return 0;
        }
        switch (steps[i].what) {
        case 'c': case 's': {
            SDL_Scancode sc = steps[i].what == 'c' ? SDL_SCANCODE_5 : SDL_SCANCODE_1;
            push_key(w, sc, 1);
            SDL_Delay(300);
            push_key(w, sc, 0);
            fprintf(stderr, "[INPUT] selftest key %s\n", steps[i].what == 'c' ? "5 (coin)" : "1 (start)");
            break;
        }
        default: {
            int b = steps[i].what == 'r' ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
            push_mouse(w, steps[i].x, steps[i].y, 0, 0);
            SDL_Delay(100);
            push_mouse(w, steps[i].x, steps[i].y, b, 1);
            SDL_Delay(b == SDL_BUTTON_RIGHT ? 400 : 150);
            push_mouse(w, steps[i].x, steps[i].y, b, 0);
            fprintf(stderr, "[INPUT] selftest %s at %.2f,%.2f\n",
                    b == SDL_BUTTON_RIGHT ? "reload" : "shot", steps[i].x, steps[i].y);
            break;
        }
        }
    }
    /* Then keep shooting through the first waves: a slow sweep, reloading
     * every eighth shot. */
    for (i = 0; i < 36; i++) {
        double fx = 0.3 + 0.4 * ((double)(i % 9) / 8.0), fy = 0.4 + 0.15 * ((i / 3) % 2);
        int b = (i % 8 == 7) ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
        uint64_t at = t0 + (uint64_t)((82.0 + i) * 1000.0);
        while (now_ms() < at)
            SDL_Delay(10);
        push_mouse(w, fx, fy, 0, 0);
        SDL_Delay(100);
        push_mouse(w, fx, fy, b, 1);
        SDL_Delay(b == SDL_BUTTON_RIGHT ? 400 : 150);
        push_mouse(w, fx, fy, b, 0);
    }
    fprintf(stderr, "[INPUT] selftest done\n");
    return 0;
}

/* Mouse position in the game picture, 0..1 each way, or -1 off the picture.
 * The renderer letterboxes the 4:3 frame inside the window. */
static void mouse_in_picture(int mx, int my, double *ux, double *uy)
{
    SDL_Window *w = s_mwin ? SDL_GetWindowFromID(s_mwin) : NULL;
    int ww = 0, wh = 0;
    double scale, pw, ph, ox, oy;

    *ux = *uy = -1.0;
    if (!w || mx < 0 || my < 0)
        return;
    /* The presenter gives its renderer a logical size, and SDL then rewrites
     * mouse events into that space (letterbox removed) before our watch sees
     * them. Dividing those by the window size halved the aim in a 2x window,
     * pulling every shot toward the top-left. */
    {
        SDL_Renderer *r = SDL_GetRenderer(w);
        int lw = 0, lh = 0;
        if (r)
            SDL_RenderGetLogicalSize(r, &lw, &lh);
        if (lw > 0 && lh > 0) {
            /* A 16:9 picture (display_settings.c) holds the game's 4:3
             * screen in its middle; the sides are view the game does not
             * aim into, so they read as off the screen. */
            double gw = lw * 3 > lh * 4 ? lh * 4.0 / 3.0 : (double)lw;
            double gx = mx - (lw - gw) / 2.0;
            if (gx < 0 || gx >= gw || my >= lh)
                return;
            *ux = gx / gw;
            *uy = (double)my / lh;
            return;
        }
    }
    SDL_GetWindowSize(w, &ww, &wh);
    if (ww <= 0 || wh <= 0)
        return;
    scale = (double)ww / 4.0 < (double)wh / 3.0 ? (double)ww / 4.0 : (double)wh / 3.0;
    pw = 4.0 * scale;
    ph = 3.0 * scale;
    ox = (ww - pw) / 2.0;
    oy = (wh - ph) / 2.0;
    if (mx < ox || my < oy || mx >= ox + pw || my >= oy + ph)
        return;
    *ux = (mx - ox) / pw;
    *uy = (my - oy) / ph;
}

static uint16_t to16(double u)
{
    return (uint16_t)(u <= 0.0 ? 0 : u >= 1.0 ? 0xFFFF : u * 65535.0);
}

/* The game turns the gun reading into gun coordinates -- about pixels from
 * the screen centre, y up: x = (byte - 128) * 2.5, y = (128 - byte) * 1.875
 * (measured on GUN ADJUSTMENT) -- and then through its calibration: five
 * readings taken at dots on a 640x480 screen (TOP, LEFT, CENTER, RIGHT,
 * BOTTOM at x 16/320/624, y 16/240/464). The default calibration is a real
 * gun's, not a mouse's, so the aim drifted from the cursor. So send the
 * reading the calibration maps to the cursor: interpolate between the dots'
 * stored readings, piecewise either side of centre. */
extern int chihiro_gun_calibration(int16_t cal[10]);

static double lerp_cal(double p, double c, double lo_px, double lo, double mid,
                       double hi_px, double hi)
{
    return p <= c ? lo + (p - lo_px) * (mid - lo) / (c - lo_px)
                  : mid + (p - c) * (hi - mid) / (hi_px - c);
}

static void gun_reading(double ux, double uy, uint16_t *ox, uint16_t *oy)
{
    /* the profile's aim block (VC3: dots at x 16/320/624, y 16/240/464 on
     * 640x480; 2.5 and 1.875 gun units a reading step) */
    const ChihiroProfile *gp = chihiro_profile();
    const float *dx = gp->aim.dots_x, *dy = gp->aim.dots_y;
    int16_t c[10];
    double gx, gy, bx, by;

    if (!chihiro_gun_calibration(c))
        memcpy(c, gp->aim.defaults, sizeof c);
    /* x: LEFT .. CENTER .. RIGHT; y: TOP .. BOTTOM */
    gx = lerp_cal(ux * gp->aim.screen_w, dx[1], dx[0], c[2], c[4], dx[2], c[6]);
    gy = lerp_cal(uy * gp->aim.screen_h, dy[1], dy[0], c[1], c[5], dy[2], c[9]);
    bx = 128.0 + gx / gp->aim.units_x;
    by = 128.0 - gy / gp->aim.units_y;
    bx = bx < 0.0 ? 0.0 : bx > 255.99 ? 255.99 : bx;
    by = by < 0.0 ? 0.0 : by > 255.99 ? 255.99 : by;
    *ox = (uint16_t)(bx * 256.0);
    *oy = (uint16_t)(by * 256.0);
}

/* Fill the board's input snapshot. Coins are edge-triggered: one per press. */
void host_input_poll(chihiro_input_t *in)
{
    static int coin_was[2];
    const Uint8 *k;
    Uint32 buttons;
    int mx = 0, my = 0, i;
    uint32_t t;
    double ux, uy;
    int fire, reload, coin[2];

    if (s_nauto < 0) {
        bind_load();
        auto_parse();
        s_t0 = now_ms();
        const char *clock = ch_env("AUTOKEYS_CLOCK");
        s_auto_flip_clock = clock && !strcmp(clock,"flips");
        s_flip0 = nv2a_backend_flip_count();
        if(s_auto_flip_clock)
            fprintf(stderr,"[INPUT] script clock: executor flips at 60 Hz (origin %llu)\n",(unsigned long long)s_flip0);
        SDL_AddEventWatch(input_watch, NULL);
#if VC3_HOST_GAMEPAD
        /* Pads already connected arrive as SDL_CONTROLLERDEVICEADDED.
         * Under gamescope the window need not hold the focus SDL looks
         * for, so pad events count without it. */
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
            fprintf(stderr, "[INPUT] no gamepads: %s\n", SDL_GetError());
        else {
            /* The toolkit may have started the subsystem before this
             * watch was there, its device-added events gone by: open what
             * is connected already. */
            int n = SDL_NumJoysticks(), j;
            fprintf(stderr, "[INPUT] gamepads: %d joystick(s) at start\n", n);
            for (j = 0; j < n; j++)
                if (SDL_IsGameController(j))
                    pad_added(j);
        }
#endif
        if (ch_env("INPUT_SELFTEST")) {
            static uint64_t start;
            start = s_t0;
            SDL_DetachThread(SDL_CreateThread(selftest_thread, "vc3-selftest", &start));
            fprintf(stderr, "[INPUT] selftest: SDL events into the queue from t=30s\n");
        }
    }
    t = s_auto_flip_clock ? (uint32_t)((nv2a_backend_flip_count()-s_flip0)*1000/60) : (uint32_t)(now_ms() - s_t0);

    memset(in, 0, sizeof(*in));
    k = (const Uint8 *)s_keys;
    buttons = s_buttons;
    mx = s_mx;
    my = s_my;
    mouse_in_picture(mx, my, &ux, &uy);

    fire = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) || auto_held("fire", t)
        || bound(k, VC3_KEY_FIRE);
    reload = (buttons & SDL_BUTTON(SDL_BUTTON_RIGHT)) || auto_held("reload", t)
        || bound(k, VC3_KEY_RELOAD);
#if VC3_HOST_GAMEPAD
    {
        static int drove;               /* the gun state below is the pad's */
        double px, py;
        int pb, on = pad_poll(&px, &py, &pb);
        if (on) {
            vc3_input_vr(px, py, pb);   /* the pad's gun, as a headset's */
            drove = 1;
        } else if (drove) {
            s_vr_on = 0;                /* only what the pad itself set */
            drove = 0;
        }
        if (fb_present_set_aim)
            fb_present_set_aim(on ? (float)px : -1.0f, on ? (float)py : -1.0f);
    }
#endif
    if (s_vr_on) {                      /* a headset's gun replaces the mouse */
        int vb = s_vr_buttons;
        ux = s_vr_ux;
        uy = s_vr_uy;
        fire = fire || (vb & VC3_VR_FIRE);
        reload = reload || (vb & VC3_VR_RELOAD);
    }

    /* A scripted shot with no window under the mouse aims at VC3_AUTOAIM
     * ("x,y" in 0..1, default the centre), so unattended runs can shoot; an
     * explicit VC3_AUTOAIM also holds the aim between shots;
     * VC3_AUTOAIM=sweep[:s] keeps it moving over the screen, as a player's
     * does, from s seconds on (the centre before: shots that hit fill the
     * E.S. gauge). */
    if (ux < 0.0 && (auto_held("fire", t) || ch_env("AUTOAIM"))) {
        const char *aim = ch_env("AUTOAIM");
        ux = 0.5;
        uy = 0.5;
        if (aim && !strncmp(aim, "sweep", 5)) {
            if (t >= (uint32_t)(atof(aim[5] == ':' ? aim + 6 : "0") * 1000.0)) {
                ux = 0.5 + 0.38 * sin(t / 1000.0 * 1.7);   /* t: ms */
                uy = 0.5 + 0.30 * sin(t / 1000.0 * 1.1 + 0.7);
            }
        } else if (aim) {
            sscanf(aim, "%lf,%lf", &ux, &uy);
        }
    }

    in->test = bound(k, VC3_KEY_TEST) || auto_held("test", t)
        || (s_vr_on && (s_vr_buttons & VC3_VR_TEST));
    if (bound(k, VC3_KEY_SERVICE) || auto_held("service", t)
        || (s_vr_on && (s_vr_buttons & VC3_VR_SERVICE)))
        in->player[0] |= JVS_SW_SERVICE;
    if (bound(k, VC3_KEY_START1) || auto_held("start", t)
        || (s_vr_on && (s_vr_buttons & VC3_VR_START)))
        in->player[0] |= JVS_SW_START;
    if (bound(k, VC3_KEY_START2))
        in->player[1] |= JVS_SW_START;
    /* A reload is the gun pointed away, then the trigger: the trigger only
     * goes down once the gun has been off the screen for 100 ms, as a player
     * points away before pulling. Pressed in the same frame, VC3 takes it
     * for a shot at the last on-screen position and keeps asking to reload. */
    {
        static uint64_t reload_since;
        uint64_t now = now_ms();
        if (reload) {
            if (!reload_since)
                reload_since = now;
            fire = now - reload_since >= 100;
        } else {
            reload_since = 0;
        }
    }
    /* The switches the profile names (VC3: trigger push 1; its reload is the
     * gun's "off screen" switch, push 2, not the position -- with only an
     * off-screen position the RELOAD prompt never cleared; the input test
     * names push 3 CHANGE and push 4 PEDAL). */
    if (fire)
        in->player[0] |= chihiro_profile()->jvs.trigger;
    if (reload)
        in->player[0] |= chihiro_profile()->jvs.offscreen;
    if ((buttons & SDL_BUTTON(SDL_BUTTON_MIDDLE)) || bound(k, VC3_KEY_CHANGE)
        || (s_vr_on && (s_vr_buttons & VC3_VR_CHANGE)))
        in->player[0] |= chihiro_profile()->jvs.change;
    if (bound(k, VC3_KEY_PEDAL) || auto_held("pedal", t)
        || (s_vr_on && (s_vr_buttons & VC3_VR_PEDAL)))
        in->player[0] |= chihiro_profile()->jvs.pedal;
    /* VC3_JVS_OR=<hex>: switch bits held on player 1, to see in the test
     * menu's input test which switch a bit is. */
    {
        const char *e = ch_env("JVS_OR");
        if (e)
            in->player[0] |= (uint16_t)strtoul(e, NULL, 16);
    }

    gun_reading(ux, uy, &in->analog[0], &in->analog[1]);
    /* VC3 polls the screen position on channel 2 every frame (25 02), and
     * channel 1 only at startup; channel 2 left at 0,0 put gameplay shots in
     * the top-left corner while Mission Select, which reads the analog
     * channels, aimed fine. Screen position is the plain position over the
     * 16-bit range (the calibrated analog reading here landed up-left). */
    in->screen_x[0] = in->screen_x[1] = ux < 0.0 ? 0 : to16(ux);
    in->screen_y[0] = in->screen_y[1] = uy < 0.0 ? 0 : to16(uy);
    /* One line per trigger pull: what the mouse reported, the window in
     * points and pixels, where that lands in the picture, and the gun
     * reading sent -- enough to tell a host mapping problem from the game's. */
    {
        static int fire_was;
        if (fire && !fire_was && !reload) {
            SDL_Window *w = s_mwin ? SDL_GetWindowFromID(s_mwin) : NULL;
            int ww = 0, wh = 0, pw = 0, ph = 0;
            if (w) {
                SDL_GetWindowSize(w, &ww, &wh);
                SDL_GetWindowSizeInPixels(w, &pw, &ph);
            }
            fprintf(stderr, "[AIM] mouse %d,%d window %dx%d (%dx%d px) -> %.3f,%.3f"
                    " -> gun %02X,%02X\n", mx, my, ww, wh, pw, ph, ux, uy,
                    in->analog[0] >> 8, in->analog[1] >> 8);
        }
        fire_was = fire;
    }
    /* Off the picture, or reloading: the gun sees no screen. */
    if (reload || ux < 0.0) {
        in->analog[0] = in->analog[1] = 0xFFFF;
        in->screen_x[0] = in->screen_y[0] = 0xFFFF;
    }

    coin[0] = bound(k, VC3_KEY_COIN1) || auto_held("coin", t)
        || (s_vr_on && (s_vr_buttons & VC3_VR_COIN));
    coin[1] = bound(k, VC3_KEY_COIN2);
    for (i = 0; i < 2; i++) {
        if (coin[i] && !coin_was[i])
            chihiro_coin_insert(i);
        coin_was[i] = coin[i];
    }
}
