/*
 * Start Virtua Cop 3 on Meta Quest: SDL's Java side calls SDL_main.
 *
 * The game's files are not in the APK: the player copies their vc3.xbe and
 * media/ to the headset (quest/install.sh), and they are looked for in
 *
 *   /sdcard/Android/data/<package>/files/   (the app's own folder)
 *   /sdcard/VirtuaCop3/                     (needs all-files access, which
 *                                            the app asks for)
 *
 * Saves and the settings file live in the app's private storage. Everything
 * the game prints goes to logcat (tag "vc3") and to vc3.log beside the game.
 * env.txt in the game folder, one NAME=value per line, sets options
 * (VC3_SOUND=0, VC3_FPS_LOG=1, VC3_SCALE=3, VC3_NO_XR=1, ...) without a
 * rebuild.
 */
#if defined(__ANDROID__)

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <android/log.h>
#include <SDL.h>
#include "chihiro/ch_env.h"
#include "chihiro/chihiro_profile.h"

int chihiro_main(int argc, char **argv);   /* main.c's main, renamed for Android */

static FILE *s_log;

static void *log_pump(void *arg)
{
    FILE *in = fdopen((int)(intptr_t)arg, "r");
    char line[2048];

    while (in && fgets(line, sizeof line, in)) {
        __android_log_write(ANDROID_LOG_INFO, chihiro_profile()->id, line);   /* the tag: "vc3" */
        if (s_log) {
            fputs(line, s_log);
            fflush(s_log);
        }
    }
    return NULL;
}

static void log_to_logcat(void)
{
    int fd[2];
    pthread_t t;

    if (pipe(fd))
        return;
    dup2(fd[1], 2);
    dup2(fd[1], 1);
    pthread_create(&t, NULL, log_pump, (void *)(intptr_t)fd[0]);
    pthread_detach(t);
}

static void load_env(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];

    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *eq;
        line[strcspn(line, "\r\n")] = 0;
        eq = strchr(line, '=');
        if (!eq || line[0] == '#')
            continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
        fprintf(stderr, "[android] %s=%s\n", line, eq + 1);
    }
    fclose(f);
}

static int has_game(const char *dir)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, chihiro_profile()->xbe);
    return access(p, R_OK) == 0;
}

int SDL_main(int argc, char **argv)
{
    char game[1024] = "", path[1100];
    const char *own = SDL_AndroidGetExternalStoragePath();
    const char *priv = SDL_AndroidGetInternalStoragePath();
    char *args[3], base[1024];
    const ChihiroProfile *gp;
    /* An app holding several of a disc's images (Silent Scope Complete: the
     * menu and its three games, each its own library) says so: each image
     * then keeps its settings, saves and pipeline cache in a folder named
     * after it, as when each was its own app. */
    const int bundled = argc > 1 && !strcmp(argv[argc - 1], "--bundled");

    chihiro_profile_apply(chihiro_game());
    gp = chihiro_profile();
    log_to_logcat();
    if (own && has_game(own))
        snprintf(game, sizeof game, "%s", own);
    else if (has_game(gp->vr.sdcard_dir))
        snprintf(game, sizeof game, "%s", gp->vr.sdcard_dir);
    if (!game[0]) {
        fprintf(stderr, "[android] no %s in %s or %s\n", gp->xbe,
                own ? own : "(no app folder)", gp->vr.sdcard_dir);
        return 1;
    }
    snprintf(path, sizeof path, "%s/%s.log", game, gp->id);
    s_log = fopen(path, "w");
    snprintf(path, sizeof path, "%s/env.txt", game);
    load_env(path);
    if (own && strcmp(own, game)) {
        snprintf(path, sizeof path, "%s/env.txt", own);
        load_env(path);
    }
    if (priv) {
        /* Per-app diagnostic overrides keep candidates isolated from the
         * working app's shared env.txt and saved settings. */
        snprintf(path, sizeof path, "%s/env.txt", priv);
        load_env(path);
        if (bundled)
            snprintf(base, sizeof base, "%s/%s", priv, gp->id);
        else
            snprintf(base, sizeof base, "%s", priv);
        snprintf(path, sizeof path, "%s/save", base);
        ch_setenv("SAVE_DIR", path, 0);
        ch_setenv("CONFIG_DIR", base, 0);
    }
    /* The app plays its sound (main.c mutes unless VC3_SOUND=1). */
    ch_setenv("SOUND", "1", 0);
    /* Started by another image's XLaunchNewImage (main.c's
     * chihiro_launch_image left its launch data page here and asked for this
     * app): the toolkit loads it at boot and removes it. */
    snprintf(path, sizeof path, "%s/launch.bin", game);
    if (access(path, R_OK) == 0)
        setenv("RECOMP_LAUNCH_PAGE", path, 1);
    chdir(game);
    fprintf(stderr, "[android] game %s, saves %s\n", game, ch_env("SAVE_DIR"));

    args[0] = "vc3";
    args[1] = game;
    args[2] = NULL;
    return chihiro_main(2, args);
}

#endif /* __ANDROID__ */
