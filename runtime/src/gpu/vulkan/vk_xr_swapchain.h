#ifndef CHIHIRO_VK_XR_SWAPCHAIN_H
#define CHIHIRO_VK_XR_SWAPCHAIN_H
#include "vk_xr.h"
#include "vk_target.h"
typedef struct ChVkXrSwapchain ChVkXrSwapchain;
/* Single-layer, single-sample RGBA8 UNORM color target; reject unsupported
 * runtime formats rather than silently changing color-space semantics. */
XrResult ch_vk_xr_swapchain_create(XrInstance instance, XrSession session,
    PFN_xrGetInstanceProcAddr get_proc, VkDevice device, uint32_t width,
    uint32_t height, ChVkXrSwapchain **out);
/* Timeout retains the acquired index; retry wait with this same function.
 * Outputs are populated only on successful wait. No second acquire on retry. */
XrResult ch_vk_xr_swapchain_wait(ChVkXrSwapchain *swapchain, XrDuration timeout,
    uint32_t *index, VkImage *image);
/* Caller must finish image in COLOR_ATTACHMENT_OPTIMAL and submit completion
 * fence on the owning graphics queue. Pending fence never releases ownership.
 * Fence must belong to this device and cover all image use, and remain valid. */
XrResult ch_vk_xr_swapchain_release(ChVkXrSwapchain *swapchain, VkFence fence, VkResult *vk_result);
/* Reject destruction while an image is acquired. Renderer/XR session lifetime
 * and frame composition are host responsibilities. */
XrResult ch_vk_xr_swapchain_destroy(ChVkXrSwapchain *swapchain);
XrSwapchain ch_vk_xr_swapchain_handle(const ChVkXrSwapchain *swapchain);
/* Copy exact-sized target to the waited image, transition, fence, then release.
 * verification must be NULL: XR swapchains are not created with transfer-src
 * usage. Diagnose copies on ordinary Vulkan images via target export. */
XrResult ch_vk_xr_swapchain_copy_release(ChVkXrSwapchain *swapchain,
    ChVkTarget *target, void *verification, VkDeviceSize bytes, VkResult *vk_result);
XrResult ch_vk_xr_swapchain_texture_release(ChVkXrSwapchain *,ChVkTexture *,VkResult *);
/* Fill the waited image from packed RGBA8 rows in a buffer of this device, on
 * queue (of family), and release it; complete on return. */
XrResult ch_vk_xr_swapchain_buffer_release(ChVkXrSwapchain *,VkQueue queue,uint32_t family,
    VkBuffer buffer,uint32_t width,uint32_t height,VkResult *);
/* Wait for the last buffer copy (before its source buffer is reused). */
XrResult ch_vk_xr_swapchain_settle(ChVkXrSwapchain *s);
/* Without waiting (OpenXR needs the writes submitted to the session's queue
 * before the release, nothing more; SteamVR gates that queue once a display
 * frame, so each wait cost up to one): record a copy of packed RGBA8 rows
 * into the waited image (left COLOR_ATTACHMENT_OPTIMAL), and release an
 * image whose writes the caller has submitted. */
XrResult ch_vk_xr_swapchain_record_buffer(ChVkXrSwapchain *s,VkCommandBuffer command,VkBuffer buffer,VkDeviceSize offset);
XrResult ch_vk_xr_swapchain_release_submitted(ChVkXrSwapchain *s);
#endif
