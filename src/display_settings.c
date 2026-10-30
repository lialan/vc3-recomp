/*
 * Display settings: frame rate, internal resolution and aspect ratio.
 *
 * VC3's logic ticks once per 60 Hz vblank at 640x480, and none of this
 * changes that; it changes how the GL back end draws and what the window
 * shows (gl_backend.c).
 *
 *   fps     60 (default) or 120: at 120 one motion-interpolated frame goes
 *           between each pair of real ones. Any multiple of 60 up to 240
 *           works the same way, with fps / 60 - 1 in-between frames.
 *   scale   1 (default) to 4: internal resolution, 640x480 times scale.
 *   stereo  0 (default; 1 on the Quest): the 3D view in stereo, each eye
 *           its own picture with depth (gl_backend.c).
 *   aspect  4:3 (default) or 16:9: at 16:9 the 3D view extends to the
 *           sides (hor+) and the HUD moves to the screen edges; movies
 *           and menus stay 4:3 (gl_backend.c, surf_xform).
 *
 * Each comes from, first that applies: VC3_FPS / VC3_SCALE / VC3_ASPECT,
 * the "fps=" / "scale=" / "aspect=" line of
 * ~/Library/Application Support/VC3/settings (VC3_CONFIG_DIR moves the
 * folder), else the default. The Display menu (and F10, for the rate)
 * changes them while the game runs and saves them to that file, so the app
 * keeps the choice; an environment variable still wins at the next launch.
 *
 * Each is one int, written on the main thread (menu, key) and read once
 * per flip on the executor thread: an atomic word, nothing else shared.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "display_settings.h"

static int s_fps = 60, s_scale = 1, s_wide, s_stereo;

static int fps_ok(int fps)
{
    return fps >= 60 && fps <= VC3_FPS_MAX && fps % 60 == 0;
}

static int scale_ok(int scale)
{
    return scale >= 1 && scale <= VC3_SCALE_MAX;
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

/* The saved value of "<key>=" (key includes the '='), or NULL. */
const char *vc3_setting_get(const char *key)
{
    static char val[64];
    char path[1024], line[256];
    size_t k = strlen(key);
    const char *found = NULL;
    FILE *f;

    settings_path(path, sizeof path, 0);
    f = fopen(path, "r");
    if (!f)
        return NULL;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, k)) {
            snprintf(val, sizeof val, "%s", line + k);
            val[strcspn(val, "\r\n")] = 0;
            found = val;
        }
    fclose(f);
    return found;
}

/* Rewrite the file with "<key>=" set to value, keeping every other line. */
void vc3_setting_put(const char *key, const char *value)
{
    char path[1024], tmp[1100], line[256];
    size_t k = strlen(key);
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
            if (strncmp(line, key, k))
                fputs(line, out);
        fclose(in);
    }
    fprintf(out, "%s%s\n", key, value);
    if (fclose(out) == 0)
        rename(tmp, path);
}

/* The env value of name, else the saved one of key. */
static const char *setting(const char *name, const char *key, int *from_env)
{
    const char *e = getenv(name);
    *from_env = e != NULL;
    return e ? e : vc3_setting_get(key);
}

static int parse_wide(const char *v)
{
    return v && (!strcmp(v, "16:9") || !strcmp(v, "16x9") || !strcmp(v, "wide"));
}

