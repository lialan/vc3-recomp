#ifndef CH_VK_DEPTH_PACK_H
#define CH_VK_DEPTH_PACK_H
#include <vulkan/vulkan.h>
typedef struct ChVkDepthPack ChVkDepthPack;
/* Same-device buffer layout: N D32 float or D24 integer depth words, ceil(N/4) stencil words,
 * N linear or full rectangular Morton guest Z24/S8 words. Requires a compute-capable caller queue. */
VkResult ch_vk_depth_pack_create(VkDevice,VkPipelineCache,VkBuffer,VkDeviceSize,ChVkDepthPack **);
void ch_vk_depth_pack_record(ChVkDepthPack *,VkCommandBuffer,uint32_t width,uint32_t height,int upload,int morton,int native_z24);
void ch_vk_depth_pack_destroy(ChVkDepthPack *);
#endif
