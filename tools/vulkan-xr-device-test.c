/* Injected XR API contract test using real Vulkan; no XR runtime/session. */
#include "vk_xr.h"
#include "vk_xr_swapchain.h"
#include "vk_xr_frame.h"
#include "vk_xr_quad.h"
#include "vk_xr_screen.h"
#include <string.h>
#include <stdio.h>
static int step,mode,bad;
static VkImage real_image;
static XrResult XRAPI_PTR req(XrInstance x,XrSystemId s,XrGraphicsRequirementsVulkanKHR *r) {
    (void)x;(void)s; if(step++!=0) bad=1;
    r->minApiVersionSupported=XR_MAKE_VERSION(1,0,0);
    r->maxApiVersionSupported=mode==3?XR_MAKE_VERSION(1,2,0):XR_MAKE_VERSION(1,1,0);
    return mode==1?XR_ERROR_RUNTIME_FAILURE:XR_SUCCESS;
}
static XrResult XRAPI_PTR instance(XrInstance x,const XrVulkanInstanceCreateInfoKHR *i,VkInstance *v,VkResult *r) {
    (void)x; if(step++!=1) bad=1;
    if(mode==3 && i->vulkanCreateInfo->pApplicationInfo->apiVersion!=VK_API_VERSION_1_2) bad=1;
    if(mode==2) { *r=VK_ERROR_INITIALIZATION_FAILED; return XR_SUCCESS; }
    *r=vkCreateInstance(i->vulkanCreateInfo,NULL,v); return XR_SUCCESS;
}
static XrResult XRAPI_PTR physical(XrInstance x,const XrVulkanGraphicsDeviceGetInfoKHR *i,VkPhysicalDevice *v) {
    (void)x; if(step++!=2) bad=1;
    uint32_t n=1; return vkEnumeratePhysicalDevices(i->vulkanInstance,&n,v)==VK_SUCCESS?XR_SUCCESS:XR_ERROR_RUNTIME_FAILURE;
}
static XrResult XRAPI_PTR device(XrInstance x,const XrVulkanDeviceCreateInfoKHR *i,VkDevice *v,VkResult *r) {
    (void)x; if(step++!=3) bad=1;
    if(mode==3 && (i->vulkanCreateInfo->enabledExtensionCount!=2 ||
       strcmp(i->vulkanCreateInfo->ppEnabledExtensionNames[0],VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) ||
       strcmp(i->vulkanCreateInfo->ppEnabledExtensionNames[1],VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME))) bad=1;
    *r=vkCreateDevice(i->vulkanPhysicalDevice,i->vulkanCreateInfo,NULL,v); return XR_SUCCESS;
}
static unsigned acquires,waits,releases,destroys;
static XrResult XRAPI_PTR formats(XrSession s,uint32_t cap,uint32_t *n,int64_t *f) {
    (void)s; *n=1; if(cap) f[0]=VK_FORMAT_R8G8B8A8_UNORM; return XR_SUCCESS;
}
static XrResult XRAPI_PTR swapcreate(XrSession s,const XrSwapchainCreateInfo *i,XrSwapchain *out) {
    (void)s; if(i->format!=VK_FORMAT_R8G8B8A8_UNORM || i->arraySize!=1) bad=1;
    *out=(XrSwapchain)(uintptr_t)2; return XR_SUCCESS;
}
static XrResult XRAPI_PTR images(XrSwapchain s,uint32_t cap,uint32_t *n,XrSwapchainImageBaseHeader *out) {
    (void)s; *n=2;
    if(cap) for(unsigned i=0;i<2;i++) {
        XrSwapchainImageVulkan2KHR *v=(XrSwapchainImageVulkan2KHR *)out;
        if(v[i].type!=XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR) bad=1;
        v[i].image=i==1?real_image:(VkImage)(uintptr_t)10;
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_PTR acquire(XrSwapchain s,const XrSwapchainImageAcquireInfo *i,uint32_t *index) {
    (void)s;(void)i; acquires++; *index=1; return XR_SUCCESS;
}
static XrResult XRAPI_PTR waitimage(XrSwapchain s,const XrSwapchainImageWaitInfo *i) {
    (void)s;(void)i; return waits++==0?XR_TIMEOUT_EXPIRED:XR_SUCCESS;
}
static ChVkXrDevice *capture_device;
static const unsigned char *capture_expected;
static uint32_t capture_width,capture_height,capture_count;
/* Read the actual injected XR destination image after the presenter's fence.
 * This fixture image has TRANSFER_SRC usage; real XR chains deliberately do not. */
static int capture_matches(void) {
    ChVkXrDevice *d=capture_device;
    VkBuffer buffer=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE;
    VkCommandPool pool=VK_NULL_HANDLE; VkFence fence=VK_NULL_HANDLE; int failed=1;
    VkDeviceSize bytes=(VkDeviceSize)capture_width*capture_height*4;
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if(vkCreateBuffer(d->device,&bi,NULL,&buffer)!=VK_SUCCESS) goto done;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(d->device,buffer,&req);
    VkPhysicalDeviceMemoryProperties props; vkGetPhysicalDeviceMemoryProperties(d->physical,&props);
    uint32_t type=0;
    while(type<props.memoryTypeCount && (!(req.memoryTypeBits&(1u<<type)) ||
        !(props.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))) type++;
    if(type==props.memoryTypeCount) goto done;
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
    if(vkAllocateMemory(d->device,&ai,NULL,&memory)!=VK_SUCCESS || vkBindBufferMemory(d->device,buffer,memory,0)!=VK_SUCCESS) goto done;
    VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=d->family};
    if(vkCreateCommandPool(d->device,&pi,NULL,&pool)!=VK_SUCCESS) goto done;
    VkCommandBuffer cmd; VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    if(vkAllocateCommandBuffers(d->device,&ca,&cmd)!=VK_SUCCESS) goto done;
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if(vkBeginCommandBuffer(cmd,&begin)!=VK_SUCCESS) goto done;
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=real_image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={capture_width,capture_height,1}};
    vkCmdCopyImageToBuffer(cmd,real_image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT; barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT;
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkBufferMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_HOST_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=buffer,.size=VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&host,0,NULL);
    if(vkEndCommandBuffer(cmd)!=VK_SUCCESS) goto done;
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if(vkCreateFence(d->device,&fi,NULL,&fence)!=VK_SUCCESS) goto done;
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
    if(vkQueueSubmit(d->queue,1,&submit,fence)!=VK_SUCCESS || vkWaitForFences(d->device,1,&fence,VK_TRUE,5000000000ULL)!=VK_SUCCESS) goto done;
    void *mapped;
    if(vkMapMemory(d->device,memory,0,VK_WHOLE_SIZE,0,&mapped)!=VK_SUCCESS) goto done;
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=memory,.size=VK_WHOLE_SIZE};
    if(vkInvalidateMappedMemoryRanges(d->device,1,&range)==VK_SUCCESS) failed=memcmp(mapped,capture_expected,(size_t)bytes)!=0;
    vkUnmapMemory(d->device,memory);
