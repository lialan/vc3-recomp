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
} ChVkXrBridge;
VkResult ch_vk_xr_bridge_create(VkPhysicalDevice physical, VkDevice xr, VkDevice game,
                                VkDeviceSize capacity, ChVkXrBridge *out);
void ch_vk_xr_bridge_destroy(ChVkXrBridge *b);
#endif
