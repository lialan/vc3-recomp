#include "vk_xr_swapchain.h"
#include <stdio.h>
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
    /* buffer copies (ch_vk_xr_swapchain_buffer_release), made on first use */
    VkCommandPool pool; VkCommandBuffer command; VkFence fence;
    int copy_pending;   /* a buffer copy submitted, not yet waited for */
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
    if(s->fence) vkDestroyFence(s->device,s->fence,NULL);
    if(s->pool) vkDestroyCommandPool(s->device,s->pool,NULL);
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
    /* R8G8B8A8_UNORM (the Quest); else R8G8B8A8_SRGB (SteamVR offers no
     * 8-bit UNORM). Images only ever reach a swapchain by copy, which keeps
     * the bytes in either: already sRGB-encoded colours, which an SRGB
     * swapchain tells the compositor they are. */
    int has_unorm=0,has_srgb=0;
    for(uint32_t i=0;i<count;i++) {
        if(formats[i]==VK_FORMAT_R8G8B8A8_UNORM) has_unorm=1;
        if(formats[i]==VK_FORMAT_R8G8B8A8_SRGB) has_srgb=1;
    }
    if(!has_unorm && !has_srgb) {
        fprintf(stderr,"[Vulkan XR] no R8G8B8A8 UNORM/SRGB swapchains; the runtime offers:");
        for(uint32_t i=0;i<count;i++) fprintf(stderr," %lld",(long long)formats[i]);
        fputc('\n',stderr);
        result=XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED; goto fail;
    }
    {
        static int logged;
        if(!has_unorm && !logged++) fprintf(stderr,"[Vulkan XR] swapchains R8G8B8A8_SRGB (no UNORM offered)\n");
    }
    XrSwapchainCreateInfo info={.type=XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
        .format=has_unorm?VK_FORMAT_R8G8B8A8_UNORM:VK_FORMAT_R8G8B8A8_SRGB,.sampleCount=1,.width=width,.height=height,
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

/* The waited image filled from a buffer of packed RGBA8 rows (the bridge from
 * the game's own device), then released -- submitted, not waited for:
 * OpenXR only needs the work submitted to the session's queue before the
 * release, and SteamVR gates that queue once a display frame, so waiting
 * cost up to a display frame each time (the loop ran late; SteamVR
 * throttled it down to 20 Hz). The previous copy is waited for before its
 * command buffer is reused (ch_vk_xr_swapchain_settle: before its source
 * goes back to the game). */
XrResult ch_vk_xr_swapchain_settle(ChVkXrSwapchain *s) {
    if(!s || !s->copy_pending) return XR_SUCCESS;
    VkResult r=vkWaitForFences(s->device,1,&s->fence,VK_TRUE,5000000000ULL);
    s->copy_pending=0;
    if(r!=VK_SUCCESS) {s->failed=1;return XR_ERROR_GRAPHICS_DEVICE_INVALID;}
    return XR_SUCCESS;
}
XrResult ch_vk_xr_swapchain_buffer_release(ChVkXrSwapchain *s,VkQueue queue,uint32_t family,
    VkBuffer buffer,uint32_t width,uint32_t height,VkResult *vk_result) {
    if(s && !width && !height) {width=s->width;height=s->height;}   /* 0x0: the swapchain's own */
    if(!s || !queue || !buffer || !vk_result || width!=s->width || height!=s->height) return XR_ERROR_VALIDATION_FAILURE;
    if(!s->waited || s->failed) return XR_ERROR_CALL_ORDER_INVALID;
    VkResult r=VK_SUCCESS;
    if(!s->pool) {
        VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};
        r=vkCreateCommandPool(s->device,&pi,NULL,&s->pool);
        VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool=s->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
        if(r==VK_SUCCESS) r=vkAllocateCommandBuffers(s->device,&ai,&s->command);
        VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if(r==VK_SUCCESS) r=vkCreateFence(s->device,&fi,NULL,&s->fence);
        if(r!=VK_SUCCESS) goto fail;
    }
    if(XR_FAILED(ch_vk_xr_swapchain_settle(s))) {r=VK_ERROR_DEVICE_LOST;goto fail;}
    VkImage image=s->images[s->index].image;
    if((r=vkResetCommandBuffer(s->command,0))!=VK_SUCCESS || (r=vkResetFences(s->device,1,&s->fence))!=VK_SUCCESS) goto fail;
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if((r=vkBeginCommandBuffer(s->command,&begin))!=VK_SUCCESS) goto fail;
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(s->command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={width,height,1}};
    vkCmdCopyBufferToImage(s->command,buffer,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(s->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,0,0,NULL,0,NULL,1,&barrier);
    if((r=vkEndCommandBuffer(s->command))!=VK_SUCCESS) goto fail;
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&s->command};
    if((r=vkQueueSubmit(queue,1,&submit,s->fence))!=VK_SUCCESS) goto fail;
    s->copy_pending=1;
    {
        XrSwapchainImageReleaseInfo release={.type=XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        XrResult xr=s->release(s->handle,&release);
        if(xr==XR_SUCCESS) {s->acquired=0;s->waited=0;}
        *vk_result=VK_SUCCESS;
        return xr;
    }
fail:
    *vk_result=r;s->failed=1;return XR_ERROR_GRAPHICS_DEVICE_INVALID;
}
XrResult ch_vk_xr_swapchain_record_buffer(ChVkXrSwapchain *s,VkCommandBuffer command,VkBuffer buffer,VkDeviceSize offset) {
    if(!s || !command || !buffer) return XR_ERROR_VALIDATION_FAILURE;
    if(!s->waited || s->failed) return XR_ERROR_CALL_ORDER_INVALID;
    VkImage image=s->images[s->index].image;
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkBufferImageCopy copy={.bufferOffset=offset,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={s->width,s->height,1}};
    vkCmdCopyBufferToImage(command,buffer,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,0,0,NULL,0,NULL,1,&barrier);
    return XR_SUCCESS;
}
XrResult ch_vk_xr_swapchain_record_image(ChVkXrSwapchain *s,VkCommandBuffer command,VkImage source,uint32_t family) {
    if(!s || !command || !source) return XR_ERROR_VALIDATION_FAILURE;
    if(!s->waited || s->failed) return XR_ERROR_CALL_ORDER_INVALID;
    VkImage image=s->images[s->index].image;
    /* the source acquired from the device that copied into it
     * (ch_vk_target_record_copy_to_image), the swapchain image's old
     * contents not kept */
    VkImageMemoryBarrier barriers[2]={{.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL,
        .dstQueueFamilyIndex=family,.image=source,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}},
        {.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}}};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,2,barriers);
    VkImageCopy copy={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.extent={s->width,s->height,1}};
    vkCmdCopyImage(command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    VkImageMemoryBarrier done=barriers[1];
    done.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    done.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    done.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;done.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,0,0,NULL,0,NULL,1,&done);
    return XR_SUCCESS;
}
XrResult ch_vk_xr_swapchain_release_submitted(ChVkXrSwapchain *s) {
    if(!s) return XR_ERROR_VALIDATION_FAILURE;
    if(!s->waited || s->failed) return XR_ERROR_CALL_ORDER_INVALID;
    XrSwapchainImageReleaseInfo release={.type=XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XrResult r=s->release(s->handle,&release);
    if(r==XR_SUCCESS) {s->acquired=0;s->waited=0;}
    return r;
}
XrResult ch_vk_xr_swapchain_texture_release(ChVkXrSwapchain *s,ChVkTexture *texture,VkResult *vk_result) {
    if(!s || !texture || !vk_result) return XR_ERROR_VALIDATION_FAILURE;
    if(!s->waited || s->failed) return XR_ERROR_CALL_ORDER_INVALID;
    VkFence fence;
    *vk_result=ch_vk_texture_export_image(texture,s->device,s->images[s->index].image,s->width,s->height,&fence);
    if(*vk_result!=VK_SUCCESS) {s->failed=1;return XR_ERROR_GRAPHICS_DEVICE_INVALID;}
    return ch_vk_xr_swapchain_release(s,fence,vk_result);
}
