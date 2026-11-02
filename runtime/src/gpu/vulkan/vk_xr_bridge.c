#include "vk_xr_bridge.h"
#include <string.h>
#include <unistd.h>

static VkResult make_buffer(VkDevice device, VkDeviceSize size, VkBuffer *out) {
    VkExternalMemoryBufferCreateInfo external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &external,
        .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    return vkCreateBuffer(device, &info, NULL, out);
}

static uint32_t memory_type(VkPhysicalDevice physical, uint32_t bits) {
    VkPhysicalDeviceMemoryProperties p;
    vkGetPhysicalDeviceMemoryProperties(physical, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; i++)   /* device-local first */
        if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            return i;
    for (uint32_t i = 0; i < p.memoryTypeCount; i++)
        if (bits & (1u << i))
            return i;
    return UINT32_MAX;
}

VkResult ch_vk_xr_bridge_create(VkPhysicalDevice physical, VkDevice xr, VkDevice game,
                                VkDeviceSize capacity, ChVkXrBridge *out) {
    if (!out || !xr || !game || !capacity) return VK_ERROR_INITIALIZATION_FAILED;
    memset(out, 0, sizeof *out);
    out->xr = xr; out->game = game; out->capacity = capacity;
    PFN_vkGetMemoryFdKHR get_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(xr, "vkGetMemoryFdKHR");
    if (!get_fd) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult r = VK_SUCCESS;
    for (unsigned i = 0; i < CH_VK_XR_BRIDGE_SLOTS && r == VK_SUCCESS; i++) {
        VkMemoryRequirements req, game_req;
        r = make_buffer(xr, capacity, &out->xr_buffer[i]);
        if (r != VK_SUCCESS) break;
        vkGetBufferMemoryRequirements(xr, out->xr_buffer[i], &req);
        r = make_buffer(game, capacity, &out->game_buffer[i]);
        if (r != VK_SUCCESS) break;
        vkGetBufferMemoryRequirements(game, out->game_buffer[i], &game_req);
        uint32_t type = memory_type(physical, req.memoryTypeBits & game_req.memoryTypeBits);
        if (type == UINT32_MAX) { r = VK_ERROR_FEATURE_NOT_PRESENT; break; }
        VkDeviceSize size = req.size > game_req.size ? req.size : game_req.size;
        VkExportMemoryAllocateInfo export = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
        VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &export,
            .allocationSize = size, .memoryTypeIndex = type};
        r = vkAllocateMemory(xr, &alloc, NULL, &out->xr_memory[i]);
        if (r != VK_SUCCESS) break;
        r = vkBindBufferMemory(xr, out->xr_buffer[i], out->xr_memory[i], 0);
        if (r != VK_SUCCESS) break;
        int fd = -1;
        VkMemoryGetFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .memory = out->xr_memory[i], .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
        r = get_fd(xr, &fd_info, &fd);
        if (r != VK_SUCCESS) break;
        /* the import owns the fd from here, whether or not it succeeds */
        VkImportMemoryFdInfoKHR import = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, .fd = fd};
        alloc.pNext = &import;
        r = vkAllocateMemory(game, &alloc, NULL, &out->game_memory[i]);
        if (r != VK_SUCCESS) { close(fd); break; }
        r = vkBindBufferMemory(game, out->game_buffer[i], out->game_memory[i], 0);
    }
    if (r != VK_SUCCESS) ch_vk_xr_bridge_destroy(out);
    return r;
}

void ch_vk_xr_bridge_destroy(ChVkXrBridge *b) {
    if (!b) return;
    for (unsigned i = 0; i < CH_VK_XR_BRIDGE_SLOTS; i++) {
        if (b->game_buffer[i]) vkDestroyBuffer(b->game, b->game_buffer[i], NULL);
        if (b->game_memory[i]) vkFreeMemory(b->game, b->game_memory[i], NULL);
        if (b->xr_buffer[i]) vkDestroyBuffer(b->xr, b->xr_buffer[i], NULL);
        if (b->xr_memory[i]) vkFreeMemory(b->xr, b->xr_memory[i], NULL);
    }
    memset(b, 0, sizeof *b);
}
