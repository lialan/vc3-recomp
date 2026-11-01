#include "vk_depth_guest.h"
#include <math.h>
static VkResult validate(const Nv2aSurface *s,const Nv2aRenderState *rs,const void *g,size_t bytes) {
    if(!s || !rs || !g || !s->width || !s->height || !rs->zeta_va) return VK_ERROR_INITIALIZATION_FAILED;
    if(((rs->surface_format>>4)&15)!=2 || ((rs->surface_format>>8)&15)!=1) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    uint64_t row=(uint64_t)s->width*4,span=(uint64_t)(s->height-1)*rs->zeta_pitch+row;
    if(row>rs->zeta_pitch || rs->zeta_va>bytes || span>bytes-rs->zeta_va) return VK_ERROR_INITIALIZATION_FAILED;
    return VK_SUCCESS;
}
VkResult ch_vk_guest_depth_load(const Nv2aSurface *s,const Nv2aRenderState *rs,
    const void *guest,size_t bytes,float *depth,uint8_t *stencil) {
    if(!depth || !stencil) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=validate(s,rs,guest,bytes); if(r!=VK_SUCCESS) return r;
    const volatile uint8_t *base=(const volatile uint8_t *)guest+rs->zeta_va;
    for(uint32_t y=0;y<s->height;y++) for(uint32_t x=0;x<s->width;x++) {
        size_t at=(size_t)y*rs->zeta_pitch+x*4,i=(size_t)y*s->width+x;
        uint32_t word=0; for(unsigned b=0;b<4;b++) word|=(uint32_t)base[at+b]<<(8*b);
        depth[i]=(float)((double)(word>>8)/16777215.0); stencil[i]=word&255;
    }
    return VK_SUCCESS;
}
VkResult ch_vk_guest_depth_store(const Nv2aSurface *s,const Nv2aRenderState *rs,
    void *guest,size_t bytes,const float *depth,const uint8_t *stencil) {
    if(!depth || !stencil) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=validate(s,rs,guest,bytes); if(r!=VK_SUCCESS) return r;
    for(size_t i=0;i<(size_t)s->width*s->height;i++) if(!isfinite(depth[i]) || depth[i]<0 || depth[i]>1) return VK_ERROR_INITIALIZATION_FAILED;
    volatile uint8_t *base=(volatile uint8_t *)guest+rs->zeta_va;
    for(uint32_t y=0;y<s->height;y++) for(uint32_t x=0;x<s->width;x++) {
        size_t at=(size_t)y*rs->zeta_pitch+x*4,i=(size_t)y*s->width+x;
        uint32_t z=(uint32_t)((double)depth[i]*16777215.0+0.5);
        uint32_t word=(z<<8)|stencil[i];
        for(unsigned b=0;b<4;b++) base[at+b]=(uint8_t)(word>>(8*b));
    }
    return VK_SUCCESS;
}
