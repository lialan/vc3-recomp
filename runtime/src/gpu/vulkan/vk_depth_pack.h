#ifndef CH_VK_DEPTH_PACK_H
#define CH_VK_DEPTH_PACK_H
#include <vulkan/vulkan.h>
typedef struct ChVkDepthPack ChVkDepthPack;
/* Same-device buffer layout: N depth float words, ceil(N/4) stencil words,
 * N guest Z24/S8 words. Requires a compute-capable caller queue. */
VkResult ch_vk_depth_pack_create(VkDevice,VkPipelineCache,VkBuffer,VkDeviceSize,ChVkDepthPack **);
void ch_vk_depth_pack_record(ChVkDepthPack *,VkCommandBuffer,uint32_t,int);
void ch_vk_depth_pack_destroy(ChVkDepthPack *);
#endif