done:
    if(fence) vkDestroyFence(d->device,fence,NULL);
    if(pool) vkDestroyCommandPool(d->device,pool,NULL);
    if(buffer) vkDestroyBuffer(d->device,buffer,NULL);
    if(memory) vkFreeMemory(d->device,memory,NULL);
    return !failed;
}
static XrResult XRAPI_PTR releaseimage(XrSwapchain s,const XrSwapchainImageReleaseInfo *i) {
    (void)s;(void)i; releases++;
    if(capture_expected) { if(!capture_matches()) bad=1; capture_count++; }
    return XR_SUCCESS;
}
static XrResult XRAPI_PTR swapdestroy(XrSwapchain s) { (void)s; destroys++; return XR_SUCCESS; }
static int frame_mode,frame_phase;
static XrResult XRAPI_PTR viewconfig(XrInstance i,XrSystemId s,XrViewConfigurationType type,uint32_t cap,uint32_t *n,XrViewConfigurationView *v) {
    (void)i;(void)s;(void)type;*n=2;
    if(cap) for(unsigned k=0;k<2;k++) { v[k].recommendedImageRectWidth=16; v[k].recommendedImageRectHeight=16; }
    return XR_SUCCESS;
}
static XrResult XRAPI_PTR waitframe(XrSession s,const XrFrameWaitInfo *i,XrFrameState *state) {
    (void)s;(void)i; if(frame_phase++!=0) bad=1;
    state->predictedDisplayTime=123456; state->shouldRender=frame_mode!=0; return XR_SUCCESS;
}
static XrResult XRAPI_PTR beginframe(XrSession s,const XrFrameBeginInfo *i) {
    (void)s;(void)i; if(frame_phase++!=1) bad=1; return XR_SUCCESS;
}
static XrResult XRAPI_PTR locateviews(XrSession s,const XrViewLocateInfo *i,XrViewState *state,uint32_t cap,uint32_t *n,XrView *views) {
    (void)s;(void)cap; if(frame_phase++!=2 || i->displayTime!=123456) bad=1;
    *n=2; state->viewStateFlags=frame_mode>=2?(XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT):0;
    for(unsigned k=0;k<2;k++) { views[k].pose.orientation.w=1; views[k].pose.position.x=k?.03f:-.03f; views[k].fov.angleLeft=-.7f; views[k].fov.angleRight=.7f; views[k].fov.angleUp=.7f; views[k].fov.angleDown=-.7f; }
    return XR_SUCCESS;
}
static XrResult XRAPI_PTR endframe(XrSession s,const XrFrameEndInfo *i) {
    (void)s;
    if(frame_phase++!=(frame_mode?3:2) || i->displayTime!=123456 || i->layerCount!=(frame_mode==3?2u:frame_mode>=2?1u:0u)) bad=1;
    if(i->layerCount && frame_mode<4) {
        const XrCompositionLayerProjection *p=(const XrCompositionLayerProjection *)i->layers[0];
        if(p->viewCount!=2 || p->views[0].pose.position.x!=-.03f || p->views[1].pose.position.x!=.03f || p->views[0].subImage.imageRect.extent.width!=16) bad=1;
    }
    if(frame_mode>=3) {
        unsigned index=frame_mode==3?1:0;
        const XrCompositionLayerQuad *quad=(const XrCompositionLayerQuad *)i->layers[index];
        if(quad->type!=XR_TYPE_COMPOSITION_LAYER_QUAD || quad->size.width!=1.2f ||
           quad->pose.position.z!=-2 || (frame_mode==5?quad->layerFlags!=0:!(quad->layerFlags&XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT))) bad=1;
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_PTR proc(XrInstance x,const char *name,PFN_xrVoidFunction *f) {
    (void)x;
    if(!strcmp(name,"xrGetVulkanGraphicsRequirements2KHR")) *f=(PFN_xrVoidFunction)req;
    else if(!strcmp(name,"xrCreateVulkanInstanceKHR")) *f=(PFN_xrVoidFunction)instance;
    else if(!strcmp(name,"xrGetVulkanGraphicsDevice2KHR")) *f=(PFN_xrVoidFunction)physical;
    else if(!strcmp(name,"xrCreateVulkanDeviceKHR")) *f=(PFN_xrVoidFunction)device;
    else if(!strcmp(name,"xrEnumerateSwapchainFormats")) *f=(PFN_xrVoidFunction)formats;
    else if(!strcmp(name,"xrCreateSwapchain")) *f=(PFN_xrVoidFunction)swapcreate;
    else if(!strcmp(name,"xrEnumerateSwapchainImages")) *f=(PFN_xrVoidFunction)images;
    else if(!strcmp(name,"xrAcquireSwapchainImage")) *f=(PFN_xrVoidFunction)acquire;
    else if(!strcmp(name,"xrWaitSwapchainImage")) *f=(PFN_xrVoidFunction)waitimage;
    else if(!strcmp(name,"xrReleaseSwapchainImage")) *f=(PFN_xrVoidFunction)releaseimage;
    else if(!strcmp(name,"xrDestroySwapchain")) *f=(PFN_xrVoidFunction)swapdestroy;
    else if(!strcmp(name,"xrEnumerateViewConfigurationViews")) *f=(PFN_xrVoidFunction)viewconfig;
    else if(!strcmp(name,"xrWaitFrame")) *f=(PFN_xrVoidFunction)waitframe;
    else if(!strcmp(name,"xrBeginFrame")) *f=(PFN_xrVoidFunction)beginframe;
    else if(!strcmp(name,"xrLocateViews")) *f=(PFN_xrVoidFunction)locateviews;
    else if(!strcmp(name,"xrEndFrame")) *f=(PFN_xrVoidFunction)endframe;
    else return XR_ERROR_FUNCTION_UNSUPPORTED;
    return XR_SUCCESS;
}
int main(void) {
    const char *extensions[]={VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
    ChVkXrDeviceOptions options={.minimum_api_version=VK_API_VERSION_1_2,.preferred_api_version=VK_API_VERSION_1_2,
        .device_extension_count=2,.device_extensions=extensions};
    ChVkXrDevice zink={0};VkResult vk_result=VK_SUCCESS;step=0;mode=0;
    if(ch_vk_xr_device_create_with_options((XrInstance)(uintptr_t)1,1,proc,&options,&zink,&vk_result)!=XR_ERROR_API_VERSION_UNSUPPORTED ||
       step!=1 || zink.instance || zink.device) return 1;
    mode=3;step=0;
    if(ch_vk_xr_device_create_with_options((XrInstance)(uintptr_t)1,1,proc,&options,&zink,&vk_result)!=XR_SUCCESS ||
       vk_result!=VK_SUCCESS || zink.api_version!=VK_API_VERSION_1_2 || step!=4 || bad ||
       !vkGetDeviceProcAddr(zink.device,"vkGetMemoryFdKHR") || !vkGetDeviceProcAddr(zink.device,"vkImportSemaphoreFdKHR")) return 1;
    ch_vk_xr_device_destroy(&zink);
    puts("PASS: XR factory forwards Vulkan1.2/external-FD requests to runtime-selected GPU; rejects1.1 ceiling before instance creation (injected XR)");
    for(mode=0;mode<3;mode++) {
        ChVkXrDevice d={0}; VkResult vr=VK_SUCCESS; step=0;
        XrResult r=ch_vk_xr_device_create((XrInstance)(uintptr_t)1,1,proc,&d,&vr);
        if(mode==0) {
            if(r!=XR_SUCCESS || vr!=VK_SUCCESS || step!=4 || !d.queue ||
               d.binding.device!=d.device || d.binding.physicalDevice!=d.physical) bad=1;
            VkImageCreateInfo image_info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,.extent={16,16,1},
                .mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
                .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
            if(vkCreateImage(d.device,&image_info,NULL,&real_image)!=VK_SUCCESS) return 1;
            VkMemoryRequirements memory_req; vkGetImageMemoryRequirements(d.device,real_image,&memory_req);
            uint32_t memory_index=0; while(!(memory_req.memoryTypeBits&(1u<<memory_index))) memory_index++;
            VkMemoryAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize=memory_req.size,.memoryTypeIndex=memory_index};
            VkDeviceMemory memory;
            if(vkAllocateMemory(d.device,&alloc,NULL,&memory)!=VK_SUCCESS ||
               vkBindImageMemory(d.device,real_image,memory,0)!=VK_SUCCESS) return 1;
            ChVkXrSwapchain *sc=NULL;
            if(ch_vk_xr_swapchain_create((XrInstance)(uintptr_t)1,(XrSession)(uintptr_t)1,proc,d.device,16,16,&sc)!=XR_SUCCESS) bad=1;
            uint32_t index=99; VkImage image=VK_NULL_HANDLE;
            if(ch_vk_xr_swapchain_wait(sc,0,&index,&image)!=XR_TIMEOUT_EXPIRED || index!=99 || image) bad=1;
            if(ch_vk_xr_swapchain_destroy(sc)!=XR_ERROR_CALL_ORDER_INVALID) bad=1;
            if(ch_vk_xr_swapchain_wait(sc,1000,&index,&image)!=XR_SUCCESS || index!=1 || image!=real_image || acquires!=1) bad=1;
            if(ch_vk_xr_swapchain_wait(sc,0,&index,&image)!=XR_ERROR_CALL_ORDER_INVALID) bad=1;
            VkFence fence=VK_NULL_HANDLE;
            VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            if(vkCreateFence(d.device,&fi,NULL,&fence)!=VK_SUCCESS) bad=1;
            if(ch_vk_xr_swapchain_release(sc,fence,&vr)!=XR_TIMEOUT_EXPIRED || vr!=VK_NOT_READY || releases) bad=1;
            vkDestroyFence(d.device,fence,NULL);
            fi.flags=VK_FENCE_CREATE_SIGNALED_BIT;
            if(vkCreateFence(d.device,&fi,NULL,&fence)!=VK_SUCCESS) bad=1;
            if(ch_vk_xr_swapchain_release(sc,fence,&vr)!=XR_SUCCESS || releases!=1) bad=1;
            vkDestroyFence(d.device,fence,NULL);
            if(ch_vk_xr_swapchain_wait(sc,1000,&index,&image)!=XR_SUCCESS) bad=1;
            ChVkTarget *target=NULL;
            if(ch_vk_target_create(d.physical,d.device,d.queue,d.family,16,16,&target)!=VK_SUCCESS) return 1;
            unsigned char source[16*16*4],copied[sizeof(source)];
            const float rgba[]={17/255.f,85/255.f,170/255.f,238/255.f};
            if(ch_vk_target_clear_read(target,rgba,source,sizeof(source))!=VK_SUCCESS ||
               ch_vk_target_export_image(target,image,copied,sizeof(copied))!=VK_SUCCESS ||
               memcmp(source,copied,sizeof(source))) bad=1;
            if(ch_vk_xr_swapchain_copy_release(sc,target,NULL,0,&vr)!=XR_SUCCESS || releases!=2) bad=1;
            ch_vk_target_destroy(target);
            if(ch_vk_xr_swapchain_destroy(sc)!=XR_SUCCESS || destroys!=1) bad=1;
            ChVkXrFrame *presenter=NULL;
            if(ch_vk_xr_frame_create((XrInstance)(uintptr_t)1,(XrSession)(uintptr_t)1,1,(XrSpace)(uintptr_t)1,proc,d.device,&presenter)!=XR_SUCCESS) bad=1;
            ChVkTarget *eyes[2]={0};
            for(unsigned k=0;k<2;k++) {
                if(ch_vk_target_create(d.physical,d.device,d.queue,d.family,16,16,&eyes[k])!=VK_SUCCESS) return 1;
                if(ch_vk_target_clear_read(eyes[k],rgba,source,sizeof(source))!=VK_SUCCESS) return 1;
            }
            ChVkXrQuad *quad=NULL;
            XrPosef pose={.orientation={0,0,0,1},.position={0,0,-2}};
            XrExtent2Df size={1.2f,.8f};
            if(ch_vk_xr_quad_create((XrInstance)(uintptr_t)1,(XrSession)(uintptr_t)1,proc,d.device,16,16,
                (XrSpace)(uintptr_t)1,&pose,size,XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,&quad)!=XR_SUCCESS) bad=1;
            if(ch_vk_xr_quad_layer(quad)) bad=1;
            waits=0;
            if(ch_vk_xr_quad_update(quad,eyes[0],0,&vr)!=XR_TIMEOUT_EXPIRED || ch_vk_xr_quad_layer(quad)) bad=1;
            if(ch_vk_xr_quad_pose(quad,&pose,size)!=XR_ERROR_CALL_ORDER_INVALID || ch_vk_xr_quad_destroy(quad)!=XR_ERROR_CALL_ORDER_INVALID) bad=1;
            if(ch_vk_xr_quad_update(quad,eyes[0],1000,&vr)!=XR_SUCCESS || !ch_vk_xr_quad_layer(quad)) bad=1;
            const XrCompositionLayerBaseHeader *extra[]={ch_vk_xr_quad_layer(quad)};
            for(frame_mode=0;frame_mode<5;frame_mode++) {
                frame_phase=0; XrFrameState fs; XrView views[2]; XrViewStateFlags flags;
                if(ch_vk_xr_frame_begin(presenter,&fs,views,&flags)!=XR_SUCCESS) bad=1;
                if(ch_vk_xr_frame_begin(presenter,&fs,views,&flags)!=XR_ERROR_CALL_ORDER_INVALID) bad=1;
                if(frame_mode==2) {
                    waits=0; unsigned previous=acquires;
                    if(ch_vk_xr_frame_submit(presenter,eyes,0,&vr)!=XR_TIMEOUT_EXPIRED) bad=1;
                    if(ch_vk_xr_frame_destroy(presenter)!=XR_ERROR_CALL_ORDER_INVALID) bad=1;
                    if(ch_vk_xr_frame_submit(presenter,eyes,1000,&vr)!=XR_SUCCESS || acquires!=previous+2) bad=1;
                } else if(frame_mode==3) {
                    if(ch_vk_xr_frame_submit_layers(presenter,eyes,1000,extra,1,&vr)!=XR_SUCCESS) bad=1;
                } else if(frame_mode==4) {
                    if(ch_vk_xr_frame_submit_flat(presenter,extra,1,&vr)!=XR_SUCCESS) bad=1;
                } else if(ch_vk_xr_frame_submit(presenter,NULL,0,&vr)!=XR_SUCCESS) bad=1;
                if(ch_vk_xr_frame_submit(presenter,eyes,0,&vr)!=XR_ERROR_CALL_ORDER_INVALID) bad=1;
            }
            ChVkFrameMailbox *mailbox=NULL; ChVkXrScreen *screen=NULL;
            if(ch_vk_frame_mailbox_create(&mailbox)!=VK_SUCCESS ||
               ch_vk_xr_screen_create((XrInstance)(uintptr_t)1,(XrSession)(uintptr_t)1,(XrSpace)(uintptr_t)1,
                   proc,d.physical,d.device,d.queue,d.family,mailbox,&screen)!=XR_SUCCESS) return 1;
            uint64_t sequence=99;
            if(ch_vk_xr_screen_update(screen,&pose,size,0,&vr,&sequence)!=XR_SUCCESS || sequence || ch_vk_xr_screen_layer(screen)) bad=1;
            Nv2aSurface surface={.width=16,.height=16,.pitch=64,.bytes_per_pixel=4};
            unsigned char first[1024],next[1024];
            for(unsigned k=0;k<sizeof first;k++) { first[k]=(unsigned char)(k*13+17); next[k]=(unsigned char)(k*7+19); }
            if(ch_vk_frame_mailbox_publish(mailbox,&surface,first,sizeof first)!=VK_SUCCESS) bad=1;
            waits=0; unsigned prior=acquires;
            if(ch_vk_xr_screen_update(screen,&pose,size,0,&vr,&sequence)!=XR_TIMEOUT_EXPIRED || sequence || ch_vk_xr_screen_layer(screen)) bad=1;
            if(ch_vk_xr_screen_destroy(screen)!=XR_ERROR_CALL_ORDER_INVALID) bad=1;
            if(ch_vk_frame_mailbox_publish(mailbox,&surface,next,sizeof next)!=VK_SUCCESS) bad=1;
            capture_device=&d; capture_expected=first; capture_width=capture_height=16;
            if(ch_vk_xr_screen_update(screen,&pose,size,1000,&vr,&sequence)!=XR_SUCCESS || sequence!=1 || acquires!=prior+1) bad=1;
            const XrCompositionLayerBaseHeader *screen_layers[]={ch_vk_xr_screen_layer(screen)};
            frame_mode=5; frame_phase=0; XrFrameState fs; XrView views[2]; XrViewStateFlags flags;
            if(ch_vk_xr_frame_begin(presenter,&fs,views,&flags)!=XR_SUCCESS ||
               ch_vk_xr_frame_submit_flat(presenter,screen_layers,1,&vr)!=XR_SUCCESS) bad=1;
            capture_expected=next;
            if(ch_vk_xr_screen_update(screen,&pose,size,1000,&vr,&sequence)!=XR_SUCCESS || sequence!=2) bad=1;
            prior=acquires;
            if(ch_vk_xr_screen_update(screen,&pose,size,0,&vr,&sequence)!=XR_SUCCESS || sequence!=2 || acquires!=prior) bad=1;
            surface.width=surface.height=8; surface.pitch=32;
            if(ch_vk_frame_mailbox_publish(mailbox,&surface,first,256)!=VK_SUCCESS) bad=1;
            capture_expected=first; capture_width=capture_height=8;
            if(ch_vk_xr_screen_update(screen,&pose,size,1000,&vr,&sequence)!=XR_SUCCESS || sequence!=3) bad=1;
            const XrCompositionLayerQuad *screen_layer=(const XrCompositionLayerQuad *)ch_vk_xr_screen_layer(screen);
            if(!screen_layer || screen_layer->subImage.imageRect.extent.width!=8 || capture_count!=3) bad=1;
            capture_expected=NULL;
            ChVkCpuFrame stereo={.surface=surface,.bytes=256,.rgba=first,.immersive=1,.view={.on=1},
                .eyes={{8,8,256,first},{8,8,256,next}}};
            if(ch_vk_frame_mailbox_publish_frame(mailbox,&stereo)!=VK_SUCCESS ||
               ch_vk_xr_screen_update(screen,&pose,size,1000,&vr,&sequence)!=XR_ERROR_LAYER_INVALID || ch_vk_xr_screen_layer(screen)) bad=1;
            if(ch_vk_xr_screen_destroy(screen)!=XR_SUCCESS) bad=1;
            ch_vk_frame_mailbox_destroy(mailbox);
            if(!bad) puts("PASS: mailbox -> Vulkan target -> XR screen destination pixels, timeout holds sequence, newer frame waits, resize and opaque flat composition (injected XR)");
            if(ch_vk_xr_quad_destroy(quad)!=XR_SUCCESS) bad=1;
            if(ch_vk_xr_frame_destroy(presenter)!=XR_SUCCESS) bad=1;
            for(unsigned k=0;k<2;k++) ch_vk_target_destroy(eyes[k]);
            vkDestroyImage(d.device,real_image,NULL); vkFreeMemory(d.device,memory,NULL);
        } else if(r>=0 || d.instance || d.device || step!=(mode==1?1:2)) bad=1;
        if(mode==2 && vr!=VK_ERROR_INITIALIZATION_FAILED) bad=1;
        ch_vk_xr_device_destroy(&d);
        if(d.instance || d.device) bad=1;
    }
    if(bad) return 1;
    puts("PASS: injected OpenXR enable2 call order, real Vulkan creation, XR/Vk failure propagation and cleanup, swapchain timeout retry and fence-gated ownership (real image copy/readback, injected XR ownership, quad/flat composition, no XR runtime)");
    return 0;
}
