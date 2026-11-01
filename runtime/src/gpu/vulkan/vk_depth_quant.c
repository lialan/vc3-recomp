#include "vk_depth_quant.h"
#include <stdlib.h>
#include <stdint.h>
#include "depth_roundtrip_spv.h"
struct ChVkDepthQuant {VkDevice device;VkBuffer buffer;VkDescriptorSetLayout dsl;VkDescriptorPool pool;VkDescriptorSet set;VkPipelineLayout layout;VkPipeline pipeline;};
void ch_vk_depth_quant_destroy(ChVkDepthQuant *q){
 if(!q)return;
 if(q->pipeline)vkDestroyPipeline(q->device,q->pipeline,NULL);
 if(q->layout)vkDestroyPipelineLayout(q->device,q->layout,NULL);
 if(q->pool)vkDestroyDescriptorPool(q->device,q->pool,NULL);
 if(q->dsl)vkDestroyDescriptorSetLayout(q->device,q->dsl,NULL);
 free(q);
}
VkResult ch_vk_depth_quant_create(VkDevice device,VkBuffer buffer,VkDeviceSize bytes,ChVkDepthQuant **out){
 if(!out || !device || !buffer || !bytes)return VK_ERROR_INITIALIZATION_FAILED;
 *out=NULL;ChVkDepthQuant *q=calloc(1,sizeof *q);if(!q)return VK_ERROR_OUT_OF_HOST_MEMORY;
 q->device=device;q->buffer=buffer;VkResult r;VkShaderModule shader=VK_NULL_HANDLE;
#define TRY(x) do{r=(x);if(r!=VK_SUCCESS)goto fail;}while(0)
 VkDescriptorSetLayoutBinding binding={0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL};
 VkDescriptorSetLayoutCreateInfo dl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
 TRY(vkCreateDescriptorSetLayout(device,&dl,NULL,&q->dsl));
 VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
 VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};
 TRY(vkCreateDescriptorPool(device,&dp,NULL,&q->pool));
 VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=q->pool,.descriptorSetCount=1,.pSetLayouts=&q->dsl};
 TRY(vkAllocateDescriptorSets(device,&da,&q->set));
 VkDescriptorBufferInfo db={buffer,0,bytes};
 VkWriteDescriptorSet dw={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=q->set,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&db};
 vkUpdateDescriptorSets(device,1,&dw,0,NULL);
 VkPushConstantRange push={VK_SHADER_STAGE_COMPUTE_BIT,0,4};
 VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&q->dsl,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
 TRY(vkCreatePipelineLayout(device,&pl,NULL,&q->layout));
 VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof depth_roundtrip_spv,.pCode=depth_roundtrip_spv};
 TRY(vkCreateShaderModule(device,&sm,NULL,&shader));
 VkComputePipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"},.layout=q->layout};
 TRY(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pi,NULL,&q->pipeline));
 vkDestroyShaderModule(device,shader,NULL);*out=q;return VK_SUCCESS;
fail:if(shader)vkDestroyShaderModule(device,shader,NULL);ch_vk_depth_quant_destroy(q);return r;
#undef TRY
}
void ch_vk_depth_quant_record(ChVkDepthQuant *q,VkCommandBuffer cmd,VkImage image,uint32_t width,uint32_t height){
 VkImageMemoryBarrier im={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,.oldLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1}};
 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&im);
 VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1},.imageExtent={width,height,1}};
 vkCmdCopyImageToBuffer(cmd,image,im.newLayout,q->buffer,1,&copy);
 VkBufferMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=q->buffer,.size=VK_WHOLE_SIZE};
 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,1,&b,0,NULL);
 vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,q->pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,q->layout,0,1,&q->set,0,NULL);
 uint32_t count=width*height;vkCmdPushConstants(cmd,q->layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&count);vkCmdDispatch(cmd,(count+63)/64,1,1);
 b.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
 im.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;im.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;im.oldLayout=im.newLayout;im.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,1,&b,1,&im);
 vkCmdCopyBufferToImage(cmd,q->buffer,image,im.newLayout,1,&copy);
 im.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;im.dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;im.oldLayout=im.newLayout;im.newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
 b.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,1,&b,1,&im);
}
