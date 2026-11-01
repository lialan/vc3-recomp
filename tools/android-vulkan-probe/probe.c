/* Separate NativeActivity probe: no game files, settings, audio or GL. */
#include <jni.h>
#include <android/native_activity.h>
#include <android/log.h>
#define XR_USE_PLATFORM_ANDROID
#include "vk_xr.h"
#include "vk_xr_swapchain.h"
#include <string.h>
#define LOG(...) __android_log_print(ANDROID_LOG_INFO,"ChVkProbe",__VA_ARGS__)
static void resume(ANativeActivity *activity) {
    activity->callbacks->onResume=NULL;
    XrInstance instance=XR_NULL_HANDLE; XrSession session=XR_NULL_HANDLE;
    ChVkXrDevice device={0}; ChVkXrSwapchain *swapchain=NULL;
    XrResult result; VkResult vk_result=VK_SUCCESS;
    const char *stage="loader initialize";
#define XR(call) do { result=(call); if(XR_FAILED(result)) { LOG("FAIL stage=%s xr=%d vk=%d",stage,result,vk_result); goto done; } } while(0)
    PFN_xrInitializeLoaderKHR initialize=NULL;
    XR(xrGetInstanceProcAddr(XR_NULL_HANDLE,"xrInitializeLoaderKHR",(PFN_xrVoidFunction *)&initialize));
    if(!initialize) goto done;
    XrLoaderInitInfoAndroidKHR init={.type=XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR,
        .applicationVM=activity->vm,.applicationContext=activity->clazz};
    XR(initialize((const XrLoaderInitInfoBaseHeaderKHR *)&init));
    stage="instance";
    const char *extensions[]={XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    XrInstanceCreateInfoAndroidKHR android={.type=XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR,
        .applicationVM=activity->vm,.applicationActivity=activity->clazz};
    XrInstanceCreateInfo info={.type=XR_TYPE_INSTANCE_CREATE_INFO,.next=&android,
        .enabledExtensionCount=2,.enabledExtensionNames=extensions};
    strcpy(info.applicationInfo.applicationName,"Chihiro Vulkan probe");
    info.applicationInfo.apiVersion=XR_CURRENT_API_VERSION;
    XR(xrCreateInstance(&info,&instance));
    stage="HMD system";
    XrSystemGetInfo system_info={.type=XR_TYPE_SYSTEM_GET_INFO,.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    XrSystemId system; XR(xrGetSystem(instance,&system_info,&system));
    stage="runtime Vulkan device";
    XR(ch_vk_xr_device_create(instance,system,xrGetInstanceProcAddr,&device,&vk_result));
    stage="Vulkan session";
    XrSessionCreateInfo session_info={.type=XR_TYPE_SESSION_CREATE_INFO,.next=&device.binding,.systemId=system};
    XR(xrCreateSession(instance,&session_info,&session));
    stage="Vulkan swapchain";
    XR(ch_vk_xr_swapchain_create(instance,session,xrGetInstanceProcAddr,device.device,64,64,&swapchain));
    LOG("PASS real OpenXR enable2 runtime-selected Vulkan device, session and swapchain creation; no frames rendered");
done:
    if(swapchain) ch_vk_xr_swapchain_destroy(swapchain);
    if(session) xrDestroySession(session);
    ch_vk_xr_device_destroy(&device);
    if(instance) xrDestroyInstance(instance);
    ANativeActivity_finish(activity);
}
void ANativeActivity_onCreate(ANativeActivity *activity,void *saved,size_t bytes) {
    (void)saved; (void)bytes;
    activity->callbacks->onResume=resume;
}
