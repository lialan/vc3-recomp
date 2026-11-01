#include "vk_color_pack.h"
#include "color_pack_spv.h"
#include <stdlib.h>
struct ChVkColorPack {VkDevice device;VkBuffer buffer;VkDescriptorSetLayout dsl;VkDescriptorPool pool;VkDescriptorSet set;VkPipelineLayout layout;VkPipeline pipeline;};
void ch_vk_color_pack_destroy(ChVkColorPack *p) {
    if(!p) return;
    if(p->pipeline) vkDestroyPipeline(p->device,p->pipeline,NULL);
    if(p->layout) vkDestroyPipelineLayout(p->device,p->layout,NULL);
    if(p->pool) vkDestroyDescriptorPool(p->device,p->pool,NULL);
    if(p->dsl) vkDestroyDescriptorSetLayout(p->device,p->dsl,NULL);
    free(p);
}
VkResult ch_vk_color_pack_create(VkDevice device,VkPipelineCache cache,VkBuffer buffer,VkDeviceSize bytes,ChVkColorPack **out) {
    if(!out || !device || !buffer || !bytes) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;ChVkColorPack *p=calloc(1,sizeof *p);if(!p)return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->device=device;p->buffer=buffer;VkResult result;VkShaderModule shader=VK_NULL_HANDLE;
#define TRY(x) do {result=(x);if(result!=VK_SUCCESS) goto fail;}while(0)
    VkDescriptorSetLayoutBinding binding={0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL};
    VkDescriptorSetLayoutCreateInfo dl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
    TRY(vkCreateDescriptorSetLayout(device,&dl,NULL,&p->dsl));
    VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
    VkDescriptorPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&size};
    TRY(vkCreateDescriptorPool(device,&pool,NULL,&p->pool));
    VkDescriptorSetAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=p->pool,.descriptorSetCount=1,.pSetLayouts=&p->dsl};
    TRY(vkAllocateDescriptorSets(device,&alloc,&p->set));
    VkDescriptorBufferInfo buffer_info={buffer,0,bytes};
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=p->set,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffer_info};
    vkUpdateDescriptorSets(device,1,&write,0,NULL);
    VkPushConstantRange push={VK_SHADER_STAGE_COMPUTE_BIT,0,8};
    VkPipelineLayoutCreateInfo layout={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&p->dsl,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
    TRY(vkCreatePipelineLayout(device,&layout,NULL,&p->layout));
    VkShaderModuleCreateInfo module={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof color_pack_spv,.pCode=color_pack_spv};
    TRY(vkCreateShaderModule(device,&module,NULL,&shader));
    VkComputePipelineCreateInfo pipeline={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"},.layout=p->layout};
    TRY(vkCreateComputePipelines(device,cache,1,&pipeline,NULL,&p->pipeline));
    vkDestroyShaderModule(device,shader,NULL);*out=p;return VK_SUCCESS;
fail:
    if(shader) vkDestroyShaderModule(device,shader,NULL);ch_vk_color_pack_destroy(p);return result;
#undef TRY
}
void ch_vk_color_pack_record(ChVkColorPack *p,VkCommandBuffer cmd,uint32_t count,int upload) {
    VkBufferMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=p->buffer,.size=VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,1,&barrier,0,NULL);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,p->pipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,p->layout,0,1,&p->set,0,NULL);
    uint32_t push[2]={count,!!upload};vkCmdPushConstants(cmd,p->layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof push,push);
    vkCmdDispatch(cmd,(count+255u)/256u,1,1);
    barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask=upload?VK_ACCESS_TRANSFER_READ_BIT:VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,upload?VK_PIPELINE_STAGE_TRANSFER_BIT:VK_PIPELINE_STAGE_HOST_BIT,
        0,0,NULL,1,&barrier,0,NULL);
}
