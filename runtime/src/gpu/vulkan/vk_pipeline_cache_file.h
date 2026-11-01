#ifndef CH_VK_PIPELINE_CACHE_FILE_H
#define CH_VK_PIPELINE_CACHE_FILE_H
#include <vulkan/vulkan.h>
/* Optional local driver-cache persistence. Missing, corrupt, oversized or
 * incompatible files create an empty cache. Vulkan allocation errors propagate.
 * Caller owns the returned cache. loaded receives accepted driver bytes or 0.
 * File envelope checks accidental corruption; it is not authentication. */
VkResult ch_vk_pipeline_cache_file_load(VkPhysicalDevice physical,VkDevice device,
    const char *path,VkPipelineCache *cache,size_t *loaded);
/* Same validated file envelope, from immutable packaged memory. */
VkResult ch_vk_pipeline_cache_memory_load(VkPhysicalDevice,VkDevice,const void *,size_t,VkPipelineCache *,size_t *);
/* Build-generated packaged seeds. Returns an empty cache if none match. */
VkResult ch_vk_pipeline_cache_packaged_load(VkPhysicalDevice,VkDevice,VkPipelineCache *,size_t *);
/* Call only with pipeline creation/external cache access serialized. Best effort:
 * 1 means atomic replacement succeeded, 0 leaves the prior file untouched.
 * Parent directory must exist. Windows file persistence is currently disabled. */
int ch_vk_pipeline_cache_file_save(VkDevice device,VkPipelineCache cache,const char *path);
#endif
