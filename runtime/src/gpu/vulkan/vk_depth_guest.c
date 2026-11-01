#include "vk_depth_guest.h"
#include "vk_depth_quant_math.h"
#include "vk_guest_layout.h"
#include <math.h>
#include <string.h>
static VkResult validate(const Nv2aSurface *s,const Nv2aRenderState *rs,const void *g,size_t bytes,
    ChVkGuestLayout *layout) {
    if(!s || !rs || !g || !s->width || !s->height || !rs->zeta_va) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t format=(rs->surface_format>>4)&15;
    if(format!=1 && format!=2) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkResult r=ch_vk_guest_layout(rs->surface_format,s->width,s->height,rs->zeta_pitch,format==1?2:4,layout);
    if(r!=VK_SUCCESS) return r;
    if(rs->zeta_va>bytes || layout->span>bytes-rs->zeta_va) return VK_ERROR_INITIALIZATION_FAILED;
    return VK_SUCCESS;
}
VkResult ch_vk_guest_depth_load(const Nv2aSurface *s,const Nv2aRenderState *rs,
    const void *guest,size_t bytes,float *depth,uint8_t *stencil) {
    if(!depth || !stencil) return VK_ERROR_INITIALIZATION_FAILED;
    ChVkGuestLayout layout;
    VkResult r=validate(s,rs,guest,bytes,&layout); if(r!=VK_SUCCESS) return r;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if(!layout.swizzled) {
        const uint8_t *base=(const uint8_t *)guest+rs->zeta_va;
        for(uint32_t y=0;y<s->height;y++) {
            const uint8_t *row=base+(size_t)y*layout.pitch;
            size_t start=(size_t)y*s->width;
            if(layout.bytes_per_pixel==4) {
                for(uint32_t x=0;x<s->width;x++) {
                    uint32_t word;memcpy(&word,row+(size_t)x*4,4);
                    depth[start+x]=ch_vk_depth_unpack(word>>8);
                    stencil[start+x]=(uint8_t)word;
                }
            } else {
                for(uint32_t x=0;x<s->width;x++) {
                    uint16_t word;memcpy(&word,row+(size_t)x*2,2);
                    depth[start+x]=(float)((double)word/65535.0);
                    stencil[start+x]=0;
                }
            }
        }
        return VK_SUCCESS;
    }
#endif
    const volatile uint8_t *base=(const volatile uint8_t *)guest+rs->zeta_va;
    for(uint32_t y=0;y<s->height;y++) for(uint32_t x=0;x<s->width;x++) {
        size_t at=(size_t)ch_vk_guest_offset(&layout,x,y),i=(size_t)y*s->width+x;
        uint32_t word=0; for(unsigned b=0;b<layout.bytes_per_pixel;b++) word|=(uint32_t)base[at+b]<<(8*b);
        depth[i]=layout.bytes_per_pixel==2?(float)((double)word/65535.0):ch_vk_depth_unpack(word>>8);
        stencil[i]=layout.bytes_per_pixel==2?0:word&255;
    }
    return VK_SUCCESS;
}
VkResult ch_vk_guest_depth_store(const Nv2aSurface *s,const Nv2aRenderState *rs,
    void *guest,size_t bytes,const float *depth,const uint8_t *stencil) {
    if(!depth || !stencil) return VK_ERROR_INITIALIZATION_FAILED;
    ChVkGuestLayout layout;
    VkResult r=validate(s,rs,guest,bytes,&layout); if(r!=VK_SUCCESS) return r;
    /* Validate the whole input before touching guest memory. Reducing the
     * invalid flag lets compilers vectorize the normal finite [0,1] case. */
    unsigned invalid=0;
    for(size_t i=0;i<(size_t)s->width*s->height;i++)
        invalid|=!isfinite(depth[i]) || depth[i]<0 || depth[i]>1;
    if(invalid) return VK_ERROR_INITIALIZATION_FAILED;
    /* Linear guest surfaces are ordinary RAM. Copy unaligned words safely,
     * retaining the scalar double-precision rounding contract. Specializing
     * the format outside the loop permits vectorization and avoids repeated
     * layout calculation and volatile byte stores for every pixel. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if(!layout.swizzled) {
        uint8_t *base=(uint8_t *)guest+rs->zeta_va;
        for(uint32_t y=0;y<s->height;y++) {
            uint8_t *row=base+(size_t)y*layout.pitch;
            size_t start=(size_t)y*s->width;
            if(layout.bytes_per_pixel==4) {
                for(uint32_t x=0;x<s->width;x++) {
                    uint32_t z=(uint32_t)((double)depth[start+x]*16777215.0+0.5);
                    uint32_t word=(z<<8)|stencil[start+x];
                    memcpy(row+(size_t)x*4,&word,4);
                }
            } else {
                for(uint32_t x=0;x<s->width;x++) {
                    uint16_t z=(uint16_t)((double)depth[start+x]*65535.0+0.5);
                    memcpy(row+(size_t)x*2,&z,2);
                }
            }
        }
        return VK_SUCCESS;
    }
#endif
    volatile uint8_t *base=(volatile uint8_t *)guest+rs->zeta_va;
    for(uint32_t y=0;y<s->height;y++) for(uint32_t x=0;x<s->width;x++) {
        size_t at=(size_t)ch_vk_guest_offset(&layout,x,y),i=(size_t)y*s->width+x;
        uint32_t z=(uint32_t)((double)depth[i]*(layout.bytes_per_pixel==2?65535.0:16777215.0)+0.5);
        uint32_t word=layout.bytes_per_pixel==2?z:(z<<8)|stencil[i];
        for(unsigned b=0;b<layout.bytes_per_pixel;b++) base[at+b]=(uint8_t)(word>>(8*b));
    }
    return VK_SUCCESS;
}
static VkResult transfer_z24s8(const Nv2aSurface *s,const Nv2aRenderState *rs,
    void *guest,size_t bytes,void *words,int store) {
    if(!words) return VK_ERROR_INITIALIZATION_FAILED;
    ChVkGuestLayout layout;
    VkResult r=validate(s,rs,guest,bytes,&layout);if(r!=VK_SUCCESS) return r;
    if(layout.bytes_per_pixel!=4) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    uint8_t *base=(uint8_t *)guest+rs->zeta_va;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if(!layout.swizzled) {
        for(uint32_t y=0;y<s->height;y++) {
            void *row=(uint32_t *)words+(size_t)y*s->width;
            if(store) memcpy(base+(size_t)y*layout.pitch,row,(size_t)s->width*4);
            else memcpy(row,base+(size_t)y*layout.pitch,(size_t)s->width*4);
        }
        return VK_SUCCESS;
    }
#endif
    for(uint32_t y=0;y<s->height;y++) for(uint32_t x=0;x<s->width;x++) {
        size_t at=(size_t)ch_vk_guest_offset(&layout,x,y),i=(size_t)y*s->width+x;
        if(store) {
            uint32_t word=((const uint32_t *)words)[i];
            for(unsigned b=0;b<4;b++) base[at+b]=(uint8_t)(word>>(8*b));
        } else {
            uint32_t word=0;for(unsigned b=0;b<4;b++) word|=(uint32_t)base[at+b]<<(8*b);
            ((uint32_t *)words)[i]=word;
        }
    }
    return VK_SUCCESS;
}
VkResult ch_vk_guest_z24s8_load(const Nv2aSurface *s,const Nv2aRenderState *rs,const void *guest,size_t bytes,uint32_t *words) {
    return transfer_z24s8(s,rs,(void *)guest,bytes,words,0);
}
VkResult ch_vk_guest_z24s8_store(const Nv2aSurface *s,const Nv2aRenderState *rs,void *guest,size_t bytes,const uint32_t *words) {
    return transfer_z24s8(s,rs,guest,bytes,(void *)words,1);
}
