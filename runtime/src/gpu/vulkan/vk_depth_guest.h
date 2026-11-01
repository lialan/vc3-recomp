#ifndef CH_VK_DEPTH_GUEST_H
#define CH_VK_DEPTH_GUEST_H
#include "vk_nv2a.h"
/* Linear Z24S8 only. Other formats/layouts fail explicitly. */
VkResult ch_vk_guest_depth_load(const Nv2aSurface *,const Nv2aRenderState *,
    const void *guest,size_t bytes,float *depth,uint8_t *stencil);
VkResult ch_vk_guest_depth_store(const Nv2aSurface *,const Nv2aRenderState *,
    void *guest,size_t bytes,const float *depth,const uint8_t *stencil);
#endif
