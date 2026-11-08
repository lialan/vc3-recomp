#include "vk_xr_bridge.h"
#include "vk_target.h"
#include <stdio.h>
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
    /* Some drivers (the Quest's) export opaque-fd memory only as a dedicated
     * allocation, bound to the one buffer on each side. */
    VkPhysicalDeviceExternalBufferInfo query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkExternalBufferProperties props = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vkGetPhysicalDeviceExternalBufferProperties(physical, &query, &props);
    VkExternalMemoryFeatureFlags features = props.externalMemoryProperties.externalMemoryFeatures;
    if (!(features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)
        || !(features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
        fprintf(stderr, "[xr bridge] opaque-fd buffers not exportable and importable (features 0x%x)\n",
                (unsigned)features);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    int dedicated = (features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
    VkResult r = VK_SUCCESS;
    const char *step = "buffers";
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
        VkMemoryDedicatedAllocateInfo xr_dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .buffer = out->xr_buffer[i]};
        VkExportMemoryAllocateInfo export = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
            .pNext = dedicated ? &xr_dedicated : NULL, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
        VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &export,
            .allocationSize = size, .memoryTypeIndex = type};
        step = "export allocation";
        r = vkAllocateMemory(xr, &alloc, NULL, &out->xr_memory[i]);
        if (r != VK_SUCCESS) break;
        r = vkBindBufferMemory(xr, out->xr_buffer[i], out->xr_memory[i], 0);
        if (r != VK_SUCCESS) break;
        int fd = -1;
        VkMemoryGetFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .memory = out->xr_memory[i], .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
        step = "fd export";
        r = get_fd(xr, &fd_info, &fd);
        if (r != VK_SUCCESS) break;
        /* the import owns the fd from here, whether or not it succeeds */
        VkMemoryDedicatedAllocateInfo game_dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .buffer = out->game_buffer[i]};
        VkImportMemoryFdInfoKHR import = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
            .pNext = dedicated ? &game_dedicated : NULL,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, .fd = fd};
        alloc.pNext = &import;
        step = "import";
        r = vkAllocateMemory(game, &alloc, NULL, &out->game_memory[i]);
        if (r != VK_SUCCESS) { close(fd); break; }
        step = "import bind";
        r = vkBindBufferMemory(game, out->game_buffer[i], out->game_memory[i], 0);
    }
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[xr bridge] %s failed: %d (features 0x%x%s)\n", step, (int)r, (unsigned)features,
                dedicated ? ", dedicated only" : "");
        ch_vk_xr_bridge_destroy(out);
    }
    return r;
}

/* The shared images' creation (identical on both devices, as an import
 * requires). Color attachment and sampled usage as well as transfer: the
 * driver's compressed layout is kept for such images, and the copies in and
 * out stay compressed. */
