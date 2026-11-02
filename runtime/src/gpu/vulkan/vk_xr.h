#ifndef CHIHIRO_VK_XR_H
#define CHIHIRO_VK_XR_H
#include <vulkan/vulkan.h>
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
typedef struct {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family, api_version;
    int precise_queries;
    int raw_fixed;
    int external_memory;   /* VK_KHR_external_memory_fd enabled (options) */
    XrGraphicsBindingVulkan2KHR binding;
} ChVkXrDevice;
typedef struct {
    /* Zero preserves defaults (minimum1.0, preferred1.1). A Zink bridge
     * requests minimum/preferred1.2 and the two external-FD extensions. */
    uint32_t minimum_api_version,preferred_api_version;
    uint32_t device_extension_count;
    const char *const *device_extensions;
    /* Nonzero: also VK_KHR_external_memory_fd when the device has it (the
     * game's own device shares buffers with this one); external_memory says. */
    int external_memory_fd;
} ChVkXrDeviceOptions;
/* Caller creates XrInstance with XR_KHR_vulkan_enable2 enabled. This function
 * queries graphics requirements before creating any Vulkan objects and delegates
 * instance/device creation and GPU selection to the OpenXR runtime. No session
 * or swapchain created here. out must be empty; ownership transfers on success.
 * Both XrResult and VkResult are preserved for meaningful failure diagnostics. */
XrResult ch_vk_xr_device_create(XrInstance xr, XrSystemId system,
    PFN_xrGetInstanceProcAddr get_proc, ChVkXrDevice *out, VkResult *vk_result);
XrResult ch_vk_xr_device_create_with_options(XrInstance xr,XrSystemId system,
    PFN_xrGetInstanceProcAddr get_proc,const ChVkXrDeviceOptions *options,
    ChVkXrDevice *out,VkResult *vk_result);
/* Destroy all associated XR sessions/swapchains and renderer resources first. */
void ch_vk_xr_device_destroy(ChVkXrDevice *device);
#endif
