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
     * HUD (surface size, premultiplied) when imm.hud, then from
     * imm.lens_offset the lens picture when imm.lens. */
    int immersive;
    ChVkImmFrame imm;
    VkDeviceSize hud_offset;
    /* The eyes in the slot's shared image instead of its buffer
     * (ch_vk_gpu_mailbox_set_images; released to VK_QUEUE_FAMILY_EXTERNAL
     * in TRANSFER_SRC_OPTIMAL). */
    int eyes_image;
    /* Zero copy: the eyes in the eye target's own image, which the
     * headset's device shares (ch_vk_target_create_exportable): read it only
     * as ch_vk_gpu_frame_eyes_begin/eyes_reading allow, until eyes_done. */
    int eyes_shared;
    /* Bridge mode: the copy into the slot's buffer is asynchronous; wait on
     * this fence (of ready_device) before reading the buffer. */
    VkDevice ready_device;
    VkFence ready;
    /* Shown from this time on (ch_vk_gpu_mailbox_set_pacing's clock; 0: at
     * once): the vblank its flip completes at, as a console shows it. */
    uint64_t due_ns;
    /* Due -- its due time, else a game frame's time (60 Hz) after the frame
     * before it was taken -- but its copy (so the game's GPU work before it)
     * unfinished at a take_ready: the headset showed the frame before it once
     * more because of the GPU. */
    int gpu_late;
} ChVkGpuFrame;
/* Present pacing: due() gives a frame published now the time it is due
 * (0: at once), now() the same clock; take_ready waits for it. */
void ch_vk_gpu_mailbox_set_pacing(ChVkGpuMailbox *,uint64_t (*due)(void),uint64_t (*now)(void));
/* Wait for a taken frame's copy (bridge mode); no-op otherwise. */
VkResult ch_vk_gpu_frame_wait(const ChVkGpuFrame *);
#define CH_VK_GPU_MAILBOX_SLOTS 4
#define CH_VK_GPU_MAILBOX_SHARES 8   /* eye targets handed over as they are, at most */
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
/* Bridge mode: an RGBA8 image per slot (width x height, the other device's
 * memory too) that an immersive frame's eyes of that size go to instead of
 * the slot's buffer (ChVkGpuFrame.eyes_image); NULL: none. */
void ch_vk_gpu_mailbox_set_images(ChVkGpuMailbox *,const VkImage images[CH_VK_GPU_MAILBOX_SLOTS],uint32_t width,uint32_t height);
/* How many finished frames may wait, taken oldest first (1, the default: only
 * the newest waits). 2 lets a frame that finishes a little late still be
 * shown, in order, instead of being replaced by the next one. A full queue
 * drops its oldest frame once a newer one has finished, which bounds the
 * added latency; until then a publish waits for the GPU (bridge mode), so a
 * game ahead of the GPU never has every waiting frame replaced unfinished. */
void ch_vk_gpu_mailbox_set_queue(ChVkGpuMailbox *,unsigned depth);
/* Source must contain current pixels, including any guest CPU edits.
 * VK_NOT_READY means all slots are held: keep the previously displayed frame. */
VkResult ch_vk_gpu_mailbox_publish(void *,const Nv2aSurface *,ChVkTarget *);
/* An immersive frame: eyes, HUD and lens picture (NULL: none; as info->lens)
 * into a slot's shared buffer (bridge mode; FEATURE_NOT_PRESENT otherwise);
 * exportable eyes handed over as they are instead (eyes_shared).
 * A lens picture that does not fit the slot is left out (imm.lens 0).
 * VK_NOT_READY as for publish. */
VkResult ch_vk_gpu_mailbox_publish_immersive(ChVkGpuMailbox *,const Nv2aSurface *,ChVkTarget *eyes,
    ChVkTarget *hud,ChVkTarget *lens,const ChVkImmFrame *);
VkResult ch_vk_gpu_mailbox_take(ChVkGpuMailbox *,ChVkGpuFrame *);
/* As take, but only a frame whose copy has finished (bridge mode); VK_NOT_READY
 * leaves it waiting. Never blocks on the GPU. */
VkResult ch_vk_gpu_mailbox_take_ready(ChVkGpuMailbox *,ChVkGpuFrame *);
/* Frames taken late for the GPU (gpu_late) so far, over all mailboxes: what
 * dynamic resolution answers. A frame the game itself publishes late (loads,
 * compiles, 30 fps scenes) is not waiting at those takes and does not count. */
uint64_t ch_vk_gpu_mailbox_gpu_late_count(void);
/* Finish consumer GPU use before release. Frames are move-only values. */
void ch_vk_gpu_frame_release(ChVkGpuFrame *);
/* Zero copy (eyes_shared), the headset's side of a taken frame. The game
 * renders into the eye target again once no frame holds its eyes: each one
 * holds them until released, or until eyes_done after its read; one not
 * being read gives them up after the game waited 100 ms (the headset not
 * taking or reading frames). begin: 1 while it holds them, with the eye
 * target's share (0..CH_VK_GPU_MAILBOX_SHARES-1, the same target's each
 * frame) and id (changed: a new target there; import again); 0: none to
 * read. memory: the share's memory for the import (vk_xr_bridge.h; a new
 * fd, the caller's), 0: none. reading, just before the read is recorded
 * and submitted: its fence (device's, unsignalled until the read is done,
 * and not reset before eyes_done), which the game waits for (and sees done
 * without waiting for eyes_done); 0: not to be read after all (given up
 * meanwhile). done: the read complete. The read acquires the image from
 * VK_QUEUE_FAMILY_EXTERNAL and releases it back to it, as
 * TRANSFER_SRC_OPTIMAL. */
int ch_vk_gpu_frame_eyes_begin(const ChVkGpuFrame *,unsigned *share,uint64_t *id);
int ch_vk_gpu_frame_eyes_memory(const ChVkGpuFrame *,ChVkTargetExport *);
int ch_vk_gpu_frame_eyes_reading(const ChVkGpuFrame *,VkDevice device,VkFence fence);
void ch_vk_gpu_frame_eyes_done(const ChVkGpuFrame *);
/* The headset cannot import the eye targets: their eyes copied from now on. */
void ch_vk_gpu_mailbox_copy_eyes(ChVkGpuMailbox *);
/* Stop/join users first. Returns VK_NOT_READY while taken frames remain;
 * leaves mailbox intact for later release and retry. */
VkResult ch_vk_gpu_mailbox_destroy(ChVkGpuMailbox *);
#endif