static VkImageCreateInfo image_info(uint32_t width, uint32_t height, const void *next) {
    return (VkImageCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = next,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {width, height, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
}
static void destroy_images(ChVkXrBridge *b) {
    for (unsigned i = 0; i < CH_VK_XR_BRIDGE_SLOTS; i++) {
        if (b->game_image[i]) vkDestroyImage(b->game, b->game_image[i], NULL);
        if (b->game_image_memory[i]) vkFreeMemory(b->game, b->game_image_memory[i], NULL);
        if (b->xr_image[i]) vkDestroyImage(b->xr, b->xr_image[i], NULL);
        if (b->xr_image_memory[i]) vkFreeMemory(b->xr, b->xr_image_memory[i], NULL);
        b->game_image[i] = b->xr_image[i] = VK_NULL_HANDLE;
        b->game_image_memory[i] = b->xr_image_memory[i] = VK_NULL_HANDLE;
    }
    b->image_w = b->image_h = 0;
}
VkResult ch_vk_xr_bridge_create_images(ChVkXrBridge *b, VkPhysicalDevice physical, uint32_t width, uint32_t height) {
    if (!b || !b->xr || !b->game || !width || !height) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetMemoryFdKHR get_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(b->xr, "vkGetMemoryFdKHR");
    if (!get_fd) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkImageCreateInfo probe = image_info(width, height, NULL);
    VkPhysicalDeviceExternalImageFormatInfo ext_query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkPhysicalDeviceImageFormatInfo2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &ext_query, .format = probe.format, .type = probe.imageType, .tiling = probe.tiling, .usage = probe.usage};
    VkExternalImageFormatProperties ext_props = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &ext_props};
    VkResult r = vkGetPhysicalDeviceImageFormatProperties2(physical, &query, &props);
    VkExternalMemoryFeatureFlags features = ext_props.externalMemoryProperties.externalMemoryFeatures;
    if (r != VK_SUCCESS || !(features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)
        || !(features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
        fprintf(stderr, "[xr bridge] opaque-fd images not offered (%d, features 0x%x): the buffers only\n",
                (int)r, (unsigned)features);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    const int dedicated = (features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
    VkExternalMemoryImageCreateInfo external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    const VkImageCreateInfo info = image_info(width, height, &external);
    const char *step = "images";
    for (unsigned i = 0; i < CH_VK_XR_BRIDGE_SLOTS && r == VK_SUCCESS; i++) {
        r = vkCreateImage(b->xr, &info, NULL, &b->xr_image[i]);
        if (r != VK_SUCCESS) break;
        r = vkCreateImage(b->game, &info, NULL, &b->game_image[i]);
        if (r != VK_SUCCESS) break;
        VkMemoryRequirements req, game_req;
        vkGetImageMemoryRequirements(b->xr, b->xr_image[i], &req);
        vkGetImageMemoryRequirements(b->game, b->game_image[i], &game_req);
        uint32_t type = memory_type(physical, req.memoryTypeBits & game_req.memoryTypeBits);
        if (type == UINT32_MAX) { r = VK_ERROR_FEATURE_NOT_PRESENT; break; }
        VkMemoryDedicatedAllocateInfo xr_dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .image = b->xr_image[i]};
        VkExportMemoryAllocateInfo export = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
            .pNext = dedicated ? &xr_dedicated : NULL, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
        VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &export,
            .allocationSize = req.size > game_req.size ? req.size : game_req.size, .memoryTypeIndex = type};
        step = "image export allocation";
        r = vkAllocateMemory(b->xr, &alloc, NULL, &b->xr_image_memory[i]);
        if (r != VK_SUCCESS) break;
        r = vkBindImageMemory(b->xr, b->xr_image[i], b->xr_image_memory[i], 0);
        if (r != VK_SUCCESS) break;
        int fd = -1;
        VkMemoryGetFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .memory = b->xr_image_memory[i], .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
        step = "image fd export";
        r = get_fd(b->xr, &fd_info, &fd);
        if (r != VK_SUCCESS) break;
        VkMemoryDedicatedAllocateInfo game_dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .image = b->game_image[i]};
        VkImportMemoryFdInfoKHR import = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
            .pNext = dedicated ? &game_dedicated : NULL,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, .fd = fd};
        alloc.pNext = &import;
        step = "image import";
        r = vkAllocateMemory(b->game, &alloc, NULL, &b->game_image_memory[i]);
        if (r != VK_SUCCESS) { close(fd); break; }
        step = "image import bind";
        r = vkBindImageMemory(b->game, b->game_image[i], b->game_image_memory[i], 0);
    }
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[xr bridge] %s failed: %d: the buffers only\n", step, (int)r);
        destroy_images(b);
        return r;
    }
    b->image_w = width; b->image_h = height;
    fprintf(stderr, "[xr bridge] shared images %ux%u%s\n", width, height, dedicated ? " (dedicated)" : "");
    return VK_SUCCESS;
}

VkResult ch_vk_xr_bridge_import_image(ChVkXrBridge *b, const ChVkTargetExport *memory,
                                      VkImage *image, VkDeviceMemory *imported) {
    if (!image || !imported) return VK_ERROR_INITIALIZATION_FAILED;
    *image = VK_NULL_HANDLE; *imported = VK_NULL_HANDLE;
    if (!b || !b->xr || !memory || memory->fd < 0) {
        if (memory && memory->fd >= 0) close(memory->fd);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkExternalMemoryImageCreateInfo external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkImageCreateInfo info = image_info(memory->width, memory->height, &external);
    info.usage = memory->usage;
    VkResult r = vkCreateImage(b->xr, &info, NULL, image);
    if (r == VK_SUCCESS) {   /* the same image on the same GPU: the same needs */
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(b->xr, *image, &req);
        if (!(req.memoryTypeBits & (1u << memory->memory_type)) || req.size > memory->size)
            r = VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (r == VK_SUCCESS) {
        VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .image = *image};
        VkImportMemoryFdInfoKHR import = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
            .pNext = memory->dedicated ? &dedicated : NULL,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, .fd = memory->fd};
        VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import,
            .allocationSize = memory->size, .memoryTypeIndex = memory->memory_type};
        r = vkAllocateMemory(b->xr, &alloc, NULL, imported);
        if (r == VK_SUCCESS) r = vkBindImageMemory(b->xr, *image, *imported, 0);   /* the memory owns the fd */
        else close(memory->fd);
    } else close(memory->fd);
    if (r != VK_SUCCESS) {
        if (*image) vkDestroyImage(b->xr, *image, NULL);
        if (*imported) vkFreeMemory(b->xr, *imported, NULL);
        *image = VK_NULL_HANDLE; *imported = VK_NULL_HANDLE;
    }
    return r;
}

void ch_vk_xr_bridge_destroy(ChVkXrBridge *b) {
    if (!b) return;
    destroy_images(b);
    for (unsigned i = 0; i < CH_VK_XR_BRIDGE_SLOTS; i++) {
        if (b->game_buffer[i]) vkDestroyBuffer(b->game, b->game_buffer[i], NULL);
        if (b->game_memory[i]) vkFreeMemory(b->game, b->game_memory[i], NULL);
        if (b->xr_buffer[i]) vkDestroyBuffer(b->xr, b->xr_buffer[i], NULL);
        if (b->xr_memory[i]) vkFreeMemory(b->xr, b->xr_memory[i], NULL);
    }
    memset(b, 0, sizeof *b);
}
