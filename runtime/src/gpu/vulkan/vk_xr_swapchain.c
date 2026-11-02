#include "vk_xr_swapchain.h"
#include <stdlib.h>
struct ChVkXrSwapchain {
    XrSwapchain handle;
    VkDevice device;
    XrSwapchainImageVulkan2KHR *images;
    uint32_t count,index,width,height;
    int acquired,waited,failed;
    PFN_xrDestroySwapchain destroy;
    PFN_xrAcquireSwapchainImage acquire;
    PFN_xrWaitSwapchainImage wait;
    PFN_xrReleaseSwapchainImage release;
};
XrSwapchain ch_vk_xr_swapchain_handle(const ChVkXrSwapchain *s) {
    return s?s->handle:XR_NULL_HANDLE;
}
XrResult ch_vk_xr_swapchain_destroy(ChVkXrSwapchain *s) {
    if(!s) return XR_SUCCESS;
    if(s->acquired) return XR_ERROR_CALL_ORDER_INVALID;
    if(s->handle) {
        XrResult r=s->destroy(s->handle);
        if(XR_FAILED(r)) return r;
    }
    free(s->images); free(s); return XR_SUCCESS;
}
XrResult ch_vk_xr_swapchain_create(XrInstance instance,XrSession session,
    PFN_xrGetInstanceProcAddr get_proc,VkDevice device,uint32_t width,uint32_t height,
    ChVkXrSwapchain **out) {
    if(!out) return XR_ERROR_VALIDATION_FAILURE;
    *out=NULL;
    if(!instance || !session || !device || !get_proc || !width || !height)
        return XR_ERROR_VALIDATION_FAILURE;
    ChVkXrSwapchain *s=calloc(1,sizeof(*s));
    if(!s) return XR_ERROR_OUT_OF_MEMORY;
    s->device=device; s->width=width; s->height=height;
    XrResult result;
    int64_t *formats=NULL;
    PFN_xrEnumerateSwapchainFormats enumerate_formats=NULL;
    PFN_xrCreateSwapchain create=NULL;
    PFN_xrEnumerateSwapchainImages enumerate_images=NULL;
#define TRY(expr) do { result=(expr); if(XR_FAILED(result)) goto fail; } while(0)
#define LOAD(name,fn) do { TRY(get_proc(instance,name,(PFN_xrVoidFunction *)&fn)); if(!fn) { result=XR_ERROR_FUNCTION_UNSUPPORTED; goto fail; } } while(0)
    LOAD("xrEnumerateSwapchainFormats",enumerate_formats);
    LOAD("xrCreateSwapchain",create);
    LOAD("xrEnumerateSwapchainImages",enumerate_images);
    LOAD("xrDestroySwapchain",s->destroy);
    LOAD("xrAcquireSwapchainImage",s->acquire);
    LOAD("xrWaitSwapchainImage",s->wait);
    LOAD("xrReleaseSwapchainImage",s->release);
    uint32_t count=0;
    TRY(enumerate_formats(session,0,&count,NULL));
    if(!count) { result=XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED; goto fail; }
    formats=calloc(count,sizeof(*formats));
    if(!formats) { result=XR_ERROR_OUT_OF_MEMORY; goto fail; }
    TRY(enumerate_formats(session,count,&count,formats));
    int supported=0;
    for(uint32_t i=0;i<count;i++) if(formats[i]==VK_FORMAT_R8G8B8A8_UNORM) supported=1;
    if(!supported) { result=XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED; goto fail; }
    XrSwapchainCreateInfo info={.type=XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.sampleCount=1,.width=width,.height=height,
        .faceCount=1,.arraySize=1,.mipCount=1};
    TRY(create(session,&info,&s->handle));
    TRY(enumerate_images(s->handle,0,&s->count,NULL));
    if(!s->count) { result=XR_ERROR_RUNTIME_FAILURE; goto fail; }
    s->images=calloc(s->count,sizeof(*s->images));
    if(!s->images) { result=XR_ERROR_OUT_OF_MEMORY; goto fail; }
    for(uint32_t i=0;i<s->count;i++) s->images[i].type=XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    TRY(enumerate_images(s->handle,s->count,&s->count,(XrSwapchainImageBaseHeader *)s->images));
    free(formats); *out=s; return XR_SUCCESS;
fail:
    free(formats);
    if(s->handle) s->destroy(s->handle);
    free(s->images); free(s); return result;
}
XrResult ch_vk_xr_swapchain_wait(ChVkXrSwapchain *s,XrDuration timeout,uint32_t *index,VkImage *image) {
    if(!s || !index || !image || timeout<0 || s->failed) return XR_ERROR_VALIDATION_FAILURE;
    if(s->waited) return XR_ERROR_CALL_ORDER_INVALID;
    if(!s->acquired) {
        XrSwapchainImageAcquireInfo acquire={.type=XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult r=s->acquire(s->handle,&acquire,&s->index);
        if(r!=XR_SUCCESS) return r;
        s->acquired=1;
        if(s->index>=s->count) { s->failed=1; return XR_ERROR_RUNTIME_FAILURE; }
    }
    XrSwapchainImageWaitInfo wait={.type=XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,.timeout=timeout};
    XrResult r=s->wait(s->handle,&wait);
    if(r!=XR_SUCCESS) return r; /* XR_TIMEOUT_EXPIRED is positive, not success. */
    s->waited=1; *index=s->index; *image=s->images[s->index].image;
    return XR_SUCCESS;
}
XrResult ch_vk_xr_swapchain_release(ChVkXrSwapchain *s,VkFence fence,VkResult *vk_result) {
    if(!s || !vk_result || !fence) return XR_ERROR_VALIDATION_FAILURE;
    if(!s->waited || s->failed) return XR_ERROR_CALL_ORDER_INVALID;
    *vk_result=vkGetFenceStatus(s->device,fence);
    if(*vk_result==VK_NOT_READY) return XR_TIMEOUT_EXPIRED;
    if(*vk_result!=VK_SUCCESS) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    XrSwapchainImageReleaseInfo release={.type=XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XrResult r=s->release(s->handle,&release);
    if(r==XR_SUCCESS) { s->acquired=0; s->waited=0; }
    return r;
}

XrResult ch_vk_xr_swapchain_copy_release(ChVkXrSwapchain *s,ChVkTarget *t,
    void *verify,VkDeviceSize bytes,VkResult *vk_result) {
    if(!s || !t || !vk_result) return XR_ERROR_VALIDATION_FAILURE;
    if(!s->waited || s->failed) return XR_ERROR_CALL_ORDER_INVALID;
    uint32_t width,height; ch_vk_target_size(t,&width,&height);
    if(width!=s->width || height!=s->height || ch_vk_target_device(t)!=s->device)
        return XR_ERROR_VALIDATION_FAILURE;
    /* Created XR swapchains do not request transfer-src usage. Verification of
     * this path must use a separate ordinary Vulkan image via target export. */
    if(verify) return XR_ERROR_FEATURE_UNSUPPORTED;
    *vk_result=ch_vk_target_export_image(t,s->images[s->index].image,NULL,bytes);
    if(*vk_result!=VK_SUCCESS) { s->failed=1; return XR_ERROR_GRAPHICS_DEVICE_INVALID; }
    return ch_vk_xr_swapchain_release(s,ch_vk_target_completion_fence(t),vk_result);
}

XrResult ch_vk_xr_swapchain_texture_release(ChVkXrSwapchain *s,ChVkTexture *texture,VkResult *vk_result) {
    if(!s || !texture || !vk_result) return XR_ERROR_VALIDATION_FAILURE;
    if(!s->waited || s->failed) return XR_ERROR_CALL_ORDER_INVALID;
    VkFence fence;
    *vk_result=ch_vk_texture_export_image(texture,s->device,s->images[s->index].image,s->width,s->height,&fence);
    if(*vk_result!=VK_SUCCESS) {s->failed=1;return XR_ERROR_GRAPHICS_DEVICE_INVALID;}
    return ch_vk_xr_swapchain_release(s,fence,vk_result);
}
