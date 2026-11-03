#include "vk_xr.h"
#include "vk_device_features.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void ch_vk_xr_device_destroy(ChVkXrDevice *d) {
    if(!d) return;
    if(d->device) { vkDeviceWaitIdle(d->device); vkDestroyDevice(d->device,NULL); }
    if(d->instance) vkDestroyInstance(d->instance,NULL);
    memset(d,0,sizeof(*d));
}
XrResult ch_vk_xr_device_create(XrInstance xr,XrSystemId system,
    PFN_xrGetInstanceProcAddr get_proc,ChVkXrDevice *out,VkResult *vk_result) {
    return ch_vk_xr_device_create_with_options(xr,system,get_proc,NULL,out,vk_result);
}
XrResult ch_vk_xr_device_create_with_options(XrInstance xr,XrSystemId system,
    PFN_xrGetInstanceProcAddr get_proc,const ChVkXrDeviceOptions *options,
    ChVkXrDevice *out,VkResult *vk_result) {
    if(!out || !vk_result || !get_proc || xr==XR_NULL_HANDLE || system==XR_NULL_SYSTEM_ID)
        return XR_ERROR_VALIDATION_FAILURE;
    *vk_result=VK_SUCCESS;
    if(out->instance || out->device) return XR_ERROR_CALL_ORDER_INVALID;
    ChVkXrDeviceOptions defaults={0};if(!options) options=&defaults;
    if(options->device_extension_count && !options->device_extensions) return XR_ERROR_VALIDATION_FAILURE;
    for(uint32_t i=0;i<options->device_extension_count;i++)
        if(!options->device_extensions[i] || !options->device_extensions[i][0]) return XR_ERROR_VALIDATION_FAILURE;
    uint32_t minimum_api=options->minimum_api_version?options->minimum_api_version:VK_API_VERSION_1_0;
    uint32_t preferred_api=options->preferred_api_version?options->preferred_api_version:VK_API_VERSION_1_1;
    if(VK_API_VERSION_VARIANT(minimum_api) || VK_API_VERSION_VARIANT(preferred_api) ||
       VK_VERSION_MAJOR(minimum_api)!=1 || VK_VERSION_MAJOR(preferred_api)!=1)
        return XR_ERROR_VALIDATION_FAILURE;
    ChVkXrDevice d={0};
    XrResult result;
    VkQueueFamilyProperties *families=NULL;
    PFN_xrGetVulkanGraphicsRequirements2KHR requirements=NULL;
    PFN_xrCreateVulkanInstanceKHR create_instance=NULL;
    PFN_xrGetVulkanGraphicsDevice2KHR get_device=NULL;
    PFN_xrCreateVulkanDeviceKHR create_device=NULL;
#define XR_TRY(expr) do { result=(expr); if(XR_FAILED(result)) goto fail; } while(0)
#define LOAD(name, dest) do { XR_TRY(get_proc(xr,name,(PFN_xrVoidFunction *)&dest)); if(!dest) { result=XR_ERROR_FUNCTION_UNSUPPORTED; goto fail; } } while(0)
    LOAD("xrGetVulkanGraphicsRequirements2KHR",requirements);
    LOAD("xrCreateVulkanInstanceKHR",create_instance);
    LOAD("xrGetVulkanGraphicsDevice2KHR",get_device);
    LOAD("xrCreateVulkanDeviceKHR",create_device);
    XrGraphicsRequirementsVulkan2KHR req={.type=XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    XR_TRY(requirements(xr,system,&req));
    uint32_t loader_version=VK_API_VERSION_1_0;
    PFN_vkEnumerateInstanceVersion enumerate_version=(PFN_vkEnumerateInstanceVersion)
        vkGetInstanceProcAddr(VK_NULL_HANDLE,"vkEnumerateInstanceVersion");
    if(enumerate_version) {
        *vk_result=enumerate_version(&loader_version);
        if(*vk_result!=VK_SUCCESS) { result=XR_ERROR_GRAPHICS_DEVICE_INVALID; goto fail; }
    }
    XrVersion loader=XR_MAKE_VERSION(VK_VERSION_MAJOR(loader_version),VK_VERSION_MINOR(loader_version),VK_VERSION_PATCH(loader_version));
    XrVersion minimum=XR_MAKE_VERSION(VK_VERSION_MAJOR(minimum_api),VK_VERSION_MINOR(minimum_api),VK_VERSION_PATCH(minimum_api));
    if(minimum<req.minApiVersionSupported) minimum=req.minApiVersionSupported;
    XrVersion version=XR_MAKE_VERSION(VK_VERSION_MAJOR(preferred_api),VK_VERSION_MINOR(preferred_api),VK_VERSION_PATCH(preferred_api));
    if(version<minimum) version=minimum;
    if(version>loader) version=loader;
    if(version>req.maxApiVersionSupported) version=req.maxApiVersionSupported;
    if(version<minimum || version>loader || version>req.maxApiVersionSupported || XR_VERSION_MAJOR(version)>1) {
        result=XR_ERROR_API_VERSION_UNSUPPORTED; goto fail;
    }
    d.api_version=VK_MAKE_VERSION(XR_VERSION_MAJOR(version),XR_VERSION_MINOR(version),XR_VERSION_PATCH(version));
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName="Chihiro Vulkan",.apiVersion=d.api_version};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    XrVulkanInstanceCreateInfoKHR xi={.type=XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR,
        .systemId=system,.pfnGetInstanceProcAddr=vkGetInstanceProcAddr,.vulkanCreateInfo=&ici};
    XR_TRY(create_instance(xr,&xi,&d.instance,vk_result));
    if(*vk_result!=VK_SUCCESS) { result=XR_ERROR_GRAPHICS_DEVICE_INVALID; goto fail; }
    XrVulkanGraphicsDeviceGetInfoKHR gi={.type=XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR,
        .systemId=system,.vulkanInstance=d.instance};
    XR_TRY(get_device(xr,&gi,&d.physical));
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(d.physical,&props);
    if(props.apiVersion<d.api_version) { result=XR_ERROR_GRAPHICS_DEVICE_INVALID; goto fail; }
    uint32_t count=0; vkGetPhysicalDeviceQueueFamilyProperties(d.physical,&count,NULL);
    if(!count) { result=XR_ERROR_GRAPHICS_DEVICE_INVALID; goto fail; }
    families=calloc(count,sizeof(*families));
    if(!families) { result=XR_ERROR_OUT_OF_MEMORY; goto fail; }
    vkGetPhysicalDeviceQueueFamilyProperties(d.physical,&count,families);
    d.family=UINT32_MAX;
    for(uint32_t i=0;i<count;i++) if(families[i].queueCount && (families[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)) { d.family=i; break; }
    if(d.family==UINT32_MAX) { result=XR_ERROR_GRAPHICS_DEVICE_INVALID; goto fail; }
    VkPhysicalDeviceFeatures features; vkGetPhysicalDeviceFeatures(d.physical,&features);
    d.precise_queries=!!features.occlusionQueryPrecise;
    d.raw_fixed=features.shaderClipDistance && features.depthClamp;
    features=(VkPhysicalDeviceFeatures){.occlusionQueryPrecise=features.occlusionQueryPrecise,
        .fillModeNonSolid=features.fillModeNonSolid,
        .shaderClipDistance=features.shaderClipDistance,.depthClamp=features.depthClamp};
    float priority=1;
    VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex=d.family,.queueCount=1,.pQueuePriorities=&priority};
    /* CHIHIRO_VULKAN_QUEUE_PRIORITY=high|realtime: the queue's system-wide
     * priority (VK_EXT_global_priority). The emulated GPU waits on many small
     * submissions each frame; behind a headset compositor's work on the same
     * GPU (SteamVR on a Steam Frame) each came back milliseconds late. A level
     * the system refuses falls back to the default. */
    const char *extensions[32];
    uint32_t extension_count=options->device_extension_count;
    VkDeviceQueueGlobalPriorityCreateInfoEXT global={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_EXT};
    {
        const char *level=getenv("CHIHIRO_VULKAN_QUEUE_PRIORITY");
        if(level && !strcmp(level,"high")) global.globalPriority=VK_QUEUE_GLOBAL_PRIORITY_HIGH_EXT;
        else if(level && !strcmp(level,"realtime")) global.globalPriority=VK_QUEUE_GLOBAL_PRIORITY_REALTIME_EXT;
        int available=0,external=0;
        if((global.globalPriority || options->external_memory_fd) && extension_count<31) {
            uint32_t n=0; vkEnumerateDeviceExtensionProperties(d.physical,NULL,&n,NULL);
            VkExtensionProperties *p=n?calloc(n,sizeof(*p)):NULL;
            if(p && vkEnumerateDeviceExtensionProperties(d.physical,NULL,&n,p)==VK_SUCCESS)
                for(uint32_t i=0;i<n;i++) {
                    if(!strcmp(p[i].extensionName,VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME)) available=1;
                    if(!strcmp(p[i].extensionName,VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) external=1;
                }
            free(p);
        }
        for(uint32_t i=0;i<extension_count && i<32;i++) extensions[i]=options->device_extensions[i];
        if(options->external_memory_fd && external) {
            extensions[extension_count++]=VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
            d.external_memory=1;
        }
        if(available && global.globalPriority) {
            extensions[extension_count++]=VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME;
            qci.pNext=&global;
        } else if(global.globalPriority) {
            fprintf(stderr,"[Vulkan] queue priority %s: VK_EXT_global_priority not offered\n",level);
            global.globalPriority=0;
        }
    }
    ChVkShaderFeatures shader_features;
    uint32_t base_extensions=extension_count;
    if(!ch_vk_shader_features(d.physical,&shader_features,extensions,&extension_count,32)) d.raw_fixed=0;
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&shader_features.ubo_layout,
        .queueCreateInfoCount=1,.pQueueCreateInfos=&qci,.pEnabledFeatures=&features,
        .enabledExtensionCount=extension_count,.ppEnabledExtensionNames=extensions};
    XrVulkanDeviceCreateInfoKHR xd={.type=XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR,
        .systemId=system,.pfnGetInstanceProcAddr=vkGetInstanceProcAddr,
        .vulkanPhysicalDevice=d.physical,.vulkanCreateInfo=&dci};
    XR_TRY(create_device(xr,&xd,&d.device,vk_result));
    if(*vk_result!=VK_SUCCESS && global.globalPriority) {
        fprintf(stderr,"[Vulkan] queue priority refused (VkResult %d): the default\n",(int)*vk_result);
        /* the same list without global priority (always appended last) */
        qci.pNext=NULL;
        if(extension_count>base_extensions) {
            uint32_t k=0;
            for(uint32_t i=0;i<extension_count;i++) if(strcmp(extensions[i],VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME)) extensions[k++]=extensions[i];
            extension_count=k;
        } else extension_count=options->device_extension_count+(d.external_memory?1:0);
        dci.enabledExtensionCount=extension_count;
        global.globalPriority=0;
        XR_TRY(create_device(xr,&xd,&d.device,vk_result));
    }
    if(*vk_result!=VK_SUCCESS) { result=XR_ERROR_GRAPHICS_DEVICE_INVALID; goto fail; }
    if(global.globalPriority)
        fprintf(stderr,"[Vulkan] queue priority %s\n",
            global.globalPriority==VK_QUEUE_GLOBAL_PRIORITY_REALTIME_EXT?"realtime":"high");
    vkGetDeviceQueue(d.device,d.family,0,&d.queue);
    d.binding=(XrGraphicsBindingVulkan2KHR){.type=XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
        .instance=d.instance,.physicalDevice=d.physical,.device=d.device,.queueFamilyIndex=d.family,.queueIndex=0};
    free(families); *out=d; return XR_SUCCESS;
fail:
    free(families); ch_vk_xr_device_destroy(&d); return result;
}
