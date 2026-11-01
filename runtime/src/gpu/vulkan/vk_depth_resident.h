#ifndef CH_VK_DEPTH_RESIDENT_H
#define CH_VK_DEPTH_RESIDENT_H
#include "vk_target.h"
typedef struct ChVkDepthResident ChVkDepthResident;
typedef struct {uint64_t imports,writebacks,cpu_merges,binds,gpu_copies,clear_skips;} ChVkDepthResidentStats;
VkResult ch_vk_depth_resident_create(void *guest,size_t bytes,ChVkDepthResident **);
/* One current owner: an exact shared-zeta target switch copies on the GPU.
 * Changing zeta interpretation publishes/imports, including partial overlaps.
 * Targets must use native D24S8. Guest RAM must be serialized with callbacks.
 * Retains target until release; no device or guest-memory ownership. */
VkResult ch_vk_depth_resident_bind(ChVkDepthResident *,ChVkTarget *,const Nv2aSurface *,const Nv2aRenderState *);
/* Caller MUST immediately clear every depth AND stencil pixel on this target
 * before any other owner operation. Reinterpreted old storage still publishes;
 * exact superseded storage and the new target's old pixels need no import. */
VkResult ch_vk_depth_resident_bind_clear(ChVkDepthResident *,ChVkTarget *,const Nv2aSurface *,const Nv2aRenderState *);
/* Diagnostic synchronous roundtrip control. Default is enabled. */
void ch_vk_depth_resident_gpu_copies(ChVkDepthResident *,int enabled);
/* Diagnostic float-conversion control. Default packed; switch only unbound. */
VkResult ch_vk_depth_resident_packed(ChVkDepthResident *,int enabled);
VkResult ch_vk_depth_resident_dirty(ChVkDepthResident *,ChVkTarget *);
/* Merge CPU changes per depth/stencil component, then publish at completion.
 * Padding is excluded from edits and preserved on writeback. */
VkResult ch_vk_depth_resident_flush(ChVkDepthResident *);
VkResult ch_vk_depth_resident_release(ChVkDepthResident *);
VkResult ch_vk_depth_resident_sample(ChVkDepthResident *,const Nv2aTexture *);
/* Color/depth reinterpretation requires a unified owner not provided here. */
VkResult ch_vk_depth_resident_check_color(const ChVkDepthResident *,uint32_t va,size_t span);
void ch_vk_depth_resident_stats(const ChVkDepthResident *,ChVkDepthResidentStats *);
/* Release first, while the target and serialized queue are still alive. */
void ch_vk_depth_resident_destroy(ChVkDepthResident *);
#endif
