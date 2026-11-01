#ifndef CH_VK_XR_FRAME_H
#define CH_VK_XR_FRAME_H
#include "vk_xr_swapchain.h"
typedef struct ChVkXrFrame ChVkXrFrame;
/* Host owns live/running session, reference space and device. Session events,
 * begin/end-session transitions, input and overlays stay with the host. */
XrResult ch_vk_xr_frame_create(XrInstance,XrSession,XrSystemId,XrSpace,
    PFN_xrGetInstanceProcAddr,VkDevice,ChVkXrFrame **);
XrResult ch_vk_xr_frame_extent(const ChVkXrFrame *,uint32_t eye,uint32_t *width,uint32_t *height);
/* Only one active frame. Returned poses/FOV at predicted display time drive
 * the renderer; valid flags, not tracking flags, decide projection eligibility. */
XrResult ch_vk_xr_frame_begin(ChVkXrFrame *,XrFrameState *,XrView views[2],XrViewStateFlags *);
/* Targets must match each eye's extent and remain unchanged across retries.
 * Timeout preserves frame and acquired image ownership; call submit again.
 * shouldRender=false or invalid poses submits zero layers and needs no targets. */
XrResult ch_vk_xr_frame_submit(ChVkXrFrame *,ChVkTarget *const eyes[2],XrDuration timeout,VkResult *);
XrResult ch_vk_xr_frame_destroy(ChVkXrFrame *);
/* Additional layers ordered after world projection (HUD/menu/overlay).
 * Borrowed headers and referenced swapchains stay alive and unchanged through
 * successful end-frame, including retries. No layers submitted when rendering
 * is disabled or view poses invalid. Maximum eight additional layers. */
XrResult ch_vk_xr_frame_submit_layers(ChVkXrFrame *,ChVkTarget *const eyes[2],
    XrDuration,const XrCompositionLayerBaseHeader *const *extra,uint32_t count,VkResult *);
/* Flat movie/menu/screen frames: quad/overlay layers without a world projection. */
XrResult ch_vk_xr_frame_submit_flat(ChVkXrFrame *,
    const XrCompositionLayerBaseHeader *const *layers,uint32_t count,VkResult *);
#endif
