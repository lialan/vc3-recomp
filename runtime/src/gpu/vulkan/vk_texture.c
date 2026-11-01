#include "vk_texture.h"
#include "vk_texture_decode.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
struct ChVkTexture {
    VkPhysicalDevice physical; VkDevice device; VkQueue queue;
    VkImage image; VkDeviceMemory image_memory;
    VkImageView view; VkSampler sampler;
    VkBuffer staging; VkDeviceMemory staging_memory;
    VkCommandPool pool; VkCommandBuffer command; VkFence fence;
    uint32_t width,height,levels,layers; VkDeviceSize bytes;
    int failed, pending, deferred_upload,raw_decode;
    uint32_t decode_pitch,decode_flags;
    VkDeviceSize staging_bytes;
    ChVkTextureDecode *decoder;
    uint32_t decode_max_bytes,decode_max_groups[2];
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
static VkResult wait_transfer(ChVkTexture *t) {
    if(!t->pending) return VK_SUCCESS;
    VkResult r=vkWaitForFences(t->device,1,&t->fence,VK_TRUE,5000000000ULL);
    if(r==VK_SUCCESS) t->pending=0;
    else t->failed=1;
    return r;
}
void ch_vk_texture_destroy(ChVkTexture *t) {
    if(!t) return;
    if(wait_transfer(t)!=VK_SUCCESS || t->failed) vkQueueWaitIdle(t->queue);
    if(t->fence) vkDestroyFence(t->device,t->fence,NULL);
    if(t->pool) vkDestroyCommandPool(t->device,t->pool,NULL);
    if(t->sampler) vkDestroySampler(t->device,t->sampler,NULL);
    if(t->view) vkDestroyImageView(t->device,t->view,NULL);
    if(t->image) vkDestroyImage(t->device,t->image,NULL);
    if(t->image_memory) vkFreeMemory(t->device,t->image_memory,NULL);
    ch_vk_texture_decode_destroy(t->decoder);
    if(t->staging) vkDestroyBuffer(t->device,t->staging,NULL);
    if(t->staging_memory) vkFreeMemory(t->device,t->staging_memory,NULL);
    free(t);
}
static VkResult ensure_staging_size(ChVkTexture *t,VkDeviceSize bytes) {
    if(t->staging && t->staging_bytes>=bytes) return VK_SUCCESS;
    VkResult result;VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;
    VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,
        .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    TRY(vkCreateBuffer(t->device,&info,NULL,&buffer));
    VkMemoryRequirements req;vkGetBufferMemoryRequirements(t->device,buffer,&req);
    VkMemoryAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size};
    if(!memory_type(t->physical,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,&alloc.memoryTypeIndex)) {
        result=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;
    }
    TRY(vkAllocateMemory(t->device,&alloc,NULL,&memory));
    TRY(vkBindBufferMemory(t->device,buffer,memory,0));
    ch_vk_texture_decode_destroy(t->decoder);t->decoder=NULL;
    if(t->staging) vkDestroyBuffer(t->device,t->staging,NULL);
    if(t->staging_memory) vkFreeMemory(t->device,t->staging_memory,NULL);
    t->staging=buffer;t->staging_memory=memory;t->staging_bytes=bytes;return VK_SUCCESS;
fail:
    if(buffer) vkDestroyBuffer(t->device,buffer,NULL);
    if(memory) vkFreeMemory(t->device,memory,NULL);
    return result;
}
static VkResult ensure_staging(ChVkTexture *t) {return ensure_staging_size(t,t->bytes);}
static void finish_upload(ChVkTexture *t) {
    uint32_t width=t->width,height=t->height;
    VkImageMemoryBarrier image={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=t->image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,t->layers}};
    for(uint32_t mip=1;mip<t->levels;mip++) {
        image.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        image.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        image.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        image.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        image.subresourceRange.baseMipLevel=mip-1;image.subresourceRange.levelCount=1;
        vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,NULL,0,NULL,1,&image);
        uint32_t next_width=width>1?width/2:1,next_height=height>1?height/2:1;
        VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip-1,0,1},
            .srcOffsets={{0,0,0},{(int32_t)width,(int32_t)height,1}},
            .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,0,1},
            .dstOffsets={{0,0,0},{(int32_t)next_width,(int32_t)next_height,1}}};
        vkCmdBlitImage(t->command,t->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            t->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_LINEAR);
        image.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;image.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        image.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;image.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
            0,0,NULL,0,NULL,1,&image);
        width=next_width;height=next_height;
    }
    image.subresourceRange.baseMipLevel=t->levels-1;image.subresourceRange.levelCount=1;
    image.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    image.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    image.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    image.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
        0,0,NULL,0,NULL,1,&image);
}
static VkResult transfer(ChVkTexture *t,int upload,uint32_t level) {
    VkResult result;
    TRY(wait_transfer(t));
    TRY(vkResetCommandBuffer(t->command,0));
    TRY(vkResetFences(t->device,1,&t->fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vkBeginCommandBuffer(t->command,&begin));
    if(t->decoder) {
        VkBufferMemoryBarrier reuse={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .buffer=t->staging,.size=VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,1,&reuse,0,NULL);
    }
    if(upload && t->raw_decode) ch_vk_texture_decode_record(t->decoder,t->command,t->width,t->height,t->decode_pitch,t->decode_flags);
    VkImageMemoryBarrier image={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=upload?0:VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask=upload?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout=upload?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout=upload?VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=t->image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,level,upload?t->levels:1,0,t->layers}};
    vkCmdPipelineBarrier(t->command,upload?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&image);
    uint32_t width=t->width>>level,height=t->height>>level;
    if(!width) width=1;
    if(!height) height=1;
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,0,t->layers},.imageExtent={width,height,1}};
    if(upload) vkCmdCopyBufferToImage(t->command,t->staging,t->image,image.newLayout,1,&copy);
    else vkCmdCopyImageToBuffer(t->command,t->image,image.newLayout,t->staging,1,&copy);
    if(upload) finish_upload(t);
    else {
        image.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;image.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        image.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;image.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
            0,0,NULL,0,NULL,1,&image);
    }
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
    t->pending=1;
    if(!upload || !t->deferred_upload) TRY(wait_transfer(t));
    return VK_SUCCESS;
