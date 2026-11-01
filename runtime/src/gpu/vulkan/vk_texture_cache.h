#ifndef CH_VK_TEXTURE_CACHE_H
#define CH_VK_TEXTURE_CACHE_H
#include "vk_nv2a.h"
typedef struct ChVkTextureCache ChVkTextureCache;
typedef struct {
    VkPhysicalDevice physical; VkDevice device; VkQueue queue; uint32_t family;
    const void *guest_base; size_t guest_bytes;
    ChVkDecodeTexture decode;
    size_t max_bytes; uint32_t max_entries; /* zero: 64 MiB / 64 entries */
} ChVkTextureCacheConfig;
typedef struct { uint64_t hits,misses,uploads,evictions; size_t bytes; } ChVkTextureCacheStats;
VkResult ch_vk_texture_cache_create(const ChVkTextureCacheConfig *,ChVkTextureCache **);
/* Resolver for synchronous guest/host draws. Exact encoded level-zero bytes
 * and palette are checked on every acquire. Sampler metadata is part of key.
 * A hit returns an immutable pinned image; caller releases only after its GPU
 * use completes. Pinned entries cannot be evicted or overwritten by CPU edits.
 * VK_NOT_READY retains the normal decoder path for unsupported storage or a
 * cache budget/pin conflict. Invalid guest spans fail before any decode. */
VkResult ch_vk_texture_cache_resolve(void *,const Nv2aTexture *,ChVkSampleImage *);
void ch_vk_texture_cache_stats(const ChVkTextureCache *,ChVkTextureCacheStats *);
/* Stop sampling first. All returned pins must have been released. */
void ch_vk_texture_cache_destroy(ChVkTextureCache *);
#endif
