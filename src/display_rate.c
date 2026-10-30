/*
 * The display frame rate: how many frames are shown per game frame.
 *
 * VC3's logic ticks once per 60 Hz vblank, and that stays as it is. What can
 * change is presentation: at 120 fps the GL back end makes one
 * motion-interpolated frame between each pair of real ones (gl_backend.c),
 * and the window shows both, spread over the flip interval. Any multiple of
 * 60 up to 240 works the same way, with fps / 60 - 1 in-between frames.
 *
 * Set by, first that applies: VC3_FPS=60|120|..., the "fps=" line of
 * ~/Library/Application Support/VC3/settings (VC3_CONFIG_DIR moves the
 * folder), else 60. The Display menu and F10 change it while the game runs
 * and save it to that file, so the app keeps the choice; VC3_FPS still wins
 * at the next launch.
 *
 * The rate is one int, written on the main thread (menu, key) and read once
 * per flip on the executor thread: an atomic word, nothing else shared.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "display_rate.h"

static int s_fps = 60;

static int valid(int fps)
{
    return fps >= 60 && fps <= VC3_FPS_MAX && fps % 60 == 0;
}

static void settings_path(char *buf, size_t n, int dir_only)
{
    const char *dir = getenv("VC3_CONFIG_DIR");
    if (dir && *dir)
        snprintf(buf, n, "%s%s", dir, dir_only ? "" : "/settings");
    else
        snprintf(buf, n, "%s/Library/Application Support/VC3%s",
                 getenv("HOME") ? getenv("HOME") : ".",
                 dir_only ? "" : "/settings");
}

/* The saved value of "fps=", or 0. */
static int settings_fps(void)
{
    char path[1024], line[256];
    FILE *f;
    int fps = 0;

    settings_path(path, sizeof path, 0);
    f = fopen(path, "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "fps=", 4))
            fps = atoi(line + 4);
    fclose(f);
    return fps;
}

/* Rewrite the file with "fps=" replaced, keeping every other line. */
static void settings_save(int fps)
{
    char path[1024], tmp[1100], line[256];
    FILE *in, *out;

    settings_path(path, sizeof path, 1);
    mkdir(path, 0755);
    settings_path(path, sizeof path, 0);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    out = fopen(tmp, "w");
    if (!out)
        return;
    in = fopen(path, "r");
    if (in) {
        while (fgets(line, sizeof line, in))
            if (strncmp(line, "fps=", 4))
                fputs(line, out);
        fclose(in);
    }
    fprintf(out, "fps=%d\n", fps);
    if (fclose(out) == 0)
        rename(tmp, path);
}

void vc3_fps_init(void)
{
    const char *e = getenv("VC3_FPS");
    int fps = e ? atoi(e) : settings_fps();

    if (e && !valid(fps))
        fprintf(stderr, "[VC3] VC3_FPS=%s: not 60..%d in steps of 60; using 60\n",
                e, VC3_FPS_MAX);
    if (!valid(fps))
        fps = 60;
    __atomic_store_n(&s_fps, fps, __ATOMIC_RELAXED);
    fprintf(stderr, "[VC3] display %d fps (game 60)%s\n", fps,
            fps > 60 ? ", motion-interpolated" : "");
}

int vc3_fps_get(void)
{
    return __atomic_load_n(&s_fps, __ATOMIC_RELAXED);
}

void vc3_fps_set(int fps)
{
    if (!valid(fps) || fps == vc3_fps_get())
        return;
    __atomic_store_n(&s_fps, fps, __ATOMIC_RELAXED);
    settings_save(fps);
    fprintf(stderr, "[VC3] display %d fps\n", fps);
}

/* F10 reaches us twice when the menu holds it as a key equivalent and SDL
 * also sees the key: AppKit runs the menu item, SDL queues the keydown. A
 * second toggle within a quarter second is the same press. */
void vc3_fps_toggle(void)
{
    static double last;
    struct timespec ts;
    double now;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (double)ts.tv_sec + ts.tv_nsec / 1e9;
    if (now - last < 0.25)
        return;
    last = now;
    vc3_fps_set(vc3_fps_get() == 60 ? 120 : 60);
}