fail:
    t->failed=1; return result;
}
VkResult ch_vk_texture_upload(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,
    uint32_t family,uint32_t width,uint32_t height,const uint32_t *argb,ChVkTexture **out) {
    return ch_vk_texture_upload_mips(gpu,device,queue,family,width,height,argb,1,out);
}
static VkResult upload(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,
    uint32_t family,uint32_t width,uint32_t height,const uint32_t *argb,uint32_t levels,uint32_t layers,int deferred,ChVkTexture **out);
VkResult ch_vk_texture_upload_mips(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,
    uint32_t family,uint32_t width,uint32_t height,const uint32_t *argb,uint32_t levels,ChVkTexture **out) {
    return upload(gpu,device,queue,family,width,height,argb,levels,1,0,out);
}
VkResult ch_vk_texture_upload_cube(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,
    uint32_t family,uint32_t size,const uint32_t *faces,ChVkTexture **out) {
    return upload(gpu,device,queue,family,size,size,faces,1,6,0,out);
}
VkResult ch_vk_texture_upload_queued(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,
    uint32_t family,uint32_t width,uint32_t height,const uint32_t *argb,uint32_t levels,int cube,ChVkTexture **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;
    if(cube && (width!=height || levels!=1)) return VK_ERROR_INITIALIZATION_FAILED;
    return upload(gpu,device,queue,family,width,height,argb,levels,cube?6:1,1,out);
}
static VkResult allocate_texture(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,
    uint32_t family,uint32_t width,uint32_t height,uint32_t levels,uint32_t layers,ChVkTexture **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;
    if(!width || !height || (uint64_t)width*height>SIZE_MAX/4/layers) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t full=1,extent=width>height?width:height;
    while(extent>1) {extent>>=1;full++;}
    if(levels!=1 && levels!=full) return VK_ERROR_INITIALIZATION_FAILED;
    if(levels>1) {
        VkFormatProperties properties;vkGetPhysicalDeviceFormatProperties(gpu,VK_FORMAT_R8G8B8A8_UNORM,&properties);
        VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT|
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if((properties.optimalTilingFeatures&needed)!=needed) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    ChVkTexture *t=calloc(1,sizeof(*t));
    if(!t) return VK_ERROR_OUT_OF_HOST_MEMORY;
    t->physical=gpu;t->device=device; t->queue=queue; t->width=width; t->height=height;t->levels=levels;t->layers=layers; t->bytes=(VkDeviceSize)width*height*4*layers;
    VkResult result;
    VkImageCreateInfo image={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.flags=layers==6?VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={width,height,1},.mipLevels=levels,.arrayLayers=layers,
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
    VkCommandPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};
    TRY(vkCreateCommandPool(device,&pool,NULL,&t->pool));
    VkCommandBufferAllocateInfo cmd={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=t->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    TRY(vkAllocateCommandBuffers(device,&cmd,&t->command));
    VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(device,&fence,NULL,&t->fence));
    VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=t->image,
        .viewType=layers==6?VK_IMAGE_VIEW_TYPE_CUBE:VK_IMAGE_VIEW_TYPE_2D,.format=image.format,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,levels,0,layers}};
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
static VkResult upload(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,
    uint32_t family,uint32_t width,uint32_t height,const uint32_t *argb,uint32_t levels,uint32_t layers,int deferred,ChVkTexture **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;if(!argb) return VK_ERROR_INITIALIZATION_FAILED;
    ChVkTexture *t=NULL;
    VkResult result=allocate_texture(gpu,device,queue,family,width,height,levels,layers,&t);
    if(result!=VK_SUCCESS) return result;
    result=ch_vk_texture_replace_argb(t,argb,(size_t)width*height*layers,deferred);
    if(result!=VK_SUCCESS) {ch_vk_texture_destroy(t);return result;}
    *out=t;return VK_SUCCESS;
}
VkResult ch_vk_texture_replace_argb(ChVkTexture *t,const uint32_t *argb,size_t pixels,int queued) {
    if(!t || t->failed || !argb || pixels<t->bytes/4) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result;
    /* Sampling leases must already be complete. The previous upload can
     * still own staging even if no draw ever sampled this image. */
    TRY(wait_transfer(t));
    TRY(ensure_staging(t));
    void *mapped;
    TRY(vkMapMemory(t->device,t->staging_memory,0,VK_WHOLE_SIZE,0,&mapped));
    unsigned char *bytes=mapped;
    for(size_t i=0;i<t->bytes/4;i++) {
        bytes[i*4]=(argb[i]>>16)&255;bytes[i*4+1]=(argb[i]>>8)&255;
        bytes[i*4+2]=argb[i]&255;bytes[i*4+3]=argb[i]>>24;
    }
    VkMappedMemoryRange flush={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=t->staging_memory,.size=VK_WHOLE_SIZE};
    result=vkFlushMappedMemoryRanges(t->device,1,&flush);
    vkUnmapMemory(t->device,t->staging_memory);
    if(result!=VK_SUCCESS) goto fail;
    t->deferred_upload=!!queued;
    TRY(transfer(t,1,0));return VK_SUCCESS;
fail:
    t->failed=1;return result;
}
VkResult ch_vk_texture_copy_from_image(ChVkTexture *t,VkDevice device,VkImage source,uint32_t width,uint32_t height) {
    if(!t || t->failed || !source || source==t->image || device!=t->device ||
        width!=t->width || height!=t->height || t->layers!=1) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result;uint32_t levels=t->levels;
    TRY(wait_transfer(t));
    TRY(vkResetCommandBuffer(t->command,0));
    TRY(vkResetFences(t->device,1,&t->fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vkBeginCommandBuffer(t->command,&begin));
    VkImageMemoryBarrier image={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=t->image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,levels,0,1}};
    vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,NULL,0,NULL,1,&image);
    VkImageCopy copy={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.extent={width,height,1}};
    vkCmdCopyImage(t->command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        t->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    finish_upload(t);
    TRY(vkEndCommandBuffer(t->command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&t->command};
    TRY(vkQueueSubmit(t->queue,1,&submit,t->fence));
    t->pending=1;
    TRY(wait_transfer(t));
    return VK_SUCCESS;
fail:
    t->failed=1;return result;
}
VkResult ch_vk_texture_copy_image(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family,
    uint32_t width,uint32_t height,VkImage source,uint32_t levels,ChVkTexture **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;if(!source) return VK_ERROR_INITIALIZATION_FAILED;
    ChVkTexture *t=NULL;
    VkResult r=allocate_texture(gpu,device,queue,family,width,height,levels,1,&t);
    if(r==VK_SUCCESS) r=ch_vk_texture_copy_from_image(t,device,source,width,height);
    if(r!=VK_SUCCESS) {ch_vk_texture_destroy(t);return r;}
    *out=t;return VK_SUCCESS;
}
VkDescriptorImageInfo ch_vk_texture_descriptor(const ChVkTexture *t) {
    return (VkDescriptorImageInfo){t?t->sampler:VK_NULL_HANDLE,t?t->view:VK_NULL_HANDLE,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}
VkResult ch_vk_texture_read_rgba(ChVkTexture *t,void *rgba,VkDeviceSize bytes) {
    return ch_vk_texture_read_level_rgba(t,0,rgba,bytes);
}
int ch_vk_texture_is_cube(const ChVkTexture *t) {return t && t->layers==6;}
uint32_t ch_vk_texture_levels(const ChVkTexture *t) {return t?t->levels:0;}
VkResult ch_vk_texture_read_level_rgba(ChVkTexture *t,uint32_t level,void *rgba,VkDeviceSize bytes) {
    if(!t || t->failed || !rgba || level>=t->levels) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t width=t->width>>level,height=t->height>>level;
    if(!width) width=1;
    if(!height) height=1;
    VkDeviceSize used=(VkDeviceSize)width*height*4*t->layers;
    if(bytes<used) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result=ensure_staging(t);
    if(result!=VK_SUCCESS) return result;
    result=transfer(t,0,level);
    if(result!=VK_SUCCESS) return result;
    void *mapped;
    TRY(vkMapMemory(t->device,t->staging_memory,0,VK_WHOLE_SIZE,0,&mapped));
    VkMappedMemoryRange inv={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=t->staging_memory,.size=VK_WHOLE_SIZE};
    result=vkInvalidateMappedMemoryRanges(t->device,1,&inv);
    if(result==VK_SUCCESS) memcpy(rgba,mapped,(size_t)used);
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

int ch_vk_texture_can_decode32(const Nv2aTexture *s) {
    if(!s || s->cube || !s->width || !s->height || (uint64_t)s->width*s->height>UINT32_MAX/3) return 0;
    if(s->color==6 || s->color==7) return !(s->width&(s->width-1)) && !(s->height&(s->height-1));
    return (s->color==0x12 || s->color==0x1e) && !(s->pitch&3) && (uint64_t)s->width*4<=s->pitch;
}
VkResult ch_vk_texture_replace_encoded32(ChVkTexture *t,VkPipelineCache cache,const Nv2aTexture *s,const void *raw,size_t bytes,int queued) {
    if(!t || t->failed || !raw) return VK_ERROR_INITIALIZATION_FAILED;
    if(!ch_vk_texture_can_decode32(s)) return VK_ERROR_FEATURE_NOT_PRESENT;
    if(t->layers!=1 || t->width!=s->width || t->height!=s->height) return VK_ERROR_INITIALIZATION_FAILED;
    int swizzled=s->color==6 || s->color==7;
    uint64_t span=swizzled?t->bytes:(uint64_t)(s->height-1)*s->pitch+(uint64_t)s->width*4;
    if(span>bytes || span>SIZE_MAX-t->bytes || span/4+t->bytes/4>UINT32_MAX) return VK_ERROR_INITIALIZATION_FAILED;
    if(!t->decode_max_bytes) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(t->physical,&properties);
        t->decode_max_bytes=properties.limits.maxStorageBufferRange;
        t->decode_max_groups[0]=properties.limits.maxComputeWorkGroupCount[0];
        t->decode_max_groups[1]=properties.limits.maxComputeWorkGroupCount[1];
    }
    if(t->bytes+span>t->decode_max_bytes || (s->width+7u)/8u>t->decode_max_groups[0] ||
       (s->height+7u)/8u>t->decode_max_groups[1]) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkResult result;TRY(wait_transfer(t));TRY(ensure_staging_size(t,t->bytes+span));
    if(!t->decoder) TRY(ch_vk_texture_decode_create(t->device,cache,t->staging,t->staging_bytes,&t->decoder));
    void *mapped;TRY(vkMapMemory(t->device,t->staging_memory,0,VK_WHOLE_SIZE,0,&mapped));
    memcpy((char *)mapped+t->bytes,raw,(size_t)span);
    VkMappedMemoryRange flush={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=t->staging_memory,.size=VK_WHOLE_SIZE};
    result=vkFlushMappedMemoryRanges(t->device,1,&flush);vkUnmapMemory(t->device,t->staging_memory);if(result!=VK_SUCCESS) goto fail;
    t->decode_pitch=s->pitch/4;t->decode_flags=swizzled|((s->color==7 || s->color==0x1e)?2:0);
    t->raw_decode=1;t->deferred_upload=!!queued;result=transfer(t,1,0);t->raw_decode=0;return result;
fail:
    t->failed=1;return result;
}
VkResult ch_vk_texture_upload_encoded32(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family,VkPipelineCache cache,
    const Nv2aTexture *s,const void *raw,size_t bytes,uint32_t levels,int queued,ChVkTexture **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;*out=NULL;
    if(!ch_vk_texture_can_decode32(s)) return VK_ERROR_FEATURE_NOT_PRESENT;
    ChVkTexture *t=NULL;VkResult r=allocate_texture(gpu,device,queue,family,s->width,s->height,levels,1,&t);
    if(r==VK_SUCCESS) r=ch_vk_texture_replace_encoded32(t,cache,s,raw,bytes,queued);
    if(r!=VK_SUCCESS) {ch_vk_texture_destroy(t);return r;}*out=t;return VK_SUCCESS;
}
