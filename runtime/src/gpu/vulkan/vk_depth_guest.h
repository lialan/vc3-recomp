#ifndef CH_VK_DEPTH_GUEST_H
#define CH_VK_DEPTH_GUEST_H
#include "vk_nv2a.h"
/* Pitched or Morton-layout Z16 and Z24S8. Other formats fail explicitly.
 * Z16 loads a zero stencil plane and never stores stencil bytes. */
VkResult ch_vk_guest_depth_load(const Nv2aSurface *,const Nv2aRenderState *,
    const void *guest,size_t bytes,float *depth,uint8_t *stencil);
VkResult ch_vk_guest_depth_store(const Nv2aSurface *,const Nv2aRenderState *,
    void *guest,size_t bytes,const float *depth,const uint8_t *stencil);
/* Exact Z24S8 words, row-major depth31..8/stencil7..0. Guest access must be
 * serialized and the word buffer must not alias guest storage. Padding kept. */
VkResult ch_vk_guest_z24s8_load(const Nv2aSurface *,const Nv2aRenderState *,
    const void *guest,size_t bytes,uint32_t *words);
VkResult ch_vk_guest_z24s8_store(const Nv2aSurface *,const Nv2aRenderState *,
    void *guest,size_t bytes,const uint32_t *words);
#endif
