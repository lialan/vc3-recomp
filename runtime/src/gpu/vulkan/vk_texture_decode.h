#ifndef CH_VK_TEXTURE_DECODE_H
#define CH_VK_TEXTURE_DECODE_H
#include <vulkan/vulkan.h>
typedef struct ChVkTextureDecode ChVkTextureDecode;
/* Buffer contains tightly packed RGBA output followed by encoded source words.
 * Caller retains it through completion and serializes the compute queue. */
VkResult ch_vk_texture_decode_create(VkDevice,VkPipelineCache,VkBuffer,VkDeviceSize,ChVkTextureDecode **);
void ch_vk_texture_decode_record(ChVkTextureDecode *,VkCommandBuffer,uint32_t,uint32_t,uint32_t,uint32_t);
void ch_vk_texture_decode_destroy(ChVkTextureDecode *);
#endif
