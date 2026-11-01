#ifndef CH_VK_TEXTURE_CACHE_H
#define CH_VK_TEXTURE_CACHE_H
#include "vk_nv2a.h"
#include "../guest_write_watch.h"
typedef struct ChVkTextureCache ChVkTextureCache;
#define CH_VK_TEXTURE_CACHE_DEFAULT_ENTRIES 2048u
typedef struct {
    VkPhysicalDevice physical; VkDevice device; VkQueue queue; uint32_t family;
    const void *guest_base; size_t guest_bytes;
    ChVkDecodeTexture decode;
    /* Optional tracker with all alias/fault/syscall hooks installed. Borrowed
     * until cache destruction; unsupported ranges retain exact comparison. */
    ChGuestWriteWatch *guest_write_watch;
    int gpu_decode; /* common 2D ARGB/XRGB on a compute-capable graphics queue */
    VkPipelineCache pipeline_cache; /* optional borrowed compiler cache */
    int reuse_uploads; /* reuse matching unpinned CPU texture allocations */
    int queued_uploads; /* opt-in: all sampling follows on the configured queue */
    size_t max_bytes; uint32_t max_entries; /* zero: 64 MiB / 2048 entries */
} ChVkTextureCacheConfig;
typedef struct { uint64_t hits,misses,uploads,evictions,gpu_copies,gpu_hits,gpu_reuses,cpu_checks,cpu_check_bytes,cpu_changes,tracked_checks,cpu_reuses,gpu_decodes; size_t bytes; uint32_t entries,capacity; } ChVkTextureCacheStats;
VkResult ch_vk_texture_cache_create(const ChVkTextureCacheConfig *,ChVkTextureCache **);
/* Encoded level-zero byte range (full padded six-face footprint for cubes),
 * with an optional 1024-byte P8 palette.
 * Metadata only; does not dereference guest memory. VK_NOT_READY means the
 * caller must conservatively handle aliasing or retain its decoder fallback. */
VkResult ch_vk_texture_storage_span(const Nv2aTexture *,size_t *span,size_t *palette);
/* Resolver for serialized guest/host draws. With queued_uploads, subsequent
 * sampling must use the configured queue. Exact encoded level-zero bytes
 * and palette are checked on every acquire. Sampler metadata is part of key.
 * A hit returns an immutable pinned image; caller releases only after its GPU
 * use completes. Pinned entries cannot be evicted or overwritten by CPU edits.
 * VK_NOT_READY retains the normal decoder path for unsupported storage or a
 * cache budget/pin conflict. Invalid guest spans fail before any decode. */
VkResult ch_vk_texture_cache_resolve(void *,const Nv2aTexture *,ChVkSampleImage *);
/* Caller proves exact render-target/texture storage equivalence and observes
 * CPU edits first. serial uniquely identifies that source's color contents.
 * Same serialized device/queue/family required. Snapshots share this cache's
 * budget and completion-gated pins, and never publish guest color memory. */
VkResult ch_vk_texture_cache_resolve_target(ChVkTextureCache *,const Nv2aTexture *,
    ChVkTarget *,uint64_t serial,ChVkSampleImage *);
void ch_vk_texture_cache_stats(const ChVkTextureCache *,ChVkTextureCacheStats *);
/* Stop sampling first. All returned pins must have been released. */
void ch_vk_texture_cache_destroy(ChVkTextureCache *);
#endif
