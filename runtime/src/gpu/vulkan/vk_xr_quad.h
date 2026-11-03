#ifndef CH_VK_XR_QUAD_H
#define CH_VK_XR_QUAD_H
#include "vk_xr_swapchain.h"
typedef struct ChVkXrQuad ChVkXrQuad;
/* Host renders HUD/menu/screen into a Vulkan target then updates this layer.
 * Pixel format/layout/alpha semantics stay explicit. Session/space not owned. */
XrResult ch_vk_xr_quad_create(XrInstance,XrSession,PFN_xrGetInstanceProcAddr,VkDevice,
    uint32_t width,uint32_t height,XrSpace,const XrPosef *,XrExtent2Df,
    XrCompositionLayerFlags,ChVkXrQuad **);
XrResult ch_vk_xr_quad_pose(ChVkXrQuad *,const XrPosef *,XrExtent2Df);
/* Timeout retains acquired ownership and hides layer until update completes. */
XrResult ch_vk_xr_quad_update(ChVkXrQuad *,ChVkTarget *,XrDuration,VkResult *);
const XrCompositionLayerBaseHeader *ch_vk_xr_quad_layer(const ChVkXrQuad *);
XrResult ch_vk_xr_quad_destroy(ChVkXrQuad *);
XrResult ch_vk_xr_quad_update_texture(ChVkXrQuad *,ChVkTexture *,XrDuration,VkResult *);
/* The same from packed RGBA8 rows in a buffer (ch_vk_xr_swapchain_buffer_release). */
XrResult ch_vk_xr_quad_update_buffer(ChVkXrQuad *,VkQueue queue,uint32_t family,VkBuffer buffer,XrDuration,VkResult *);
/* Without waiting (ch_vk_xr_swapchain_record_buffer): the copy recorded
 * into command, then, once the caller has submitted it, the release. */
XrResult ch_vk_xr_quad_record_buffer(ChVkXrQuad *,VkCommandBuffer command,VkBuffer buffer,VkDeviceSize offset);
XrResult ch_vk_xr_quad_release_submitted(ChVkXrQuad *);
#endif
