#include "vk_image_read.h"
#include "depth_read_spv.h"
#include "color_read_spv.h"
#include "depth_color_read_spv.h"
#include <stdlib.h>
struct ChVkImageRead {VkDevice device;VkBuffer buffer;VkDescriptorSetLayout dsl;VkDescriptorPool pool;VkDescriptorSet set;VkPipelineLayout layout;VkPipeline pipeline;VkImageView views[2];VkSampler sampler;int depth;VkBuffer color_buffer;VkImageView color_view;};
void ch_vk_image_read_destroy(ChVkImageRead *p) {
    if(!p) return;
    if(p->pipeline) vkDestroyPipeline(p->device,p->pipeline,NULL);
    if(p->layout) vkDestroyPipelineLayout(p->device,p->layout,NULL);
    if(p->pool) vkDestroyDescriptorPool(p->device,p->pool,NULL);
    if(p->dsl) vkDestroyDescriptorSetLayout(p->device,p->dsl,NULL);
    for(unsigned i=0;i<2;i++) if(p->views[i]) vkDestroyImageView(p->device,p->views[i],NULL);
    if(p->color_view) vkDestroyImageView(p->device,p->color_view,NULL);
    if(p->sampler) vkDestroySampler(p->device,p->sampler,NULL);
    free(p);
}
static VkResult create(VkDevice device,VkPipelineCache cache,VkBuffer buffer,VkDeviceSize bytes,VkImage image,VkFormat format,VkImage color,VkBuffer color_buffer,VkDeviceSize color_bytes,ChVkImageRead **out) {
    if(!out || !device || !buffer || !bytes || !image) return VK_ERROR_INITIALIZATION_FAILED;
    if(format!=VK_FORMAT_D32_SFLOAT_S8_UINT && format!=VK_FORMAT_R8G8B8A8_UNORM) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    *out=NULL;ChVkImageRead *p=calloc(1,sizeof *p);if(!p)return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->depth=format==VK_FORMAT_D32_SFLOAT_S8_UINT;p->color_buffer=color_buffer;
    p->device=device;p->buffer=buffer;VkResult result;VkShaderModule shader=VK_NULL_HANDLE;
#define TRY(x) do {result=(x);if(result!=VK_SUCCESS) goto fail;}while(0)
    for(unsigned i=0;i<(p->depth?2u:1u);i++) {
        VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image=image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=format,
            .subresourceRange={!p->depth?VK_IMAGE_ASPECT_COLOR_BIT:i?VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1}};
        TRY(vkCreateImageView(device,&view,NULL,&p->views[i]));
    }
    if(color) {
        VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image=color,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        TRY(vkCreateImageView(device,&view,NULL,&p->color_view));
    }
    VkSamplerCreateInfo sampler={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.maxLod=0};
    TRY(vkCreateSampler(device,&sampler,NULL,&p->sampler));
    VkDescriptorSetLayoutBinding bindings[]={
        {0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
        {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
        {2,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
        {3,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
        {4,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo dl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=color?5:p->depth?3:2,.pBindings=bindings};
    TRY(vkCreateDescriptorSetLayout(device,&dl,NULL,&p->dsl));
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,color?2:1},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,color?3:2}};
    VkDescriptorPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=sizes};
    TRY(vkCreateDescriptorPool(device,&pool,NULL,&p->pool));
    VkDescriptorSetAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=p->pool,.descriptorSetCount=1,.pSetLayouts=&p->dsl};
    TRY(vkAllocateDescriptorSets(device,&alloc,&p->set));
    VkDescriptorBufferInfo buffer_info={buffer,0,bytes};
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=p->set,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffer_info};
    vkUpdateDescriptorSets(device,1,&write,0,NULL);
    for(unsigned i=0;i<(p->depth?2u:1u);i++) {
        VkDescriptorImageInfo info={p->sampler,p->views[i],p->depth?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet texture={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=p->set,
            .dstBinding=i+1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&info};
        vkUpdateDescriptorSets(device,1,&texture,0,NULL);
    }
    if(color) {
        VkDescriptorImageInfo info={p->sampler,p->color_view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo pixels={color_buffer,0,color_bytes};
        VkWriteDescriptorSet writes[]={
            {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=p->set,.dstBinding=3,
             .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&info},
            {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=p->set,.dstBinding=4,
             .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&pixels}};
        vkUpdateDescriptorSets(device,2,writes,0,NULL);
    }
    VkPushConstantRange push={VK_SHADER_STAGE_COMPUTE_BIT,0,20};
    VkPipelineLayoutCreateInfo layout={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&p->dsl,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
    TRY(vkCreatePipelineLayout(device,&layout,NULL,&p->layout));
    VkShaderModuleCreateInfo module={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=color?sizeof depth_color_read_spv:p->depth?sizeof depth_read_spv:sizeof color_read_spv,.pCode=color?depth_color_read_spv:p->depth?depth_read_spv:color_read_spv};
    TRY(vkCreateShaderModule(device,&module,NULL,&shader));
    VkComputePipelineCreateInfo pipeline={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"},.layout=p->layout};
    TRY(vkCreateComputePipelines(device,cache,1,&pipeline,NULL,&p->pipeline));
    vkDestroyShaderModule(device,shader,NULL);*out=p;return VK_SUCCESS;
fail:
    if(shader) vkDestroyShaderModule(device,shader,NULL);ch_vk_image_read_destroy(p);return result;
#undef TRY
}
static void record(ChVkImageRead *p,VkCommandBuffer cmd,uint32_t width,uint32_t height,int morton,int color_morton) {
    uint32_t count=width*height;
    VkBufferMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=p->buffer,.size=VK_WHOLE_SIZE};
    VkBufferMemoryBarrier buffers[2]={barrier,barrier};buffers[1].buffer=p->color_buffer;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,p->color_buffer?2:1,buffers,0,NULL);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,p->pipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,p->layout,0,1,&p->set,0,NULL);
    uint32_t push[5]={count,width,height,!!morton,!!color_morton};vkCmdPushConstants(cmd,p->layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof push,push);
    vkCmdDispatch(cmd,(count+63u)/64u,1,1);
    for(unsigned i=0;i<2;i++) {buffers[i].srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;buffers[i].dstAccessMask=VK_ACCESS_HOST_READ_BIT;}
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,0,NULL,p->color_buffer?2:1,buffers,0,NULL);
}

