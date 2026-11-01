#ifndef CH_NV2A_TEXTURE_MIPS_H
#define CH_NV2A_TEXTURE_MIPS_H
#include "nv2a_backend.h"
/* Reference policy: derive a full chain from decoded level zero. Stored
 * guest levels select the policy, not the uploaded lower-level contents.
 * mode: 0 off, 1 guest mip filters, 2 also eligible linear textures. */
static inline uint32_t ch_nv2a_texture_mip_levels(const Nv2aTexture *t,int mode) {
    if(!mode || t->cube || t->width<4 || t->height<4) return 1;
    uint32_t min=(t->filter>>16)&255;
    int wanted=(min>=3 && min<=6 && t->levels>1) ||
        (mode==2 && min!=1 && (t->width>8 || t->height>8) &&
         !(t->width&(t->width-1)) && !(t->height&(t->height-1)));
    if(!wanted) return 1;
    uint32_t size=t->width>t->height?t->width:t->height,levels=1;
    while(size>1) {size>>=1;levels++;}
    return levels;
}
#endif
