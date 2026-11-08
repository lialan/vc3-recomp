#ifndef CH_VK_XR_BRIDGE_H
#define CH_VK_XR_BRIDGE_H
#include <vulkan/vulkan.h>
/* Buffers two VkDevices on one GPU both see: allocated on the headset's
 * device (the one the OpenXR runtime shares a queue with), exported as an
 * opaque fd, imported on the game's device. The game renders on a device of
 * its own because SteamVR (Steam Frame) puts its frame waits on the queue it
 * is given: on a shared queue every game submission waited for the next
 * display frame, and the emulated GPU's many small waits each frame cost a
 * display period apiece (30 fps at 120 Hz, worse at 90). Both devices need
 * VK_KHR_external_memory_fd. */
#define CH_VK_XR_BRIDGE_SLOTS 4 /* one per GPU mailbox slot */
typedef struct {
    VkDevice xr, game;
    VkDeviceSize capacity;
    VkBuffer xr_buffer[CH_VK_XR_BRIDGE_SLOTS], game_buffer[CH_VK_XR_BRIDGE_SLOTS];
    VkDeviceMemory xr_memory[CH_VK_XR_BRIDGE_SLOTS], game_memory[CH_VK_XR_BRIDGE_SLOTS];
    /* ch_vk_xr_bridge_create_images: an RGBA8 image per slot, image_w x
     * image_h, the same memory on both devices (none: image_w 0). The eyes
     * cross in them image to image: through the buffers both copies went to
     * and from linear memory -- 2.5 + 1.8 ms of GPU a frame on the Quest,
     * against ~0.8 ms for one image-to-image copy. */
    VkImage xr_image[CH_VK_XR_BRIDGE_SLOTS], game_image[CH_VK_XR_BRIDGE_SLOTS];
    VkDeviceMemory xr_image_memory[CH_VK_XR_BRIDGE_SLOTS], game_image_memory[CH_VK_XR_BRIDGE_SLOTS];
    uint32_t image_w, image_h;
} ChVkXrBridge;
VkResult ch_vk_xr_bridge_create(VkPhysicalDevice physical, VkDevice xr, VkDevice game,
                                VkDeviceSize capacity, ChVkXrBridge *out);
/* The shared images (after create); on failure none, the buffers kept. */
VkResult ch_vk_xr_bridge_create_images(ChVkXrBridge *b, VkPhysicalDevice physical, uint32_t width, uint32_t height);
/* Zero copy: an image of the game's device (ch_vk_target_export: its eye
 * target's) on the headset's -- made as the game made it, its memory
 * imported; the fd consumed either way. */
struct ChVkTargetExport;
VkResult ch_vk_xr_bridge_import_image(ChVkXrBridge *b, const struct ChVkTargetExport *memory,
                                      VkImage *image, VkDeviceMemory *imported);
void ch_vk_xr_bridge_destroy(ChVkXrBridge *b);
#endif
