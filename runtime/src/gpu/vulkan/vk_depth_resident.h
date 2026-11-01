#ifndef CH_VK_DEPTH_RESIDENT_H
#define CH_VK_DEPTH_RESIDENT_H
#include "vk_target.h"
#include "../guest_write_watch.h"
typedef struct ChVkDepthResident ChVkDepthResident;
typedef struct {uint64_t imports,writebacks,cpu_merges,binds,gpu_copies,clear_skips,clean_reuses;
    /* Index: bit1 different target image, bit0 overlapping guest storage. */
    uint64_t switches[4],dirty_switches[4];
    /* Guest transfer work only; row memcpy and CPU edit comparisons excluded. */
    uint64_t cpu_layout_pixels,cpu_float_pixels;} ChVkDepthResidentStats;
VkResult ch_vk_depth_resident_create(void *guest,size_t bytes,ChVkDepthResident **);
/* Set before binding a target. Host keeps tracker and hooks alive. */
void ch_vk_depth_resident_write_watch(ChVkDepthResident *,ChGuestWriteWatch *);
/* One current owner: an exact shared-zeta target switch copies on the GPU.
 * Changing zeta interpretation publishes/imports, including partial overlaps.
 * Targets must preserve exact guest Z24 values (native D24 or quantized D32).
 * Guest RAM must be serialized with callbacks.
 * Retains target until release; no device or guest-memory ownership. */
VkResult ch_vk_depth_resident_bind(ChVkDepthResident *,ChVkTarget *,const Nv2aSurface *,const Nv2aRenderState *);
/* Caller MUST immediately clear every depth AND stencil pixel on this target
 * before any other owner operation. Reinterpreted old storage still publishes;
 * exact superseded storage and the new target's old pixels need no import. */
VkResult ch_vk_depth_resident_bind_clear(ChVkDepthResident *,ChVkTarget *,const Nv2aSurface *,const Nv2aRenderState *);
/* Optional bounded clean-image reuse: four borrowed targets / 16 MiB of raw
 * snapshots. Release before any target destruction or external depth mutation. */
void ch_vk_depth_resident_reuse_clean(ChVkDepthResident *,int enabled);
/* Diagnostic synchronous roundtrip control. Default is enabled. */
void ch_vk_depth_resident_gpu_copies(ChVkDepthResident *,int enabled);
/* Enable before binding; full Morton packed transfers use GPU layout conversion. */
void ch_vk_depth_resident_gpu_layout(ChVkDepthResident *,int enabled);
/* Diagnostic float-conversion control. Default packed; switch only unbound. */
VkResult ch_vk_depth_resident_packed(ChVkDepthResident *,int enabled);
VkResult ch_vk_depth_resident_dirty(ChVkDepthResident *,ChVkTarget *);
/* Merge CPU changes per depth/stencil component, then publish at completion.
 * Padding is excluded from edits and preserved on writeback. */
/* Current GPU-written owner needing publication; borrowed, serialized access. */
ChVkTarget *ch_vk_depth_resident_dirty_target(const ChVkDepthResident *);
VkResult ch_vk_depth_resident_flush(ChVkDepthResident *);
/* Requires dirty_target != NULL. Also returns that target's RGBA bytes, without
 * publishing color guest storage; caller retains its own CPU-edit merge rules. */
VkResult ch_vk_depth_resident_flush_color(ChVkDepthResident *,void *rgba,size_t bytes);
VkResult ch_vk_depth_resident_release(ChVkDepthResident *);
VkResult ch_vk_depth_resident_sample(ChVkDepthResident *,const Nv2aTexture *);
/* Color/depth reinterpretation requires a unified owner not provided here. */
VkResult ch_vk_depth_resident_check_color(const ChVkDepthResident *,uint32_t va,size_t span);
void ch_vk_depth_resident_stats(const ChVkDepthResident *,ChVkDepthResidentStats *);
/* Release first, while the target and serialized queue are still alive. */
void ch_vk_depth_resident_destroy(ChVkDepthResident *);
#endif
