/*
 * Host controls for the emulated JVS I/O board.
 *
 * The toolkit's window (xbox_HostWindowMain) pumps SDL events on the main
 * thread; this only reads the resulting mouse and keyboard state, on the
 * guest thread, when the game polls the board.
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

/* Mouse position in the game picture, 0..1 each way, or -1 off the picture.
 * The renderer letterboxes the 4:3 frame inside the window. */
static void mouse_in_picture(int mx, int my, double *ux, double *uy)
{
    SDL_Window *w = SDL_GetMouseFocus();
    int ww = 0, wh = 0;
    double scale, pw, ph, ox, oy;

    *ux = *uy = -1.0;
    if (!w)
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
    }
    t = (uint32_t)(now_ms() - s_t0);

    memset(in, 0, sizeof(*in));
    k = SDL_GetKeyboardState(NULL);
    buttons = SDL_GetMouseState(&mx, &my);
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
    if (fire || reload)
        in->player[0] |= JVS_SW_PUSH1;

    /* Off the picture, or reloading: the gun sees no screen. */
    if (reload || ux < 0.0) {
        ux = 0.0;
        uy = 0.0;
    }
    in->analog[0] = to16(ux);
    in->analog[1] = to16(uy);
    in->screen_x[0] = to16(ux);
    in->screen_y[0] = to16(uy);

    coin[0] = (k && k[SDL_SCANCODE_5]) || auto_held("coin", t);
    coin[1] = k && k[SDL_SCANCODE_6];
    for (i = 0; i < 2; i++) {
        if (coin[i] && !coin_was[i])
            chihiro_coin_insert(i);
        coin_was[i] = coin[i];
    }
}
