#ifndef CH_VK_FRAME_MAILBOX_H
#define CH_VK_FRAME_MAILBOX_H
#include "vk_nv2a.h"
#include "../../host/vr_view.h"
typedef struct ChVkFrameMailbox ChVkFrameMailbox;
typedef struct { uint32_t width,height; size_t bytes; unsigned char *rgba; } ChVkCpuImage;
typedef struct {
    Nv2aSurface surface;
    uint64_t sequence;
    size_t bytes;
    unsigned char *rgba;
    int immersive;
    Vc3VrView view;
    VrGameProj projection;
    ChVkCpuImage eyes[2],hud;
} ChVkCpuFrame;
/* Latest completed frame. Producer copies borrowed RGBA; consumer takes sole
 * ownership. No GPU objects cross threads; the consumer uploads with its own
 * Vulkan host context. Shared Vulkan queues still require external
 * synchronization; the mailbox performs no Vulkan calls or queue locking.
 * New unconsumed frames replace older pending frames.
 * No pacing, guest completion, input or XR frame lifecycle is hidden here. */
VkResult ch_vk_frame_mailbox_create(ChVkFrameMailbox **);
VkResult ch_vk_frame_mailbox_publish(void *,const Nv2aSurface *,const void *,size_t);
/* Atomic deep copy of all planes and render metadata. Immersive packets need
 * both eyes; HUD optional. Flat packets cannot carry stereo/HUD planes.
 * sequence is assigned by mailbox; input pointers remain caller-owned. */
VkResult ch_vk_frame_mailbox_publish_frame(ChVkFrameMailbox *,const ChVkCpuFrame *);
/* out must be empty. VK_NOT_READY leaves it unchanged. */
VkResult ch_vk_frame_mailbox_take(ChVkFrameMailbox *,ChVkCpuFrame *out);
void ch_vk_cpu_frame_release(ChVkCpuFrame *);
/* Stop and join both threads before destroying. Owned consumer frames survive. */
void ch_vk_frame_mailbox_destroy(ChVkFrameMailbox *);
#endif
