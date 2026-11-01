#include "vk_texture.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
struct ChVkTexture {
    VkDevice device; VkQueue queue;
    VkImage image; VkDeviceMemory image_memory;
    VkImageView view; VkSampler sampler;
    VkBuffer staging; VkDeviceMemory staging_memory;
    VkCommandPool pool; VkCommandBuffer command; VkFence fence;
    uint32_t width,height; VkDeviceSize bytes;
    int failed;
};
#define TRY(x) do { result=(x); if(result!=VK_SUCCESS) goto fail; } while(0)
static int memory_type(VkPhysicalDevice gpu,uint32_t bits,VkMemoryPropertyFlags flags,uint32_t *out) {
    VkPhysicalDeviceMemoryProperties p;
    vkGetPhysicalDeviceMemoryProperties(gpu,&p);
    for(uint32_t i=0;i<p.memoryTypeCount;i++) if((bits&(1u<<i)) && (p.memoryTypes[i].propertyFlags&flags)==flags) {
        *out=i; return 1;
    }
    return 0;
}
void ch_vk_texture_destroy(ChVkTexture *t) {
    if(!t) return;
    if(t->failed) vkQueueWaitIdle(t->queue);
    if(t->fence) vkDestroyFence(t->device,t->fence,NULL);
    if(t->pool) vkDestroyCommandPool(t->device,t->pool,NULL);
    if(t->sampler) vkDestroySampler(t->device,t->sampler,NULL);
    if(t->view) vkDestroyImageView(t->device,t->view,NULL);
    if(t->image) vkDestroyImage(t->device,t->image,NULL);
    if(t->image_memory) vkFreeMemory(t->device,t->image_memory,NULL);
    if(t->staging) vkDestroyBuffer(t->device,t->staging,NULL);
    if(t->staging_memory) vkFreeMemory(t->device,t->staging_memory,NULL);
    free(t);
}
static VkResult transfer(ChVkTexture *t,int upload) {
    VkResult result;
    TRY(vkResetCommandBuffer(t->command,0));
    TRY(vkResetFences(t->device,1,&t->fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vkBeginCommandBuffer(t->command,&begin));
    VkImageMemoryBarrier image={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=upload?0:VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask=upload?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout=upload?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout=upload?VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=t->image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(t->command,upload?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&image);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={t->width,t->height,1}};
    if(upload) vkCmdCopyBufferToImage(t->command,t->staging,t->image,image.newLayout,1,&copy);
    else vkCmdCopyImageToBuffer(t->command,t->image,image.newLayout,t->staging,1,&copy);
    image.srcAccessMask=upload?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_TRANSFER_READ_BIT;
    image.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    image.oldLayout=image.newLayout; image.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
        0,0,NULL,0,NULL,1,&image);
    if(!upload) {
        VkBufferMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .buffer=t->staging,.size=VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
            0,0,NULL,1,&host,0,NULL);
    }
    TRY(vkEndCommandBuffer(t->command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&t->command};
    TRY(vkQueueSubmit(t->queue,1,&submit,t->fence));
    TRY(vkWaitForFences(t->device,1,&t->fence,VK_TRUE,5000000000ULL));
    return VK_SUCCESS;
fail:
    t->failed=1; return result;
}
VkResult ch_vk_texture_upload(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,
    uint32_t family,uint32_t width,uint32_t height,const uint32_t *argb,ChVkTexture **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;
    if(!width || !height || !argb || (uint64_t)width*height>SIZE_MAX/4) return VK_ERROR_INITIALIZATION_FAILED;
    ChVkTexture *t=calloc(1,sizeof(*t));
    if(!t) return VK_ERROR_OUT_OF_HOST_MEMORY;
    t->device=device; t->queue=queue; t->width=width; t->height=height; t->bytes=(VkDeviceSize)width*height*4;
    VkResult result;
    VkImageCreateInfo image={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={width,height,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    TRY(vkCreateImage(device,&image,NULL,&t->image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device,t->image,&req);
    VkMemoryAllocateInfo mem={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size};
    if(!memory_type(gpu,req.memoryTypeBits,0,&mem.memoryTypeIndex)) { result=VK_ERROR_FEATURE_NOT_PRESENT; goto fail; }
    TRY(vkAllocateMemory(device,&mem,NULL,&t->image_memory));
    TRY(vkBindImageMemory(device,t->image,t->image_memory,0));
    VkBufferCreateInfo buf={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=t->bytes,
        .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    TRY(vkCreateBuffer(device,&buf,NULL,&t->staging));
    vkGetBufferMemoryRequirements(device,t->staging,&req); mem.allocationSize=req.size;
    if(!memory_type(gpu,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,&mem.memoryTypeIndex)) { result=VK_ERROR_FEATURE_NOT_PRESENT; goto fail; }
    TRY(vkAllocateMemory(device,&mem,NULL,&t->staging_memory));
    TRY(vkBindBufferMemory(device,t->staging,t->staging_memory,0));
    void *mapped;
    TRY(vkMapMemory(device,t->staging_memory,0,VK_WHOLE_SIZE,0,&mapped));
    unsigned char *bytes=mapped;
    for(size_t i=0;i<(size_t)width*height;i++) {
        bytes[i*4]=(argb[i]>>16)&255; bytes[i*4+1]=(argb[i]>>8)&255;
        bytes[i*4+2]=argb[i]&255; bytes[i*4+3]=argb[i]>>24;
    }
    VkMappedMemoryRange flush={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=t->staging_memory,.size=VK_WHOLE_SIZE};
    result=vkFlushMappedMemoryRanges(device,1,&flush);
    vkUnmapMemory(device,t->staging_memory);
    if(result!=VK_SUCCESS) goto fail;
    VkCommandPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};
    TRY(vkCreateCommandPool(device,&pool,NULL,&t->pool));
    VkCommandBufferAllocateInfo cmd={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=t->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    TRY(vkAllocateCommandBuffers(device,&cmd,&t->command));
    VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(device,&fence,NULL,&t->fence));
    TRY(transfer(t,1));
    VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=t->image,
        .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=image.format,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    TRY(vkCreateImageView(device,&view,NULL,&t->view));
    VkSamplerCreateInfo sampler={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    TRY(vkCreateSampler(device,&sampler,NULL,&t->sampler));
    *out=t; return VK_SUCCESS;
fail:
    ch_vk_texture_destroy(t); return result;
}
VkDescriptorImageInfo ch_vk_texture_descriptor(const ChVkTexture *t) {
    return (VkDescriptorImageInfo){t?t->sampler:VK_NULL_HANDLE,t?t->view:VK_NULL_HANDLE,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}
VkResult ch_vk_texture_read_rgba(ChVkTexture *t,void *rgba,VkDeviceSize bytes) {
    if(!t || t->failed || !rgba || bytes<t->bytes) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result=transfer(t,0);
    if(result!=VK_SUCCESS) return result;
    void *mapped;
    TRY(vkMapMemory(t->device,t->staging_memory,0,VK_WHOLE_SIZE,0,&mapped));
    VkMappedMemoryRange inv={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=t->staging_memory,.size=VK_WHOLE_SIZE};
    result=vkInvalidateMappedMemoryRanges(t->device,1,&inv);
    if(result==VK_SUCCESS) memcpy(rgba,mapped,(size_t)t->bytes);
    vkUnmapMemory(t->device,t->staging_memory);
fail:
    return result;
}

VkResult ch_vk_texture_set_sampler(ChVkTexture *t,const VkSamplerCreateInfo *info) {
    if(!t || t->failed || !info) return VK_ERROR_INITIALIZATION_FAILED;
    VkSampler sampler;
    VkResult r=vkCreateSampler(t->device,info,NULL,&sampler);
    if(r!=VK_SUCCESS) return r;
    vkDestroySampler(t->device,t->sampler,NULL);
    t->sampler=sampler;
    return VK_SUCCESS;
}
