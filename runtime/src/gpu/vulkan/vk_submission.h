#ifndef CH_VK_SUBMISSION_H
#define CH_VK_SUBMISSION_H
#include "vk_backend.h"
#include "vk_scene.h"
#include "vk_frame_mailbox.h"
typedef struct ChVkSubmission ChVkSubmission;
typedef struct {
    /* Runtime owns all backend rendering hooks; supply only device/guest/decode,
     * precise_queries and fatal. Handles/memory remain host-owned. */
    ChVkBackendConfig backend;
    uint32_t eye_width,eye_height,hud_width,hud_height;
    ChVkFrameMailbox *frames;
    void *view_context;
    VkResult (*latch_view)(void *,Vc3VrView *);
} ChVkSubmissionConfig;
/* Connects registered guest backend -> scene/coherence -> atomic owned packet.
 * Serialized executor thread only, including create/attach/destroy. Host display
 * consumes mailbox on its own thread; no Vulkan objects cross that boundary.
 * Uses synchronous readback as correctness baseline, not a performance path. */
VkResult ch_vk_submission_create(const ChVkSubmissionConfig *,ChVkSubmission **);
VkResult ch_vk_submission_attach(ChVkSubmission *);
/* Stop and drain executor first; mailbox and consumed packets remain alive. */
void ch_vk_submission_destroy(ChVkSubmission *);
#endif
