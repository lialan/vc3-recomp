#ifndef CH_VK_COLOR_PACK_H
#define CH_VK_COLOR_PACK_H
#include <vulkan/vulkan.h>
typedef struct ChVkColorPack ChVkColorPack;
/* RGBA/BGRA permutation; Morton mode uses disjoint N-word linear/guest
 * planes to avoid cross-invocation overwrite. Requires a compute queue. */
VkResult ch_vk_color_pack_create(VkDevice,VkPipelineCache,VkBuffer,VkDeviceSize,ChVkColorPack **);
void ch_vk_color_pack_record(ChVkColorPack *,VkCommandBuffer,uint32_t,uint32_t,int,int);
void ch_vk_color_pack_destroy(ChVkColorPack *);
#endif