void vc3_display_init(void)
{
    const char *v;
    int env, n;

    v = setting("VC3_FPS", "fps=", &env);
    n = v ? atoi(v) : 60;
#if defined(__ANDROID__)
    n = 60;            /* the headset makes its own in-between frames */
#endif
    if (!fps_ok(n)) {
        if (env)
            fprintf(stderr, "[VC3] VC3_FPS=%s: not 60..%d in steps of 60; using 60\n",
                    v, VC3_FPS_MAX);
        n = 60;
    }
    __atomic_store_n(&s_fps, n, __ATOMIC_RELAXED);

    v = setting("VC3_SCALE", "scale=", &env);
#if defined(__ANDROID__)
    n = v ? atoi(v) : 2;               /* the Quest: a sharper big screen */
#else
    n = v ? atoi(v) : 1;
#endif
    if (!scale_ok(n)) {
        if (env)
            fprintf(stderr, "[VC3] VC3_SCALE=%s: not 1..%d; using 1\n", v, VC3_SCALE_MAX);
        n = 1;
    }
    __atomic_store_n(&s_scale, n, __ATOMIC_RELAXED);

    v = setting("VC3_ASPECT", "aspect=", &env);
#if defined(__ANDROID__)
    if (!v)
        v = "16:9";                    /* the Quest: a wide screen in front */
#endif
    if (env && !parse_wide(v) && strcmp(v, "4:3"))
        fprintf(stderr, "[VC3] VC3_ASPECT=%s: not 4:3 or 16:9; using 4:3\n", v);
    __atomic_store_n(&s_wide, parse_wide(v), __ATOMIC_RELAXED);

    v = setting("VC3_STEREO", "stereo=", &env);
#if defined(__ANDROID__)
    n = v ? atoi(v) != 0 : 1;          /* the Quest: 3D */
#else
    n = v ? atoi(v) != 0 : 0;
#endif
    __atomic_store_n(&s_stereo, n, __ATOMIC_RELAXED);

    fprintf(stderr, "[VC3] display %d fps (game 60)%s, %dx%d internal, %s%s\n",
            vc3_fps_get(), vc3_fps_get() > 60 ? " motion-interpolated" : "",
            640 * vc3_scale_get(), 480 * vc3_scale_get(),
            vc3_wide_get() ? "16:9" : "4:3", vc3_stereo_get() ? ", stereo 3D" : "");
}

int vc3_fps_get(void)
{
    return __atomic_load_n(&s_fps, __ATOMIC_RELAXED);
}

void vc3_fps_set(int fps)
{
    char buf[16];

    if (!fps_ok(fps) || fps == vc3_fps_get())
        return;
    __atomic_store_n(&s_fps, fps, __ATOMIC_RELAXED);
    snprintf(buf, sizeof buf, "%d", fps);
    vc3_setting_put("fps=", buf);
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

int vc3_scale_get(void)
{
    return __atomic_load_n(&s_scale, __ATOMIC_RELAXED);
}

void vc3_scale_set(int scale)
{
    char buf[16];

    if (!scale_ok(scale) || scale == vc3_scale_get())
        return;
    __atomic_store_n(&s_scale, scale, __ATOMIC_RELAXED);
    snprintf(buf, sizeof buf, "%d", scale);
    vc3_setting_put("scale=", buf);
    fprintf(stderr, "[VC3] internal resolution %dx%d\n", 640 * scale, 480 * scale);
}

int vc3_stereo_get(void)
{
    return __atomic_load_n(&s_stereo, __ATOMIC_RELAXED);
}

void vc3_stereo_set(int on)
{
    on = on != 0;
    if (on == vc3_stereo_get())
        return;
    __atomic_store_n(&s_stereo, on, __ATOMIC_RELAXED);
    vc3_setting_put("stereo=", on ? "1" : "0");
    fprintf(stderr, "[VC3] stereo 3D %s\n", on ? "on" : "off");
}

int vc3_wide_get(void)
{
    return __atomic_load_n(&s_wide, __ATOMIC_RELAXED);
}

void vc3_wide_set(int wide)
{
    wide = wide != 0;
    if (wide == vc3_wide_get())
        return;
    __atomic_store_n(&s_wide, wide, __ATOMIC_RELAXED);
    vc3_setting_put("aspect=", wide ? "16:9" : "4:3");
    fprintf(stderr, "[VC3] aspect %s\n", wide ? "16:9" : "4:3");
}
