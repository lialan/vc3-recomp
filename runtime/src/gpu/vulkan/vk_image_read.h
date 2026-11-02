#ifndef CH_VK_IMAGE_READ_H
#define CH_VK_IMAGE_READ_H
#include <vulkan/vulkan.h>
typedef struct ChVkImageRead ChVkImageRead;
/* D32/S8 packs guest depth into the final depth-pack buffer plane. RGBA8
 * packs BGRA bytes into plane zero (linear) or one (Morton). The caller
 * owns attachment layouts, compute capability, bounds and resource lifetime. */
VkResult ch_vk_image_read_create(VkDevice,VkPipelineCache,VkBuffer,VkDeviceSize,VkImage,VkFormat,ChVkImageRead **);
void ch_vk_image_read_record(ChVkImageRead *,VkCommandBuffer,uint32_t,uint32_t,int);
/* Pair D32/S8 and BGRA color publication in one dispatch; both images must
 * have equal dimensions. Each output retains its independent guest layout. */
VkResult ch_vk_image_read_create_pair(VkDevice,VkPipelineCache,VkBuffer,VkDeviceSize,VkImage,VkImage,VkBuffer,VkDeviceSize,ChVkImageRead **);
void ch_vk_image_read_record_pair(ChVkImageRead *,VkCommandBuffer,uint32_t,uint32_t,int,int);
void ch_vk_image_read_destroy(ChVkImageRead *);
#endif
