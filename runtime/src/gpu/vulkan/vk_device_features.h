#ifndef CH_VK_DEVICE_FEATURES_H
#define CH_VK_DEVICE_FEATURES_H
/* Device features the shared renderer's shaders rely on, for every place a
 * VkDevice is created. The GPU-transform vertex shaders read their per-draw
 * state (matrices, lights, vertex-program constants) from a std430 uniform
 * buffer: as a uniform buffer the compiler can load it into constant
 * registers once a draw (Adreno: ir3 pushes UBO ranges to the const file),
 * where a storage buffer is a memory load per vertex. std430 keeps the C
 * layout; uniform blocks need VK_KHR_uniform_buffer_standard_layout for it. */
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
static inline int ch_vk_device_has_extension(VkPhysicalDevice physical,const char *name) {
    uint32_t n=0;int found=0;
    if(vkEnumerateDeviceExtensionProperties(physical,NULL,&n,NULL)!=VK_SUCCESS || !n) return 0;
    VkExtensionProperties *p=calloc(n,sizeof *p);
    if(p && vkEnumerateDeviceExtensionProperties(physical,NULL,&n,p)==VK_SUCCESS)
        for(uint32_t i=0;i<n && !found;i++) found=!strcmp(p[i].extensionName,name);
    free(p);return found;
}
typedef struct {
    VkPhysicalDeviceUniformBufferStandardLayoutFeaturesKHR ubo_layout;
    int ubo_layout_supported;
} ChVkShaderFeatures;
/* Fills features (chain features->ubo_layout into VkDeviceCreateInfo.pNext)
 * and appends the extension to names[*count] when supported; returns whether
 * the GPU-transform shaders can run. */
static inline int ch_vk_shader_features(VkPhysicalDevice physical,ChVkShaderFeatures *features,
    const char **names,uint32_t *count,uint32_t capacity) {
    memset(features,0,sizeof *features);
    features->ubo_layout.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES_KHR;
    if(*count<capacity && ch_vk_device_has_extension(physical,VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME)) {
        names[(*count)++]=VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME;
        features->ubo_layout.uniformBufferStandardLayout=VK_TRUE;
        features->ubo_layout_supported=1;
    }
    return features->ubo_layout_supported;
}
#endif
