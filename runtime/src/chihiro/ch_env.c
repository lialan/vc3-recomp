/* ch_env.c - CHIHIRO_<name>, else <prefix>_<name> (ch_env.h). */
#include "ch_env.h"
#include "chihiro_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CH_ENV_PREFIX
#define CH_ENV_PREFIX "CHIHIRO"
#endif

static const char *s_prefix = CH_ENV_PREFIX;
static int s_set;

/* The game's prefix comes with its profile: the first lookup applies it,
 * whenever that is (the Quest reads its env.txt before main). */
static void ensure_prefix(void)
{
    if (!s_set)
        (void)chihiro_profile();
}

void ch_env_set_prefix(const char *prefix)
{
    s_set = 1;
    if (prefix && *prefix)
        s_prefix = prefix;
}

const char *ch_env_prefix(void)
{
    ensure_prefix();
    return s_prefix;
}

static const char *named(char *buf, size_t n, const char *prefix, const char *name)
{
    snprintf(buf, n, "%s_%s", prefix, name);
    return buf;
}

const char *ch_env(const char *name)
{
    char buf[128];
    const char *v;

    ensure_prefix();
    v = getenv(named(buf, sizeof buf, "CHIHIRO", name));
    if (v || !strcmp(s_prefix, "CHIHIRO"))
        return v;
    return getenv(named(buf, sizeof buf, s_prefix, name));
}

void ch_setenv(const char *name, const char *value, int overwrite)
{
    char buf[128];
    ensure_prefix();
    setenv(named(buf, sizeof buf, s_prefix, name), value, overwrite);
}

void ch_unsetenv(const char *name)
{
    char buf[128];
    ensure_prefix();
    unsetenv(named(buf, sizeof buf, s_prefix, name));
}
