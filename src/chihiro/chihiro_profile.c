/* chihiro_profile.c - the game's profile in force (chihiro_profile.h). */
#include "chihiro_profile.h"
#include "ch_env.h"

#include <stddef.h>

static const ChihiroProfile *s_profile;

void chihiro_profile_apply(const ChihiroProfile *p)
{
    if (!p || s_profile == p)
        return;
    s_profile = p;
    ch_env_set_prefix(p->env_prefix);
}

const ChihiroProfile *chihiro_profile(void)
{
    if (!s_profile)
        chihiro_profile_apply(chihiro_game());
    return s_profile;
}
