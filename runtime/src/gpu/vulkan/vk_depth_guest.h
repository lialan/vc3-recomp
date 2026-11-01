#ifndef CH_VK_DEPTH_GUEST_H
#define CH_VK_DEPTH_GUEST_H
#include "vk_nv2a.h"
/* Pitched or Morton-layout Z16 and Z24S8. Other formats fail explicitly.
 * Z16 loads a zero stencil plane and never stores stencil bytes. */
VkResult ch_vk_guest_depth_load(const Nv2aSurface *,const Nv2aRenderState *,
    const void *guest,size_t bytes,float *depth,uint8_t *stencil);
VkResult ch_vk_guest_depth_store(const Nv2aSurface *,const Nv2aRenderState *,
    void *guest,size_t bytes,const float *depth,const uint8_t *stencil);
#endif