VkResult ch_vk_image_read_create(VkDevice device,VkPipelineCache cache,VkBuffer buffer,VkDeviceSize bytes,
    VkImage image,VkFormat format,ChVkImageRead **out) {
    return create(device,cache,buffer,bytes,image,format,VK_NULL_HANDLE,VK_NULL_HANDLE,0,out);
}
VkResult ch_vk_image_read_create_pair(VkDevice device,VkPipelineCache cache,VkBuffer buffer,VkDeviceSize bytes,
    VkImage depth,VkImage color,VkBuffer pixels,VkDeviceSize color_bytes,ChVkImageRead **out) {
    if(!color || !pixels || !color_bytes) return VK_ERROR_INITIALIZATION_FAILED;
    return create(device,cache,buffer,bytes,depth,VK_FORMAT_D32_SFLOAT_S8_UINT,color,pixels,color_bytes,out);
}
void ch_vk_image_read_record(ChVkImageRead *p,VkCommandBuffer cmd,uint32_t width,uint32_t height,int morton) {
    record(p,cmd,width,height,morton,0);
}
void ch_vk_image_read_record_pair(ChVkImageRead *p,VkCommandBuffer cmd,uint32_t width,uint32_t height,int depth_morton,int color_morton) {
    record(p,cmd,width,height,depth_morton,color_morton);
}
