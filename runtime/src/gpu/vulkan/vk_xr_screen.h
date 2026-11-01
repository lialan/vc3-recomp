#ifndef CH_VK_XR_SCREEN_H
#define CH_VK_XR_SCREEN_H
#include "vk_frame_mailbox.h"
#include "vk_xr_quad.h"
typedef struct ChVkXrScreen ChVkXrScreen;
/* Display-thread flat-screen presenter. Borrows running session, space,
 * mailbox and Vulkan handles. Session/input/frame begin/end stay with host.
 * Uses opaque composition: framebuffer alpha is guest data, not coverage.
 * Caller serializes this Vulkan queue against all its other users. */
XrResult ch_vk_xr_screen_create(XrInstance,XrSession,XrSpace,PFN_xrGetInstanceProcAddr,
    VkPhysicalDevice,VkDevice,VkQueue,uint32_t family,ChVkFrameMailbox *,ChVkXrScreen **);
/* Call only between completed prior frame and submission of the next layer.
 * Timeout holds the exact CPU frame/target/quad pose; retry without consuming
 * newer mailbox frames. No new frame reuses the released image and updates
 * pose only. No received frame yields success with a NULL layer.
 * sequence changes only after successful copy and release. */
XrResult ch_vk_xr_screen_update(ChVkXrScreen *,const XrPosef *,XrExtent2Df,
    XrDuration,VkResult *,uint64_t *sequence);
const XrCompositionLayerBaseHeader *ch_vk_xr_screen_layer(const ChVkXrScreen *);
/* Stop producer/consumer first. Pending acquired image must finish by retry. */
XrResult ch_vk_xr_screen_destroy(ChVkXrScreen *);
#endif
