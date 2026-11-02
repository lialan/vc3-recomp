#ifndef CHIHIRO_VK_STATE_H
#define CHIHIRO_VK_STATE_H
#include <vulkan/vulkan.h>
#include "nv2a_backend.h"
#include "../nv2a_texture_mips.h"
/* Possible attachment writes after a validated draw: bit0 color, bit1 Z/S.
 * Conservative for data-dependent comparisons; no rasterization is skipped. */
unsigned ch_vk_attachment_write_mask(const Nv2aRenderState *guest);
/* Unsupported enums fail before command recording; never substitute defaults. */
VkResult ch_vk_blend_state(const Nv2aRenderState *guest,
    VkPipelineColorBlendAttachmentState *attachment, float constants[4]);
VkResult ch_vk_depth_state(const Nv2aRenderState *guest, VkPipelineDepthStencilStateCreateInfo *out);
VkResult ch_vk_sampler_state(const Nv2aTexture *guest, VkSamplerCreateInfo *out);
#endif
