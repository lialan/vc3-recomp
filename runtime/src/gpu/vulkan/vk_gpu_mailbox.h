#ifndef CH_VK_GPU_MAILBOX_H
#define CH_VK_GPU_MAILBOX_H
#include "vk_nv2a.h"
#include "vk_immersive.h"
typedef struct ChVkGpuMailbox ChVkGpuMailbox;
typedef struct {
    Nv2aSurface surface;
    uint64_t sequence;
    ChVkTexture *image;
    ChVkGpuMailbox *owner;
    unsigned slot;
    /* An immersive frame (bridge mode only): the slot's buffer holds the
     * eyes, side by side (eye_w*2 x eye_h RGBA8), then from hud_offset the
     * HUD (surface size, premultiplied) when imm.hud. */
    int immersive;
    ChVkImmFrame imm;
    VkDeviceSize hud_offset;
    /* Bridge mode: the copy into the slot's buffer is asynchronous; wait on
     * this fence (of ready_device) before reading the buffer. */
    VkDevice ready_device;
    VkFence ready;
} ChVkGpuFrame;
/* Wait for a taken frame's copy (bridge mode); no-op otherwise. */
VkResult ch_vk_gpu_frame_wait(const ChVkGpuFrame *);
#define CH_VK_GPU_MAILBOX_SLOTS 4
/* Four immutable GPU snapshots. One producer; any taken frame owns a slot
 * until release. No CPU pixels, readback, or upload. Vulkan calls (publish,
 * consumer image use, destroy) require the host's shared queue lock. Take and
 * release only use the mailbox mutex. Never hold that mutex while calling GPU
 * operations or acquiring the queue lock. */
VkResult ch_vk_gpu_mailbox_create(ChVkGpuMailbox **);
/* Bridge mode (the game on a VkDevice of its own): publish copies each frame,
 * packed RGBA8, into its slot's buffer -- buffers[slot], of the target's
 * device, capacity bytes each -- and complete; a taken frame then has no
 * image, only its slot, whose shared buffer the consumer reads. */
void ch_vk_gpu_mailbox_set_buffers(ChVkGpuMailbox *,const VkBuffer buffers[CH_VK_GPU_MAILBOX_SLOTS],VkDeviceSize capacity);
/* How many finished frames may wait, taken oldest first (1, the default: only
 * the newest waits). 2 lets a frame that finishes a little late still be
 * shown, in order, instead of being replaced by the next one. A full queue
 * drops its oldest frame, which bounds the added latency. */
void ch_vk_gpu_mailbox_set_queue(ChVkGpuMailbox *,unsigned depth);
/* Source must contain current pixels, including any guest CPU edits.
 * VK_NOT_READY means all slots are held: keep the previously displayed frame. */
VkResult ch_vk_gpu_mailbox_publish(void *,const Nv2aSurface *,ChVkTarget *);
/* An immersive frame: eyes and HUD into a slot's shared buffer (bridge mode;
 * FEATURE_NOT_PRESENT otherwise). VK_NOT_READY as for publish. */
VkResult ch_vk_gpu_mailbox_publish_immersive(ChVkGpuMailbox *,const Nv2aSurface *,ChVkTarget *eyes,
    ChVkTarget *hud,const ChVkImmFrame *);
VkResult ch_vk_gpu_mailbox_take(ChVkGpuMailbox *,ChVkGpuFrame *);
/* As take, but only a frame whose copy has finished (bridge mode); VK_NOT_READY
 * leaves it waiting. Never blocks on the GPU. */
VkResult ch_vk_gpu_mailbox_take_ready(ChVkGpuMailbox *,ChVkGpuFrame *);
/* Finish consumer GPU use before release. Frames are move-only values. */
void ch_vk_gpu_frame_release(ChVkGpuFrame *);
/* Stop/join users first. Returns VK_NOT_READY while taken frames remain;
 * leaves mailbox intact for later release and retry. */
VkResult ch_vk_gpu_mailbox_destroy(ChVkGpuMailbox *);
#endif
