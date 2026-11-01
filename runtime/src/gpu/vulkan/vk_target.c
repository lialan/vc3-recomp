#include "vk_target.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include "color_spv.h"
typedef struct {
    VkPipelineColorBlendAttachmentState blend;
    float constants[4];
    VkPipelineDepthStencilStateCreateInfo depth;
    VkCullModeFlags cull;
    VkFrontFace front;
    unsigned shader;
} PipelineKey;
struct ChVkTarget {
    uint32_t family;
    VkPhysicalDevice physical;
    VkRenderPass pass;
    VkImageView view;
    VkFramebuffer framebuffer;
    const ChVkTexture *const *stage_images;
    ChVkTexture *dummy;
    const Nv2aCombiner *rc;
    VkDeviceSize vertex_offset;
    VkDescriptorSetLayout descriptor_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor_set;
    int textured;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkPipelineCache pipeline_cache;
    PipelineKey pipeline_key;
    int pipeline_key_valid;
    VkDevice device;
    VkQueue queue;
    VkImage depth;
    VkFormat depth_format;
    VkDeviceMemory depth_mem;
    VkImageView depth_view;
    VkImage image;
    VkDeviceMemory image_mem, read_mem;
    VkBuffer read;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkFence fence;
    uint32_t width, height;
    VkDeviceSize bytes;
    VkImageLayout layout;
    VkQueryPool queries,timing;
    float timestamp_period;
    uint32_t timestamp_bits;
    uint64_t timing_start,timing_gpu,timing_cpu,timing_calls;
    int count_requested;
    uint64_t samples;
    struct { uint32_t alpha[3]; ChVkEffect effect; } push;
    int effect;
    int defer_color_readback;
    int initialized;
    int failed;
};
#define TRY(expr) do { result=(expr); if(result!=VK_SUCCESS) goto fail; } while(0)
static uint64_t time_ns(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;
}
static int memory_type(VkPhysicalDevice gpu, uint32_t bits,
                       VkMemoryPropertyFlags flags, uint32_t *index) {
    VkPhysicalDeviceMemoryProperties p;
    vkGetPhysicalDeviceMemoryProperties(gpu, &p);
    for(uint32_t i=0;i<p.memoryTypeCount;i++)
        if((bits & (1u<<i)) && (p.memoryTypes[i].propertyFlags & flags)==flags) {
            *index=i; return 1;
        }
    return 0;
}
void ch_vk_target_destroy(ChVkTarget *t) {
    if(!t) return;
    /* Also covers an error after submission: never free pending resources. */
    if(t->failed) vkQueueWaitIdle(t->queue);
    if(t->pipeline) vkDestroyPipeline(t->device,t->pipeline,NULL);
    if(t->pipeline_cache) vkDestroyPipelineCache(t->device,t->pipeline_cache,NULL);
    if(t->descriptor_pool) vkDestroyDescriptorPool(t->device,t->descriptor_pool,NULL);
    if(t->descriptor_layout) vkDestroyDescriptorSetLayout(t->device,t->descriptor_layout,NULL);
    if(t->pipeline_layout) vkDestroyPipelineLayout(t->device,t->pipeline_layout,NULL);
    if(t->framebuffer) vkDestroyFramebuffer(t->device,t->framebuffer,NULL);
    if(t->view) vkDestroyImageView(t->device,t->view,NULL);
    if(t->pass) vkDestroyRenderPass(t->device,t->pass,NULL);
    ch_vk_texture_destroy(t->dummy);
    if(t->queries) vkDestroyQueryPool(t->device,t->queries,NULL);
    if(t->timing) vkDestroyQueryPool(t->device,t->timing,NULL);
    if(t->fence) vkDestroyFence(t->device,t->fence,NULL);
    if(t->pool) vkDestroyCommandPool(t->device,t->pool,NULL);
    if(t->read) vkDestroyBuffer(t->device,t->read,NULL);
    if(t->read_mem) vkFreeMemory(t->device,t->read_mem,NULL);
    if(t->depth_view) vkDestroyImageView(t->device,t->depth_view,NULL);
    if(t->depth) vkDestroyImage(t->device,t->depth,NULL);
    if(t->depth_mem) vkFreeMemory(t->device,t->depth_mem,NULL);
    if(t->image) vkDestroyImage(t->device,t->image,NULL);
    if(t->image_mem) vkFreeMemory(t->device,t->image_mem,NULL);
    free(t);
}
VkResult ch_vk_target_create(VkPhysicalDevice physical, VkDevice device,
                            VkQueue queue, uint32_t family,
                            uint32_t width, uint32_t height, ChVkTarget **out) {
    return ch_vk_target_create_depth(physical,device,queue,family,width,height,
        VK_FORMAT_D32_SFLOAT_S8_UINT,out);
}
VkResult ch_vk_target_create_depth(VkPhysicalDevice physical, VkDevice device,
    VkQueue queue, uint32_t family, uint32_t width, uint32_t height,
    VkFormat depth_format, ChVkTarget **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;
    if(depth_format!=VK_FORMAT_D32_SFLOAT_S8_UINT && depth_format!=VK_FORMAT_D24_UNORM_S8_UINT)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if(!width || !height || (uint64_t)width*height>SIZE_MAX/5)
        return VK_ERROR_INITIALIZATION_FAILED;
    ChVkTarget *t=calloc(1,sizeof(*t));
    if(!t) return VK_ERROR_OUT_OF_HOST_MEMORY;
    t->family=family; t->physical=physical; t->device=device; t->queue=queue; t->width=width; t->height=height;
    t->depth_format=depth_format;
    t->bytes=(VkDeviceSize)width*height*4;
    VkResult result;
    VkImageCreateInfo image={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
        .extent={width,height,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    TRY(vkCreateImage(device,&image,NULL,&t->image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device,t->image,&req);
    VkMemoryAllocateInfo mem={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=req.size};
    if(!memory_type(physical,req.memoryTypeBits,0,&mem.memoryTypeIndex)) {
        result=VK_ERROR_FEATURE_NOT_PRESENT; goto fail;
    }
    TRY(vkAllocateMemory(device,&mem,NULL,&t->image_mem));
    TRY(vkBindImageMemory(device,t->image,t->image_mem,0));
    VkFormatProperties depth_properties;
    vkGetPhysicalDeviceFormatProperties(physical,depth_format,&depth_properties);
    if(!(depth_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        result=VK_ERROR_FORMAT_NOT_SUPPORTED; goto fail;
    }
    image.format=depth_format;
    image.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    TRY(vkCreateImage(device,&image,NULL,&t->depth));
    vkGetImageMemoryRequirements(device,t->depth,&req);
    mem.allocationSize=req.size;
    if(!memory_type(physical,req.memoryTypeBits,0,&mem.memoryTypeIndex)) {
        result=VK_ERROR_FEATURE_NOT_PRESENT; goto fail;
    }
    TRY(vkAllocateMemory(device,&mem,NULL,&t->depth_mem));
    TRY(vkBindImageMemory(device,t->depth,t->depth_mem,0));
    VkImageViewCreateInfo depth_view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=t->depth,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=image.format,
        .subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1}};
    TRY(vkCreateImageView(device,&depth_view,NULL,&t->depth_view));
    VkBufferCreateInfo buffer={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=t->bytes+t->bytes/4,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    TRY(vkCreateBuffer(device,&buffer,NULL,&t->read));
    vkGetBufferMemoryRequirements(device,t->read,&req);
    mem.allocationSize=req.size;
    if(!memory_type(physical,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                    &mem.memoryTypeIndex)) {
        result=VK_ERROR_FEATURE_NOT_PRESENT; goto fail;
    }
    TRY(vkAllocateMemory(device,&mem,NULL,&t->read_mem));
    TRY(vkBindBufferMemory(device,t->read,t->read_mem,0));
    VkCommandPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};
    TRY(vkCreateCommandPool(device,&pool,NULL,&t->pool));
    VkCommandBufferAllocateInfo cmd={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=t->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    TRY(vkAllocateCommandBuffers(device,&cmd,&t->command));
    VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(device,&fence,NULL,&t->fence));
    const char *profile=getenv("CHIHIRO_VULKAN_PROFILE");
    if(profile && atoi(profile)) {
        uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,NULL);
        VkQueueFamilyProperties *properties=calloc(n,sizeof *properties);
        if(!properties) {result=VK_ERROR_OUT_OF_HOST_MEMORY;goto fail;}
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,properties);
        if(family<n) t->timestamp_bits=properties[family].timestampValidBits;
        free(properties);
        if(t->timestamp_bits) {
            VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(physical,&properties);
            t->timestamp_period=properties.limits.timestampPeriod;
            VkQueryPoolCreateInfo timing={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=2};
            TRY(vkCreateQueryPool(device,&timing,NULL,&t->timing));
        }
    }
    *out=t; return VK_SUCCESS;
fail:
    ch_vk_target_destroy(t); return result;
}
static VkResult render_read(ChVkTarget *t, const float rgba[4], uint32_t clear_flags, float clear_depth, uint32_t clear_stencil,
    VkBuffer geometry, VkDeviceSize index_offset, uint32_t index_count,
    void *pixels, VkDeviceSize bytes) {
    if(!t || t->failed || !pixels || bytes<t->bytes ||
       (clear_flags & ~7u) || ((clear_flags & 1) && !rgba) ||
       (!t->initialized && clear_flags!=7))
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result;
    uint64_t cpu_start=t->timing?time_ns():0;
    TRY(vkResetCommandBuffer(t->command,0));
    TRY(vkResetFences(t->device,1,&t->fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vkBeginCommandBuffer(t->command,&begin));
    if(t->timing) {
        vkCmdResetQueryPool(t->command,t->timing,0,2);
        vkCmdWriteTimestamp(t->command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,t->timing,0);
    }
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=t->layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=t->layout,
        .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=t->image,.subresourceRange=range};
    vkCmdPipelineBarrier(t->command,t->layout==VK_IMAGE_LAYOUT_UNDEFINED?
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    if(clear_flags & 1) {
        VkClearColorValue color;
        memcpy(color.float32,rgba,sizeof(color.float32));
        vkCmdClearColorImage(t->command,t->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&range);
    }
    VkImageMemoryBarrier depth_barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=t->initialized?(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT):0,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout=t->initialized?VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=t->depth,.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(t->command,t->initialized?
        (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT):VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&depth_barrier);
    if(clear_flags & 6) {
        VkImageSubresourceRange depth_range=depth_barrier.subresourceRange;
        depth_range.aspectMask=((clear_flags & 2)?VK_IMAGE_ASPECT_DEPTH_BIT:0)|
                               ((clear_flags & 4)?VK_IMAGE_ASPECT_STENCIL_BIT:0);
        VkClearDepthStencilValue value={clear_depth,clear_stencil};
        vkCmdClearDepthStencilImage(t->command,t->depth,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&value,1,&depth_range);
    }
    depth_barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depth_barrier.dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depth_barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    depth_barrier.newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        0,0,NULL,0,NULL,1,&depth_barrier);
    if(geometry) {
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,NULL,0,NULL,1,&barrier);
        VkRenderPassBeginInfo rp={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass=t->pass,.framebuffer=t->framebuffer,
            .renderArea={{0,0},{t->width,t->height}}};
        if(t->count_requested) vkCmdResetQueryPool(t->command,t->queries,0,1);
        vkCmdBeginRenderPass(t->command,&rp,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(t->command,VK_PIPELINE_BIND_POINT_GRAPHICS,t->pipeline);
        if(t->textured) vkCmdBindDescriptorSets(t->command,VK_PIPELINE_BIND_POINT_GRAPHICS,
            t->pipeline_layout,0,1,&t->descriptor_set,0,NULL);
        vkCmdPushConstants(t->command,t->pipeline_layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(t->push),t->push.alpha);
        VkDeviceSize offset=t->vertex_offset;
        vkCmdBindVertexBuffers(t->command,0,1,&geometry,&offset);
        vkCmdBindIndexBuffer(t->command,geometry,index_offset,VK_INDEX_TYPE_UINT16);
        if(t->count_requested) vkCmdBeginQuery(t->command,t->queries,0,VK_QUERY_CONTROL_PRECISE_BIT);
        vkCmdDrawIndexed(t->command,index_count,1,0,0,0);
        if(t->count_requested) vkCmdEndQuery(t->command,t->queries,0);
        vkCmdEndRenderPass(t->command);
    }
    barrier.srcAccessMask=geometry?VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT:VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout=geometry?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(t->command,geometry?VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    if(!t->defer_color_readback) {
        VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .imageExtent={t->width,t->height,1}};
        vkCmdCopyImageToBuffer(t->command,t->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,t->read,1,&copy);
        VkBufferMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .buffer=t->read,.offset=0,.size=VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&host,0,NULL);
    }
    if(t->timing) vkCmdWriteTimestamp(t->command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,t->timing,1);
    TRY(vkEndCommandBuffer(t->command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount=1,.pCommandBuffers=&t->command};
    TRY(vkQueueSubmit(t->queue,1,&submit,t->fence));
    TRY(vkWaitForFences(t->device,1,&t->fence,VK_TRUE,5000000000ULL));
    if(t->timing) {
        uint64_t stamps[2];
        TRY(vkGetQueryPoolResults(t->device,t->timing,0,2,sizeof stamps,stamps,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT));
        uint64_t mask=t->timestamp_bits==64?UINT64_MAX:((UINT64_C(1)<<t->timestamp_bits)-1);
        uint64_t now=time_ns();
        t->timing_gpu+=(uint64_t)(((stamps[1]-stamps[0])&mask)*t->timestamp_period);
        t->timing_cpu+=now-cpu_start;t->timing_calls++;
        if(!t->timing_start) t->timing_start=cpu_start;
        if(now-t->timing_start>=1000000000) {
            fprintf(stderr,"[Vulkan GPU] %ux%u %llu submissions: GPU %.2fms host record/submit/wait %.2fms\n",
                t->width,t->height,(unsigned long long)t->timing_calls,t->timing_gpu/1e6,t->timing_cpu/1e6);
            t->timing_start=now;t->timing_gpu=t->timing_cpu=t->timing_calls=0;
        }
    }
    if(geometry && t->count_requested)
        TRY(vkGetQueryPoolResults(t->device,t->queries,0,1,sizeof(t->samples),&t->samples,
            sizeof(t->samples),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
    t->layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    t->initialized=1;
    if(t->defer_color_readback) return VK_SUCCESS;
    void *mapped;
    TRY(vkMapMemory(t->device,t->read_mem,0,VK_WHOLE_SIZE,0,&mapped));
    VkMappedMemoryRange invalidate={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory=t->read_mem,.offset=0,.size=VK_WHOLE_SIZE};
    result=vkInvalidateMappedMemoryRanges(t->device,1,&invalidate);
    if(result==VK_SUCCESS) memcpy(pixels,mapped,(size_t)t->bytes);
    vkUnmapMemory(t->device,t->read_mem);
    if(result!=VK_SUCCESS) goto fail;
    return VK_SUCCESS;
fail:
    t->failed=1; return result;
}

void ch_vk_target_defer_color_readback(ChVkTarget *t,int enabled) {
    if(t) t->defer_color_readback=!!enabled;
}
VkResult ch_vk_target_read_color(ChVkTarget *t,void *pixels,VkDeviceSize bytes) {
    if(!t) return VK_ERROR_INITIALIZATION_FAILED;
    int deferred=t->defer_color_readback;
    t->defer_color_readback=0;
    VkResult result=render_read(t,NULL,0,1,0,VK_NULL_HANDLE,0,0,pixels,bytes);
    t->defer_color_readback=deferred;
    return result;
}
VkResult ch_vk_target_clear_read(ChVkTarget *t, const float rgba[4],
    void *pixels, VkDeviceSize bytes) {
    return render_read(t,rgba,7,1.0f,0,VK_NULL_HANDLE,0,0,pixels,bytes);
}
static VkResult create_pipeline(ChVkTarget *t, const VkPipelineColorBlendAttachmentState *blend, const float constants[4], const VkPipelineDepthStencilStateCreateInfo *depth_state, VkCullModeFlags cull, VkFrontFace front, const ChVkTexture *texture) {
    VkResult result;
    VkShaderModule vert=VK_NULL_HANDLE, frag=VK_NULL_HANDLE;
    VkAttachmentDescription attachment={.format=VK_FORMAT_R8G8B8A8_UNORM,
        .samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentDescription attachments[2]={attachment,{
        .format=t->depth_format,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,.finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference depth_ref={1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ref={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=1,.pColorAttachments=&ref,.pDepthStencilAttachment=&depth_ref};
    VkRenderPassCreateInfo rp={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=2,.pAttachments=attachments,.subpassCount=1,.pSubpasses=&sub};
    TRY(vkCreateRenderPass(t->device,&rp,NULL,&t->pass));
    VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=t->image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    TRY(vkCreateImageView(t->device,&view,NULL,&t->view));
    VkImageView views[]={t->view,t->depth_view};
    VkFramebufferCreateInfo fb={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass=t->pass,.attachmentCount=2,.pAttachments=views,
        .width=t->width,.height=t->height,.layers=1};
    TRY(vkCreateFramebuffer(t->device,&fb,NULL,&t->framebuffer));
    VkShaderModuleCreateInfo shader={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(color_vert_spv),.pCode=color_vert_spv};
    TRY(vkCreateShaderModule(t->device,&shader,NULL,&vert));
    shader.codeSize=t->effect?sizeof(color_effect_spv):t->rc?sizeof(color_combiner_spv):texture?sizeof(color_textured_spv):sizeof(color_frag_spv);
    shader.pCode=t->effect?color_effect_spv:t->rc?color_combiner_spv:texture?color_textured_spv:color_frag_spv;
    TRY(vkCreateShaderModule(t->device,&shader,NULL,&frag));
    VkPipelineShaderStageCreateInfo stages[]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vert,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=frag,.pName="main"}};
    VkVertexInputBindingDescription binding={0,sizeof(ChVkColorVertex),VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[]={
        {0,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(ChVkColorVertex,xy)},
        {1,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(ChVkColorVertex,rgba)},
        {2,0,VK_FORMAT_R32G32_SFLOAT,offsetof(ChVkColorVertex,xy)+4*sizeof(float)},
        {3,0,VK_FORMAT_R32G32B32A32_SFLOAT,6*sizeof(float)},
        {4,0,VK_FORMAT_R32_SFLOAT,10*sizeof(float)},
        {5,0,VK_FORMAT_R32G32B32A32_SFLOAT,11*sizeof(float)},
        {6,0,VK_FORMAT_R32G32B32A32_SFLOAT,15*sizeof(float)},
        {7,0,VK_FORMAT_R32G32B32A32_SFLOAT,19*sizeof(float)},
        {8,0,VK_FORMAT_R32G32B32A32_SFLOAT,23*sizeof(float)}};
    VkPipelineVertexInputStateCreateInfo input={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount=1,.pVertexBindingDescriptions=&binding,
        .vertexAttributeDescriptionCount=9,.pVertexAttributeDescriptions=attrs};
    VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport={0,0,(float)t->width,(float)t->height,0,1};
    VkRect2D scissor={{0,0},{t->width,t->height}};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=cull,.frontFace=front,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount=1,.pAttachments=blend};
    memcpy(cb.blendConstants,constants,sizeof(cb.blendConstants));
    VkPushConstantRange alpha_range={VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(t->push)};
    VkPipelineLayoutCreateInfo layout={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount=1,.pPushConstantRanges=&alpha_range};
    t->textured=texture!=NULL || t->rc!=NULL;
    if(texture || t->rc) {
        VkDescriptorSetLayoutBinding bindings[2]={
            {.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=t->rc?4:1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT},
            {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT}};
        VkDescriptorSetLayoutCreateInfo dsl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount=t->rc?2:1,.pBindings=bindings};
        TRY(vkCreateDescriptorSetLayout(t->device,&dsl,NULL,&t->descriptor_layout));
        VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,t->rc?4:1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
        VkDescriptorPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets=1,.poolSizeCount=t->rc?2:1,.pPoolSizes=sizes};
        TRY(vkCreateDescriptorPool(t->device,&pool,NULL,&t->descriptor_pool));
        VkDescriptorSetAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool=t->descriptor_pool,.descriptorSetCount=1,.pSetLayouts=&t->descriptor_layout};
        TRY(vkAllocateDescriptorSets(t->device,&alloc,&t->descriptor_set));
        layout.setLayoutCount=1; layout.pSetLayouts=&t->descriptor_layout;
    }
    TRY(vkCreatePipelineLayout(t->device,&layout,NULL,&t->pipeline_layout));
    VkGraphicsPipelineCreateInfo pipeline={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount=2,.pStages=stages,.pVertexInputState=&input,.pInputAssemblyState=&assembly,
        .pViewportState=&vp,.pRasterizationState=&raster,.pMultisampleState=&ms,
        .pDepthStencilState=depth_state,.pColorBlendState=&cb,.layout=t->pipeline_layout,.renderPass=t->pass};
    /* Retain driver compilation results across synchronous pipeline changes.
     * This does not reuse a VkPipeline with a different render state. */
    if(!t->pipeline_cache) {
        VkPipelineCacheCreateInfo cache={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        TRY(vkCreatePipelineCache(t->device,&cache,NULL,&t->pipeline_cache));
    }
    TRY(vkCreateGraphicsPipelines(t->device,t->pipeline_cache,1,&pipeline,NULL,&t->pipeline));
fail:
    if(vert) vkDestroyShaderModule(t->device,vert,NULL);
    if(frag) vkDestroyShaderModule(t->device,frag,NULL);
    if(result!=VK_SUCCESS) t->failed=1;
    return result;
}
static void update_images(ChVkTarget *t,const ChVkTexture *texture)
{
    if(!texture && !t->rc) return;
    VkDescriptorImageInfo images[4];
    unsigned count=t->rc?4:1;
    for(unsigned i=0;i<count;i++)
        images[i]=ch_vk_texture_descriptor(t->rc?
            (t->stage_images && t->stage_images[i]?t->stage_images[i]:t->dummy):texture);
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet=t->descriptor_set,.dstBinding=0,.descriptorCount=count,
        .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=images};
    vkUpdateDescriptorSets(t->device,1,&write,0,NULL);
}
static VkResult draw_full(ChVkTarget *t, const float rgba[4],
    const ChVkColorVertex *vertices, uint32_t vertex_count,
    const uint16_t *indices, uint32_t index_count, const Nv2aRenderState *state,
    const ChVkTexture *texture, uint32_t *samples, const Nv2aCombiner *rc, uint32_t fog_color, const ChVkTexture *const stage_images[4], const ChVkEffect *effect, int coverage_alpha, void *pixels, VkDeviceSize bytes) {
    if(!t || t->failed || !vertices || !indices || !vertex_count || !index_count ||
       index_count%3 || !pixels || bytes<t->bytes)
        return VK_ERROR_INITIALIZATION_FAILED;
    for(uint32_t i=0;i<index_count;i++) if(indices[i]>=vertex_count)
        return VK_ERROR_INITIALIZATION_FAILED;
    if(samples && !t->queries) return VK_ERROR_FEATURE_NOT_PRESENT;
    if(rc) {
        if(texture) return VK_ERROR_FEATURE_NOT_PRESENT;
        for(unsigned i=0;i<4;i++) {
            uint32_t mode=(rc->stage_program>>(i*5))&31;
            if(mode>4) return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    if(effect && (effect->kind<1 || effect->kind>2 || rc || texture)) return VK_ERROR_FEATURE_NOT_PRESENT;
    t->effect=effect!=NULL;
    if(effect) t->push.effect=*effect; else memset(&t->push.effect,0,sizeof(t->push.effect));
    t->rc=rc; t->stage_images=stage_images; t->vertex_offset=rc?256:0;
    if(rc && !t->dummy) {
        const uint32_t white=0xffffffff;
        VkResult r=ch_vk_target_upload_texture(t,1,1,&white,&t->dummy);
        if(r!=VK_SUCCESS) return r;
    }
    t->count_requested=samples!=NULL;
    VkResult result;
    VkBuffer buffer=VK_NULL_HANDLE;
    VkDeviceMemory memory=VK_NULL_HANDLE;
    if(!state) return VK_ERROR_INITIALIZATION_FAILED;
    if(state->alpha_test_enable && (state->alpha_func<0x200 || state->alpha_func>0x207))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkCullModeFlags cull=VK_CULL_MODE_NONE;
    VkFrontFace front=VK_FRONT_FACE_COUNTER_CLOCKWISE;
    if(state->cull_enable) {
        switch(state->cull_face) {
        case 0x404: cull=VK_CULL_MODE_FRONT_BIT; break;
        case 0x405: cull=VK_CULL_MODE_BACK_BIT; break;
        case 0x408: cull=VK_CULL_MODE_FRONT_AND_BACK; break;
        default: return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        /* Guest surface coordinates increase downwards. Vulkan's positive
         * viewport has the same orientation: clockwise guest fronts stay CW. */
        if(state->front_face==0x900) front=VK_FRONT_FACE_CLOCKWISE;
        else if(state->front_face==0x901) front=VK_FRONT_FACE_COUNTER_CLOCKWISE;
        else return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    t->push.alpha[0]=!!state->alpha_test_enable;
    t->push.alpha[1]=state->alpha_func; t->push.alpha[2]=state->alpha_ref & 255;
    VkPipelineDepthStencilStateCreateInfo depth_state;
    result=ch_vk_depth_state(state,&depth_state);
    if(result!=VK_SUCCESS) return result;
    VkPipelineColorBlendAttachmentState blend;
    float constants[4];
    result=ch_vk_blend_state(state,&blend,constants);
    if(result!=VK_SUCCESS) return result;
    /* Reference hud_bind keeps guest RGB factors/equation, but accumulates
     * source alpha as coverage for a separately composited HUD image. */
    if(coverage_alpha && blend.blendEnable) {
        blend.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    }
    /* Reuse a consecutive draw's immutable state. Texture handles, combiner
     * program/buffer, alpha test and effects remain draw data and are updated
     * below even on a hit. Target extent/render pass format are fixed here. */
    PipelineKey key={0};
    key.blend=blend; memcpy(key.constants,constants,sizeof constants);
    key.depth=depth_state; key.cull=cull; key.front=front;
    key.shader=effect?3:rc?2:texture?1:0;
    if(!t->pipeline_key_valid || memcmp(&key,&t->pipeline_key,sizeof key)) {
        if(t->pipeline) vkDestroyPipeline(t->device,t->pipeline,NULL);
        if(t->descriptor_pool) vkDestroyDescriptorPool(t->device,t->descriptor_pool,NULL);
        if(t->descriptor_layout) vkDestroyDescriptorSetLayout(t->device,t->descriptor_layout,NULL);
        if(t->pipeline_layout) vkDestroyPipelineLayout(t->device,t->pipeline_layout,NULL);
        if(t->framebuffer) vkDestroyFramebuffer(t->device,t->framebuffer,NULL);
        if(t->view) vkDestroyImageView(t->device,t->view,NULL);
        if(t->pass) vkDestroyRenderPass(t->device,t->pass,NULL);
        t->descriptor_pool=VK_NULL_HANDLE; t->descriptor_layout=VK_NULL_HANDLE;
        t->pipeline=VK_NULL_HANDLE; t->pipeline_layout=VK_NULL_HANDLE;
        t->framebuffer=VK_NULL_HANDLE; t->view=VK_NULL_HANDLE; t->pass=VK_NULL_HANDLE;
        result=create_pipeline(t,&blend,constants,&depth_state,cull,front,texture);
        if(result!=VK_SUCCESS) return result;
        t->pipeline_key=key; t->pipeline_key_valid=1;
    }
    update_images(t,texture);
    VkDeviceSize offset=t->vertex_offset+(VkDeviceSize)vertex_count*sizeof(*vertices);
    VkDeviceSize size=offset+(VkDeviceSize)index_count*sizeof(*indices);
    if(size>SIZE_MAX) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,
        .usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    TRY(vkCreateBuffer(t->device,&info,NULL,&buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(t->device,buffer,&req);
    VkMemoryAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size};
    if(!memory_type(t->physical,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,&alloc.memoryTypeIndex)) {
        result=VK_ERROR_FEATURE_NOT_PRESENT; goto fail;
    }
    TRY(vkAllocateMemory(t->device,&alloc,NULL,&memory));
    TRY(vkBindBufferMemory(t->device,buffer,memory,0));
    void *mapped;
    TRY(vkMapMemory(t->device,memory,0,VK_WHOLE_SIZE,0,&mapped));
    if(rc) {
        memset(mapped,0,256); memcpy(mapped,rc,sizeof(*rc));
        float *fog=(float *)((char *)mapped+224);
        fog[0]=((fog_color>>16)&255)/255.f; fog[1]=((fog_color>>8)&255)/255.f;
        fog[2]=(fog_color&255)/255.f; fog[3]=(fog_color>>24)/255.f;
        uint32_t *on=(uint32_t *)((char *)mapped+240);
        for(unsigned i=0;i<4;i++) on[i]=stage_images && stage_images[i];
        VkDescriptorBufferInfo info={buffer,0,256};
        VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=t->descriptor_set,
            .dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&info};
        vkUpdateDescriptorSets(t->device,1,&write,0,NULL);
    }
    memcpy((char *)mapped+t->vertex_offset,vertices,(size_t)vertex_count*sizeof(*vertices));
    memcpy((char *)mapped+offset,indices,(size_t)(size-offset));
    VkMappedMemoryRange flush={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory=memory,.size=VK_WHOLE_SIZE};
    result=vkFlushMappedMemoryRanges(t->device,1,&flush);
    vkUnmapMemory(t->device,memory);
    if(result!=VK_SUCCESS) goto fail;
    result=render_read(t,rgba,rgba?7:0,1.0f,0,buffer,offset,index_count,pixels,bytes);
    if(result==VK_SUCCESS && samples) *samples=(uint32_t)t->samples;
fail:
    if(t->failed) vkQueueWaitIdle(t->queue);
    if(buffer) vkDestroyBuffer(t->device,buffer,NULL);
    if(memory) vkFreeMemory(t->device,memory,NULL);
    return result;
}

VkResult ch_vk_target_draw_read(ChVkTarget *t, const float rgba[4],
    const ChVkColorVertex *vertices, uint32_t vertex_count,
    const uint16_t *indices, uint32_t index_count, void *pixels, VkDeviceSize bytes) {
    const Nv2aRenderState state={.color_mask=0x01010101};
    return ch_vk_target_draw_state_read(t,rgba,vertices,vertex_count,indices,index_count,&state,pixels,bytes);
}

VkResult ch_vk_target_clear_aspects_read(ChVkTarget *t, uint32_t flags,
    const float rgba[4], float depth, uint32_t stencil, void *pixels, VkDeviceSize bytes) {
    return render_read(t,rgba,flags,depth,stencil,VK_NULL_HANDLE,0,0,pixels,bytes);
}

void ch_vk_target_size(const ChVkTarget *t, uint32_t *width, uint32_t *height) {
    if(width) *width=t?t->width:0;
    if(height) *height=t?t->height:0;
}

VkResult ch_vk_target_draw_state_read(ChVkTarget *t, const float rgba[4],
    const ChVkColorVertex *vertices, uint32_t vertex_count,
    const uint16_t *indices, uint32_t index_count, const Nv2aRenderState *state,
    void *pixels, VkDeviceSize bytes) {
    return ch_vk_target_draw_texture_read(t,rgba,vertices,vertex_count,indices,index_count,state,NULL,pixels,bytes);
}

VkResult ch_vk_target_upload_texture(ChVkTarget *t,uint32_t width,uint32_t height,
    const uint32_t *argb,ChVkTexture **out) {
    if(!t || t->failed) return VK_ERROR_INITIALIZATION_FAILED;
    return ch_vk_texture_upload(t->physical,t->device,t->queue,t->family,width,height,argb,out);
}

VkResult ch_vk_target_enable_queries(ChVkTarget *t) {
    if(!t || t->failed) return VK_ERROR_INITIALIZATION_FAILED;
    if(t->queries) return VK_SUCCESS;
    VkQueryPoolCreateInfo info={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType=VK_QUERY_TYPE_OCCLUSION,.queryCount=1};
    return vkCreateQueryPool(t->device,&info,NULL,&t->queries);
}
VkResult ch_vk_target_draw_texture_read(ChVkTarget *t, const float rgba[4],
    const ChVkColorVertex *vertices, uint32_t vertex_count,
    const uint16_t *indices, uint32_t index_count, const Nv2aRenderState *state,
    const ChVkTexture *texture, void *pixels, VkDeviceSize bytes) {
    return ch_vk_target_draw_count_read(t,rgba,vertices,vertex_count,indices,index_count,state,texture,NULL,pixels,bytes);
}

VkDevice ch_vk_target_device(const ChVkTarget *t) { return t?t->device:VK_NULL_HANDLE; }
VkFence ch_vk_target_completion_fence(const ChVkTarget *t) { return t?t->fence:VK_NULL_HANDLE; }
VkResult ch_vk_target_export_image(ChVkTarget *t,VkImage destination,void *verify,VkDeviceSize bytes) {
    if(!t || t->failed || !t->initialized || !destination || destination==t->image ||
        (verify && bytes<t->bytes)) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result;
    TRY(vkResetCommandBuffer(t->command,0));
    TRY(vkResetFences(t->device,1,&t->fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vkBeginCommandBuffer(t->command,&begin));
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=destination,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,NULL,0,NULL,1,&barrier);
    VkImageCopy copy={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.extent={t->width,t->height,1}};
    vkCmdCopyImage(t->command,t->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        destination,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if(verify) {
        barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,NULL,0,NULL,1,&barrier);
        VkBufferImageCopy read={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={t->width,t->height,1}};
        vkCmdCopyImageToBuffer(t->command,destination,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,t->read,1,&read);
        VkBufferMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .buffer=t->read,.size=VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&host,0,NULL);
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    barrier.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,0,NULL,0,NULL,1,&barrier);
    TRY(vkEndCommandBuffer(t->command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&t->command};
    TRY(vkQueueSubmit(t->queue,1,&submit,t->fence));
    TRY(vkWaitForFences(t->device,1,&t->fence,VK_TRUE,5000000000ULL));
    if(verify) {
        void *mapped;
        TRY(vkMapMemory(t->device,t->read_mem,0,VK_WHOLE_SIZE,0,&mapped));
        VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=t->read_mem,.size=VK_WHOLE_SIZE};
        result=vkInvalidateMappedMemoryRanges(t->device,1,&range);
        if(result==VK_SUCCESS) memcpy(verify,mapped,(size_t)t->bytes);
        vkUnmapMemory(t->device,t->read_mem);
        if(result!=VK_SUCCESS) goto fail;
    }
    return VK_SUCCESS;
fail:
    t->failed=1; return result;
}

VkResult ch_vk_target_draw_count_read(ChVkTarget *t,const float rgba[4],
    const ChVkColorVertex *v,uint32_t n,const uint16_t *indices,uint32_t count,
    const Nv2aRenderState *state,const ChVkTexture *texture,uint32_t *samples,
    void *pixels,VkDeviceSize bytes) {
    return ch_vk_target_draw_combiner_read(t,rgba,v,n,indices,count,state,texture,samples,NULL,0,NULL,pixels,bytes);
}

VkResult ch_vk_target_import_color(ChVkTarget *t,const void *rgba,VkDeviceSize bytes) {
    if(!t || t->failed || !t->initialized || !rgba || bytes<t->bytes) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result; void *mapped;
    TRY(vkMapMemory(t->device,t->read_mem,0,VK_WHOLE_SIZE,0,&mapped));
    memcpy(mapped,rgba,(size_t)t->bytes);
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=t->read_mem,.size=VK_WHOLE_SIZE};
    result=vkFlushMappedMemoryRanges(t->device,1,&range);
    vkUnmapMemory(t->device,t->read_mem);
    if(result!=VK_SUCCESS) goto fail;
    TRY(vkResetCommandBuffer(t->command,0)); TRY(vkResetFences(t->device,1,&t->fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vkBeginCommandBuffer(t->command,&begin));
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=t->image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={t->width,t->height,1}};
    vkCmdCopyBufferToImage(t->command,t->read,t->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    TRY(vkEndCommandBuffer(t->command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&t->command};
    TRY(vkQueueSubmit(t->queue,1,&submit,t->fence));
    TRY(vkWaitForFences(t->device,1,&t->fence,VK_TRUE,5000000000ULL));
    return VK_SUCCESS;
fail:
    t->failed=1; return result;
}

static VkResult transfer_depth_stencil(ChVkTarget *t,void *depth,void *stencil,size_t pixels,int upload) {
    if(!t || t->failed || !t->initialized || !depth || !stencil || pixels<t->bytes/4)
        return VK_ERROR_INITIALIZATION_FAILED;
    if(upload) for(size_t i=0;i<t->bytes/4;i++) {
        float d=((const float *)depth)[i];
        if(!isfinite(d) || d<0 || d>1) return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkResult result; void *mapped;
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=t->read_mem,.size=VK_WHOLE_SIZE};
    if(upload) {
        TRY(vkMapMemory(t->device,t->read_mem,0,VK_WHOLE_SIZE,0,&mapped));
        if(t->depth_format==VK_FORMAT_D24_UNORM_S8_UINT) {
            uint32_t *packed=mapped;
            for(size_t i=0;i<t->bytes/4;i++)
                packed[i]=(uint32_t)((double)((const float *)depth)[i]*16777215.0+0.5);
        } else memcpy(mapped,depth,(size_t)t->bytes);
        memcpy((char *)mapped+t->bytes,stencil,(size_t)t->bytes/4);
        result=vkFlushMappedMemoryRanges(t->device,1,&range);
        vkUnmapMemory(t->device,t->read_mem);
        if(result!=VK_SUCCESS) goto fail;
    }
    TRY(vkResetCommandBuffer(t->command,0)); TRY(vkResetFences(t->device,1,&t->fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vkBeginCommandBuffer(t->command,&begin));
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask=upload?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .newLayout=upload?VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=t->depth,.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkBufferImageCopy regions[2]={
        {.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1},.imageExtent={t->width,t->height,1}},
        {.bufferOffset=t->bytes,.imageSubresource={VK_IMAGE_ASPECT_STENCIL_BIT,0,0,1},.imageExtent={t->width,t->height,1}}};
    if(upload) vkCmdCopyBufferToImage(t->command,t->read,t->depth,barrier.newLayout,2,regions);
    else vkCmdCopyImageToBuffer(t->command,t->depth,barrier.newLayout,t->read,2,regions);
    barrier.srcAccessMask=upload?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout=barrier.newLayout; barrier.newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,0,0,NULL,0,NULL,1,&barrier);
    if(!upload) {
        VkBufferMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .buffer=t->read,.size=VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&host,0,NULL);
    }
    TRY(vkEndCommandBuffer(t->command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&t->command};
    TRY(vkQueueSubmit(t->queue,1,&submit,t->fence));
    TRY(vkWaitForFences(t->device,1,&t->fence,VK_TRUE,5000000000ULL));
    if(!upload) {
        TRY(vkMapMemory(t->device,t->read_mem,0,VK_WHOLE_SIZE,0,&mapped));
        result=vkInvalidateMappedMemoryRanges(t->device,1,&range);
        if(result==VK_SUCCESS) {
            if(t->depth_format==VK_FORMAT_D24_UNORM_S8_UINT) {
                const uint32_t *packed=mapped;
                for(size_t i=0;i<t->bytes/4;i++)
                    ((float *)depth)[i]=(float)((double)(packed[i]&0xffffffu)/16777215.0);
            } else memcpy(depth,mapped,(size_t)t->bytes);
            memcpy(stencil,(char *)mapped+t->bytes,(size_t)t->bytes/4);
        }
        vkUnmapMemory(t->device,t->read_mem);
        if(result!=VK_SUCCESS) goto fail;
    }
    return VK_SUCCESS;
fail:
    t->failed=1; return result;
}
VkResult ch_vk_target_read_depth_stencil(ChVkTarget *t,float *depth,uint8_t *stencil,size_t pixels) {
    return transfer_depth_stencil(t,depth,stencil,pixels,0);
}
VkResult ch_vk_target_write_depth_stencil(ChVkTarget *t,const float *depth,const uint8_t *stencil,size_t pixels) {
    return transfer_depth_stencil(t,(void *)depth,(void *)stencil,pixels,1);
}

VkResult ch_vk_target_draw_combiner_read(ChVkTarget *t,const float rgba[4],const ChVkColorVertex *v,uint32_t n,
    const uint16_t *indices,uint32_t count,const Nv2aRenderState *state,const ChVkTexture *texture,
    uint32_t *samples,const Nv2aCombiner *rc,uint32_t fog,const ChVkTexture *const stages[4],void *pixels,VkDeviceSize bytes) {
    return draw_full(t,rgba,v,n,indices,count,state,texture,samples,rc,fog,stages,NULL,0,pixels,bytes);
}
VkResult ch_vk_target_draw_effect_read(ChVkTarget *t,const ChVkColorVertex *v,uint32_t n,
    const uint16_t *indices,uint32_t count,const Nv2aRenderState *state,const ChVkEffect *effect,void *pixels,VkDeviceSize bytes) {
    if(!effect) return VK_ERROR_INITIALIZATION_FAILED;
    return draw_full(t,NULL,v,n,indices,count,state,NULL,NULL,NULL,0,NULL,effect,0,pixels,bytes);
}

VkResult ch_vk_target_draw_panel_read(ChVkTarget *t,const ChVkColorVertex *v,uint32_t n,
    const uint16_t *indices,uint32_t count,const Nv2aRenderState *state,const ChVkTexture *texture,
    const Nv2aCombiner *rc,uint32_t fog,const ChVkTexture *const stages[4],int coverage_alpha,void *pixels,VkDeviceSize bytes) {
    if(coverage_alpha!=0 && coverage_alpha!=1) return VK_ERROR_INITIALIZATION_FAILED;
    return draw_full(t,NULL,v,n,indices,count,state,texture,NULL,rc,fog,stages,NULL,coverage_alpha,pixels,bytes);
}
