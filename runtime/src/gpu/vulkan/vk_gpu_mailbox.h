#ifndef CH_VK_GPU_MAILBOX_H
#define CH_VK_GPU_MAILBOX_H
#include "vk_nv2a.h"
typedef struct ChVkGpuMailbox ChVkGpuMailbox;
typedef struct {
    Nv2aSurface surface;
    uint64_t sequence;
    ChVkTexture *image;
    ChVkGpuMailbox *owner;
    unsigned slot;
} ChVkGpuFrame;
/* Three immutable GPU snapshots. One producer; any taken frame owns a slot
 * until release. No CPU pixels, readback, or upload. Vulkan calls (publish,
 * consumer image use, destroy) require the host's shared queue lock. Take and
 * release only use the mailbox mutex. Never hold that mutex while calling GPU
 * operations or acquiring the queue lock. */
VkResult ch_vk_gpu_mailbox_create(ChVkGpuMailbox **);
/* Bridge mode (the game on a VkDevice of its own): publish copies each frame,
 * packed RGBA8, into its slot's buffer -- buffers[slot], of the target's
 * device, capacity bytes each -- and complete; a taken frame then has no
 * image, only its slot, whose shared buffer the consumer reads. */
void ch_vk_gpu_mailbox_set_buffers(ChVkGpuMailbox *,const VkBuffer buffers[3],VkDeviceSize capacity);
/* Source must contain current pixels, including any guest CPU edits.
 * VK_NOT_READY means all slots are held: keep the previously displayed frame. */
VkResult ch_vk_gpu_mailbox_publish(void *,const Nv2aSurface *,ChVkTarget *);
VkResult ch_vk_gpu_mailbox_take(ChVkGpuMailbox *,ChVkGpuFrame *);
/* Finish consumer GPU use before release. Frames are move-only values. */
void ch_vk_gpu_frame_release(ChVkGpuFrame *);
/* Stop/join users first. Returns VK_NOT_READY while taken frames remain;
 * leaves mailbox intact for later release and retry. */
VkResult ch_vk_gpu_mailbox_destroy(ChVkGpuMailbox *);
#endif
