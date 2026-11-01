#ifndef CHIHIRO_VK_TEXTURE_H
#define CHIHIRO_VK_TEXTURE_H
#include <vulkan/vulkan.h>
/* Immutable level-0 texture. Host owns device/queue; calls are serialized.
 * CPU Nv2a decoder output is native uint32 ARGB, not byte-order-dependent RGBA.
 * Upload completes before return. Destroy only after all sampling has completed. */
typedef struct ChVkTexture ChVkTexture;
VkResult ch_vk_texture_upload(VkPhysicalDevice gpu, VkDevice device, VkQueue queue,
    uint32_t family, uint32_t width, uint32_t height, const uint32_t *argb,
    ChVkTexture **out);
/* Descriptor uses SHADER_READ_ONLY_OPTIMAL, nearest filtering and clamp. */
VkDescriptorImageInfo ch_vk_texture_descriptor(const ChVkTexture *texture);
/* Diagnostic GPU copy, restores shader-read layout; caller excludes sampling. */
VkResult ch_vk_texture_read_rgba(ChVkTexture *texture, void *rgba, VkDeviceSize bytes);
void ch_vk_texture_destroy(ChVkTexture *texture);
/* Caller must have completed all previous sampling before replacing sampler. */
VkResult ch_vk_texture_set_sampler(ChVkTexture *texture, const VkSamplerCreateInfo *info);
#endif
