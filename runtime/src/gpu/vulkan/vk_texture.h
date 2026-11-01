#ifndef CHIHIRO_VK_TEXTURE_H
#define CHIHIRO_VK_TEXTURE_H
#include <vulkan/vulkan.h>
/* Immutable texture, optionally with a generated mip chain. Host owns
 * device/queue; calls are serialized.
 * CPU Nv2a decoder output is native uint32 ARGB, not byte-order-dependent RGBA.
 * Upload completes before return. Destroy only after all sampling has completed. */
typedef struct ChVkTexture ChVkTexture;
VkResult ch_vk_texture_upload(VkPhysicalDevice gpu, VkDevice device, VkQueue queue,
    uint32_t family, uint32_t width, uint32_t height, const uint32_t *argb,
    ChVkTexture **out);
/* Generate a full filtered mip chain from level zero, as glGenerateMipmap.
 * levels must be one or the full chain length for the given dimensions. */
VkResult ch_vk_texture_upload_mips(VkPhysicalDevice,VkDevice,VkQueue,uint32_t family,
    uint32_t width,uint32_t height,const uint32_t *argb,uint32_t levels,ChVkTexture **out);
uint32_t ch_vk_texture_levels(const ChVkTexture *);
int ch_vk_texture_is_cube(const ChVkTexture *);
/* Six tightly packed ARGB planes, in +X,-X,+Y,-Y,+Z,-Z order. */
VkResult ch_vk_texture_upload_cube(VkPhysicalDevice,VkDevice,VkQueue,uint32_t family,
    uint32_t size,const uint32_t *faces,ChVkTexture **out);
VkResult ch_vk_texture_read_level_rgba(ChVkTexture *,uint32_t level,void *,VkDeviceSize);
/* Descriptor uses SHADER_READ_ONLY_OPTIMAL, nearest filtering and clamp. */
VkDescriptorImageInfo ch_vk_texture_descriptor(const ChVkTexture *texture);
/* Diagnostic GPU copy, restores shader-read layout; caller excludes sampling.
 * Cube readbacks contain all six tightly packed faces in upload order. */
VkResult ch_vk_texture_read_rgba(ChVkTexture *texture, void *rgba, VkDeviceSize bytes);
void ch_vk_texture_destroy(ChVkTexture *texture);
/* Caller must have completed all previous sampling before replacing sampler. */
VkResult ch_vk_texture_set_sampler(ChVkTexture *texture, const VkSamplerCreateInfo *info);
#endif
