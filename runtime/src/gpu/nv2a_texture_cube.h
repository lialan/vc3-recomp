#ifndef CH_NV2A_TEXTURE_CUBE_H
#define CH_NV2A_TEXTURE_CUBE_H
#include <stddef.h>
#include "nv2a_backend.h"
#include "../d3d/d3d8_swizzle.h"
/* NV2A faces are +X,-X,+Y,-Y,+Z,-Z. Each stored mip chain is rounded
 * to 128 bytes; reference sampling uploads only level zero of each face. */
static inline size_t ch_nv2a_cube_face_bytes(const Nv2aTexture *t) {
    if(!t || !t->width || t->width!=t->height || t->width>2048 || t->levels>32) return 0;
    uint32_t w=t->width,h=t->height,levels=t->levels?t->levels:1;
    size_t bytes=0;
    for(uint32_t l=0;l<levels;l++) {
        uint32_t block=d3d8_format_dxt_block_bytes(t->color),bpp=4;
        if(block) bytes+=(size_t)((w+3)/4)*((h+3)/4)*block;
        else {
            switch(t->color) {
            case 0x00:case 0x01:case 0x0b:case 0x13:case 0x19:case 0x1f:bpp=1;break;
            case 0x02:case 0x03:case 0x04:case 0x05:case 0x10:case 0x11:
            case 0x1a:case 0x1c:case 0x1d:case 0x24:case 0x25:bpp=2;break;
            }
            bytes+=(size_t)w*h*bpp;
        }
        w=w>1?w/2:1;h=h>1?h/2:1;
    }
    return (bytes+127)&~(size_t)127;
}
#endif
