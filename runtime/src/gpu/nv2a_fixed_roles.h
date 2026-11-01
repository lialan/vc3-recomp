#ifndef CH_NV2A_FIXED_ROLES_H
#define CH_NV2A_FIXED_ROLES_H
#include "nv2a_backend.h"
/* The reference renderer's role mapping, shared with native raw uploads. */
static inline void ch_nv2a_fixed_roles(const Nv2aFixedState *f,int slot[8]) {
    slot[0]=0;slot[1]=f->skin_mode?1:-1;slot[2]=f->has_normal?2:-1;
    slot[3]=f->has_color?(int)f->color_slot:-1;slot[4]=(int)f->tex0_slot;
    slot[5]=10;slot[6]=11;slot[7]=12;
}
#endif
