/*
 * ch_env.h - the runtime's environment switches, under any game's name.
 *
 * ch_env("FPS") reads CHIHIRO_FPS, else <prefix>_FPS, where <prefix> is the
 * game's own (VC3 for Virtua Cop 3: VC3_FPS), so the names a game has always
 * used keep working beside the common ones. Not cached: a game's start-up
 * sets some of them (a relaunch's input script, the Quest's save folder).
 */
#ifndef CH_ENV_H
#define CH_ENV_H

/* The value of CHIHIRO_<name>, else of <prefix>_<name>; NULL if neither. */
const char *ch_env(const char *name);
/* setenv(<prefix>_<name>, value, overwrite) / unsetenv: the game's name. */
void ch_setenv(const char *name, const char *value, int overwrite);
void ch_unsetenv(const char *name);
/* The game's prefix (without the underscore); set before the first ch_env. */
void ch_env_set_prefix(const char *prefix);
const char *ch_env_prefix(void);

#endif
