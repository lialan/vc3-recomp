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
 *   5 / 6            coin, slot 1 / 2
 *   1 / 2            start, player 1 / 2
 *   F2               test       F1   service
 *
 * VC3_AUTOKEYS scripts presses for unattended runs: "t:key,t:key", with t in
 * seconds since the first poll and key one of test, service, coin, start,
 * fire, reload (each held for half a second), e.g.
 * VC3_AUTOKEYS="3:test,8:coin,9:start".
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#include "chihiro/chihiro.h"

#define AUTO_MAX     32
#define AUTO_HOLD_MS 500   /* > one poll at the current frame rate */

typedef struct { uint32_t at_ms; char key[12]; } auto_t;

static auto_t s_auto[AUTO_MAX];
static int s_nauto = -1;
static uint64_t s_t0;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void auto_parse(void)
{
    const char *s = getenv("VC3_AUTOKEYS");
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

/* Is the scripted key held right now? */
static int auto_held(const char *key, uint32_t t)
{
    static uint32_t logged;          /* one bit per script entry */
    int i;
    for (i = 0; i < s_nauto; i++)
        if (!strcmp(s_auto[i].key, key) && t >= s_auto[i].at_ms
                && t < s_auto[i].at_ms + AUTO_HOLD_MS) {
            if (!(logged & (1u << i))) {
                logged |= 1u << i;
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
        if (e->key.keysym.scancode < SDL_NUM_SCANCODES)
            s_keys[e->key.keysym.scancode] = e->type == SDL_KEYDOWN;
        break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_LEAVE)
            s_mwin = 0;
        break;
    default:
        break;
    }
    return 0;
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
        auto_parse();
        s_t0 = now_ms();
        SDL_AddEventWatch(input_watch, NULL);
        if (getenv("VC3_INPUT_SELFTEST")) {
            static uint64_t start;
            start = s_t0;
            SDL_DetachThread(SDL_CreateThread(selftest_thread, "vc3-selftest", &start));
            fprintf(stderr, "[INPUT] selftest: SDL events into the queue from t=30s\n");
        }
    }
    t = (uint32_t)(now_ms() - s_t0);

    memset(in, 0, sizeof(*in));
    k = (const Uint8 *)s_keys;
    buttons = s_buttons;
    mx = s_mx;
    my = s_my;
    mouse_in_picture(mx, my, &ux, &uy);

    fire = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) || auto_held("fire", t);
    reload = (buttons & SDL_BUTTON(SDL_BUTTON_RIGHT)) || auto_held("reload", t);

    /* A scripted shot with no window under the mouse aims at VC3_AUTOAIM
     * ("x,y" in 0..1, default the centre), so unattended runs can shoot. */
    if (ux < 0.0 && auto_held("fire", t)) {
        const char *aim = getenv("VC3_AUTOAIM");
        ux = 0.5;
        uy = 0.5;
        if (aim)
            sscanf(aim, "%lf,%lf", &ux, &uy);
    }

    in->test = (k && k[SDL_SCANCODE_F2]) || auto_held("test", t);
    if ((k && k[SDL_SCANCODE_F1]) || auto_held("service", t))
        in->player[0] |= JVS_SW_SERVICE;
    if ((k && k[SDL_SCANCODE_1]) || auto_held("start", t))
        in->player[0] |= JVS_SW_START;
    if (k && k[SDL_SCANCODE_2])
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
    if (fire)
        in->player[0] |= JVS_SW_PUSH1;
    /* VC3 reloads on the gun's "off screen" switch (button 2), not on the
     * position: with only an off-screen position the RELOAD prompt never
     * cleared. */
    if (reload)
        in->player[0] |= JVS_SW_PUSH2;

    in->analog[0] = to16(ux);
    in->analog[1] = to16(uy);
    in->screen_x[0] = to16(ux);
    in->screen_y[0] = to16(uy);
    /* Off the picture, or reloading: the gun sees no screen. */
    if (reload || ux < 0.0) {
        in->analog[0] = in->analog[1] = 0xFFFF;
        in->screen_x[0] = in->screen_y[0] = 0xFFFF;
    }

    coin[0] = (k && k[SDL_SCANCODE_5]) || auto_held("coin", t);
    coin[1] = k && k[SDL_SCANCODE_6];
    for (i = 0; i < 2; i++) {
        if (coin[i] && !coin_was[i])
            chihiro_coin_insert(i);
        coin_was[i] = coin[i];
    }
}
