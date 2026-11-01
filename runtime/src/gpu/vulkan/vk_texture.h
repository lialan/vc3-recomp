#ifndef CHIHIRO_VK_TEXTURE_H
#define CHIHIRO_VK_TEXTURE_H
#include <vulkan/vulkan.h>
#include <stddef.h>
#include "nv2a_backend.h"
/* Texture immutable throughout every sampling lease, optionally with a mip chain. Host owns
 * device/queue; calls are serialized.
 * CPU Nv2a decoder output is native uint32 ARGB, not byte-order-dependent RGBA.
 * Synchronous upload completes before return (except the explicit queued API). Destroy only after all sampling has completed. */
typedef struct ChVkTexture ChVkTexture;
VkResult ch_vk_texture_upload(VkPhysicalDevice gpu, VkDevice device, VkQueue queue,
    uint32_t family, uint32_t width, uint32_t height, const uint32_t *argb,
    ChVkTexture **out);
/* Generate a full filtered mip chain from level zero, as glGenerateMipmap.
 * levels must be one or the full chain length for the given dimensions. */
VkResult ch_vk_texture_upload_mips(VkPhysicalDevice,VkDevice,VkQueue,uint32_t family,
    uint32_t width,uint32_t height,const uint32_t *argb,uint32_t levels,ChVkTexture **out);
/* Queue an immutable upload without a host wait. All sampling must be submitted
 * later on the same queue, with externally serialized queue access. Input bytes
 * are copied before return; staging and commands remain owned until completion.
 * Readback/reuse/destroy wait for the upload; destroy still requires completed
 * sampling leases. cube requests six square planes and levels==1. */
VkResult ch_vk_texture_upload_queued(VkPhysicalDevice,VkDevice,VkQueue,uint32_t family,
    uint32_t width,uint32_t height,const uint32_t *argb,uint32_t levels,int cube,ChVkTexture **out);
/* Replace all base-level pixels and regenerate existing mips, preserving the
 * allocation and sampler. Caller must complete ALL prior sampling leases.
 * Waits for any previous upload before reusing staging; queued follows the
 * same-queue contract above. pixels counts all six faces for cube images. */
VkResult ch_vk_texture_replace_argb(ChVkTexture *,const uint32_t *argb,size_t pixels,int queued);
/* Shared compute decoder for 2D ARGB/XRGB, linear or rectangular Morton.
 * Raw bytes are snapshotted before return; sampling/replacement contracts match
 * upload_queued/replace_argb. Requires a compute-capable graphics queue.
 * Other formats deliberately return FEATURE_NOT_PRESENT before GPU work. */
int ch_vk_texture_can_decode32(const Nv2aTexture *);
VkResult ch_vk_texture_replace_encoded32(ChVkTexture *,VkPipelineCache,const Nv2aTexture *,const void *,size_t,int queued);
VkResult ch_vk_texture_upload_encoded32(VkPhysicalDevice,VkDevice,VkQueue,uint32_t family,VkPipelineCache,
    const Nv2aTexture *,const void *,size_t,uint32_t levels,int queued,ChVkTexture **);
/* Immutable RGBA8 snapshot of a completed, same-device single-layer source in
 * TRANSFER_SRC_OPTIMAL. No CPU readback/upload. Source layout is unchanged;
 * optional mips use the same generation policy as CPU uploads. */
VkResult ch_vk_texture_copy_image(VkPhysicalDevice,VkDevice,VkQueue,uint32_t family,
    uint32_t width,uint32_t height,VkImage source,uint32_t levels,ChVkTexture **out);
/* Reuse an existing image only after every previous sampling pin completes.
 * Same source contract; checks device/extent and regenerates existing mips. */
VkResult ch_vk_texture_copy_from_image(ChVkTexture *,VkDevice,VkImage source,uint32_t width,uint32_t height);
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
