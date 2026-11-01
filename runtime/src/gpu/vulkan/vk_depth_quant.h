#ifndef CH_VK_DEPTH_QUANT_H
#define CH_VK_DEPTH_QUANT_H
#include <vulkan/vulkan.h>
typedef struct ChVkDepthQuant ChVkDepthQuant;
/* Buffer is same-device storage+transfer src/dst, with >= width*height*4
 * bytes. Caller serializes recording/execution and retains buffer and helper
 * until recorded work completes. D32_SFLOAT_S8_UINT only, outside renderpass.
 * Image begins/ends in DEPTH_STENCIL_ATTACHMENT_OPTIMAL; stencil unchanged. */
VkResult ch_vk_depth_quant_create(VkDevice,VkBuffer,VkDeviceSize,ChVkDepthQuant **);
/* Alternative exact depth-only draw. Borrowed D32S8 attachment view and buffer
 * must survive completion; no input attachment or feedback-loop feature needed. */
VkResult ch_vk_depth_quant_create_draw(VkDevice,VkBuffer,VkDeviceSize,VkImageView,uint32_t,uint32_t,ChVkDepthQuant **);
/* Direct input-attachment prototype; image needs INPUT_ATTACHMENT usage. */
VkResult ch_vk_depth_quant_create_input(VkDevice,VkImage,VkImageView,uint32_t,uint32_t,ChVkDepthQuant **);
void ch_vk_depth_quant_record(ChVkDepthQuant *,VkCommandBuffer,VkImage,uint32_t,uint32_t);
void ch_vk_depth_quant_destroy(ChVkDepthQuant *);
#endif
