#ifndef CH_VK_SPIRV_SPECIALIZE_H
#define CH_VK_SPIRV_SPECIALIZE_H
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The module with spec's constants frozen and folded (loops unrolled, dead
 * code gone); malloc'd, the caller frees it. 0: done (from the cache, or
 * computed when compute is set, and then cached); 1: not cached and compute
 * not set; -1: failed. Otherwise the caller keeps the original module and
 * its VkSpecializationInfo. */
int ch_vk_spirv_specialize(const uint32_t *code,size_t bytes,const VkSpecializationInfo *spec,
    int compute,uint32_t **out_code,size_t *out_bytes);
/* The file the specialized modules are kept in (loaded now, appended to). */
void ch_vk_spirv_specialize_cache_open(const char *path);
#ifdef __cplusplus
}
#endif
#endif
