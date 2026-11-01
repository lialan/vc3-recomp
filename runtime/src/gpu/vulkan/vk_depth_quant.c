#include "vk_depth_quant.h"
#include <stdlib.h>
#include <stdint.h>
#include "depth_roundtrip_spv.h"
#include "depth_roundtrip_vert.h"
#include "depth_roundtrip_frag.h"
#include "depth_input.h"
struct ChVkDepthQuant {VkDevice device;VkImageView input;VkBuffer buffer;VkDescriptorSetLayout dsl;VkDescriptorPool pool;VkDescriptorSet set;VkPipelineLayout layout;VkPipeline pipeline;VkRenderPass pass;VkFramebuffer framebuffer;uint32_t width,height;};
void ch_vk_depth_quant_destroy(ChVkDepthQuant *q){
 if(!q)return;
 if(q->framebuffer)vkDestroyFramebuffer(q->device,q->framebuffer,NULL);
 if(q->input)vkDestroyImageView(q->device,q->input,NULL);
 if(q->pass)vkDestroyRenderPass(q->device,q->pass,NULL);
 if(q->pipeline)vkDestroyPipeline(q->device,q->pipeline,NULL);
 if(q->layout)vkDestroyPipelineLayout(q->device,q->layout,NULL);
 if(q->pool)vkDestroyDescriptorPool(q->device,q->pool,NULL);
 if(q->dsl)vkDestroyDescriptorSetLayout(q->device,q->dsl,NULL);
 free(q);
}
static VkResult create(VkDevice device,VkBuffer buffer,VkDeviceSize bytes,VkImageView view,uint32_t width,uint32_t height,VkImage input,VkRenderPass inline_pass,ChVkDepthQuant **out){
 if(!out || !device || (!input && (!buffer || !bytes)))return VK_ERROR_INITIALIZATION_FAILED;
 *out=NULL;ChVkDepthQuant *q=calloc(1,sizeof *q);if(!q)return VK_ERROR_OUT_OF_HOST_MEMORY;
 q->device=device;q->buffer=buffer;q->width=width;q->height=height;VkResult r;VkShaderModule shader=VK_NULL_HANDLE,vert=VK_NULL_HANDLE;
 VkShaderStageFlags stage=view?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_COMPUTE_BIT;
#define TRY(x) do{r=(x);if(r!=VK_SUCCESS)goto fail;}while(0)
 VkDescriptorType type=input?VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
 if(input) {
  VkImageViewCreateInfo iv={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=input,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_D32_SFLOAT_S8_UINT,.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1}};
  TRY(vkCreateImageView(device,&iv,NULL,&q->input));
 }
 VkDescriptorSetLayoutBinding binding={0,type,1,stage,NULL};
 VkDescriptorSetLayoutCreateInfo dl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
 TRY(vkCreateDescriptorSetLayout(device,&dl,NULL,&q->dsl));
 VkDescriptorPoolSize ps={type,1};
 VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};
 TRY(vkCreateDescriptorPool(device,&dp,NULL,&q->pool));
 VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=q->pool,.descriptorSetCount=1,.pSetLayouts=&q->dsl};
 TRY(vkAllocateDescriptorSets(device,&da,&q->set));
 VkDescriptorBufferInfo db={buffer,0,bytes};
 VkDescriptorImageInfo di={.imageView=q->input,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
 VkWriteDescriptorSet dw={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=q->set,.descriptorCount=1,.descriptorType=type,.pBufferInfo=input?NULL:&db,.pImageInfo=input?&di:NULL};
 vkUpdateDescriptorSets(device,1,&dw,0,NULL);
 VkPushConstantRange push={stage,0,4};
 VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&q->dsl,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
 TRY(vkCreatePipelineLayout(device,&pl,NULL,&q->layout));
 VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
  .codeSize=input?sizeof depth_input_spv:view?sizeof depth_roundtrip_frag_spv:sizeof depth_roundtrip_spv,
  .pCode=input?depth_input_spv:view?depth_roundtrip_frag_spv:depth_roundtrip_spv};
 TRY(vkCreateShaderModule(device,&sm,NULL,&shader));
 if(!view) {
  VkComputePipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"},.layout=q->layout};
  TRY(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pi,NULL,&q->pipeline));
 } else {
  if(!inline_pass) {
   VkAttachmentDescription attachment={.format=VK_FORMAT_D32_SFLOAT_S8_UINT,.samples=VK_SAMPLE_COUNT_1_BIT,
    .loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
    .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE,
    .initialLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,.finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
   VkAttachmentReference depth={0,input?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
   if(input)attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_GENERAL;
   VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.pDepthStencilAttachment=&depth,.inputAttachmentCount=input?1:0,.pInputAttachments=input?&depth:NULL};
   VkRenderPassCreateInfo pass={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&sub};
   TRY(vkCreateRenderPass(device,&pass,NULL,&q->pass));
   VkFramebufferCreateInfo fb={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=q->pass,.attachmentCount=1,.pAttachments=&view,.width=width,.height=height,.layers=1};
   TRY(vkCreateFramebuffer(device,&fb,NULL,&q->framebuffer));
  }
  sm.codeSize=sizeof depth_roundtrip_vert_spv;sm.pCode=depth_roundtrip_vert_spv;
  TRY(vkCreateShaderModule(device,&sm,NULL,&vert));
  VkPipelineShaderStageCreateInfo stages[]={
   {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vert,.pName="main"},
   {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=shader,.pName="main"}};
  VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
  VkViewport viewport={0,0,(float)width,(float)height,0,1};VkRect2D scissor={{0,0},{width,height}};
  VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
  VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.lineWidth=1};
  VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
  VkPipelineDepthStencilStateCreateInfo ds={.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,.depthTestEnable=VK_TRUE,.depthWriteEnable=VK_TRUE,.depthCompareOp=VK_COMPARE_OP_ALWAYS};
  VkPipelineColorBlendAttachmentState color={0}; /* Preserve the caller color attachment. */
  VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=inline_pass?1:0,.pAttachments=inline_pass?&color:NULL};
  VkGraphicsPipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,.pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&vp,.pRasterizationState=&raster,.pMultisampleState=&ms,.pDepthStencilState=&ds,.pColorBlendState=&cb,.layout=q->layout,.renderPass=inline_pass?inline_pass:q->pass};
  TRY(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pi,NULL,&q->pipeline));
 }
 if(vert)vkDestroyShaderModule(device,vert,NULL);
 vkDestroyShaderModule(device,shader,NULL);*out=q;return VK_SUCCESS;
fail:if(vert)vkDestroyShaderModule(device,vert,NULL);if(shader)vkDestroyShaderModule(device,shader,NULL);ch_vk_depth_quant_destroy(q);return r;
#undef TRY
}
VkResult ch_vk_depth_quant_create(VkDevice d,VkBuffer b,VkDeviceSize n,ChVkDepthQuant **out) {
 return create(d,b,n,VK_NULL_HANDLE,0,0,VK_NULL_HANDLE,VK_NULL_HANDLE,out);
}
VkResult ch_vk_depth_quant_create_draw(VkDevice d,VkBuffer b,VkDeviceSize n,VkImageView view,uint32_t w,uint32_t h,ChVkDepthQuant **out) {
 if(!view || !w || !h || (uint64_t)w*h>n/4)return VK_ERROR_INITIALIZATION_FAILED;
 return create(d,b,n,view,w,h,VK_NULL_HANDLE,VK_NULL_HANDLE,out);
}
VkResult ch_vk_depth_quant_create_input(VkDevice d,VkImage image,VkImageView view,uint32_t w,uint32_t h,ChVkDepthQuant **out) {
 if(!image || !view || !w || !h)return VK_ERROR_INITIALIZATION_FAILED;
 return create(d,VK_NULL_HANDLE,0,view,w,h,image,VK_NULL_HANDLE,out);
}
VkResult ch_vk_depth_quant_create_inline(VkDevice d,VkImage image,VkImageView view,uint32_t w,uint32_t h,VkRenderPass pass,ChVkDepthQuant **out) {
 if(!image || !view || !w || !h || !pass)return VK_ERROR_INITIALIZATION_FAILED;
 return create(d,VK_NULL_HANDLE,0,view,w,h,image,pass,out);
}
void ch_vk_depth_quant_record_inline(ChVkDepthQuant *q,VkCommandBuffer cmd) {
 const VkPipelineStageFlags tests=VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
 VkMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
  .dstAccessMask=VK_ACCESS_INPUT_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
 vkCmdPipelineBarrier(cmd,tests,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|tests,VK_DEPENDENCY_BY_REGION_BIT,1,&b,0,NULL,0,NULL);
 vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,q->pipeline);
 vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,q->layout,0,1,&q->set,0,NULL);
 vkCmdDraw(cmd,3,1,0,0);
 b.srcAccessMask=VK_ACCESS_INPUT_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
 b.dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|tests,tests,VK_DEPENDENCY_BY_REGION_BIT,1,&b,0,NULL,0,NULL);
}
void ch_vk_depth_quant_record(ChVkDepthQuant *q,VkCommandBuffer cmd,VkImage image,uint32_t width,uint32_t height){
 VkImageMemoryBarrier im={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,.oldLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1}};
 if(q->input) {
  /* One fullscreen triangle, one fragment per pixel, no early depth tests.
   * Each invocation reads its old depth before writing the rounded value. */
  im.dstAccessMask=VK_ACCESS_INPUT_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  im.newLayout=VK_IMAGE_LAYOUT_GENERAL;
  vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,0,0,NULL,0,NULL,1,&im);
  VkRenderPassBeginInfo pass={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=q->pass,.framebuffer=q->framebuffer,.renderArea={{0,0},{q->width,q->height}}};
  vkCmdBeginRenderPass(cmd,&pass,VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,q->pipeline);
  vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,q->layout,0,1,&q->set,0,NULL);
  vkCmdDraw(cmd,3,1,0,0);
  vkCmdEndRenderPass(cmd);
  im.srcAccessMask=im.dstAccessMask;im.dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  im.oldLayout=VK_IMAGE_LAYOUT_GENERAL;im.newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,0,0,NULL,0,NULL,1,&im);
  return;
 }
 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&im);
 VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1},.imageExtent={width,height,1}};
 vkCmdCopyImageToBuffer(cmd,image,im.newLayout,q->buffer,1,&copy);
 VkBufferMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=q->buffer,.size=VK_WHOLE_SIZE};
 if(q->pass) {
  b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
  im.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;im.dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  im.oldLayout=im.newLayout;im.newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,0,0,NULL,1,&b,1,&im);
  VkRenderPassBeginInfo pass={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=q->pass,.framebuffer=q->framebuffer,.renderArea={{0,0},{q->width,q->height}}};
  vkCmdBeginRenderPass(cmd,&pass,VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,q->pipeline);
  vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,q->layout,0,1,&q->set,0,NULL);
  vkCmdPushConstants(cmd,q->layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,4,&width);
  vkCmdDraw(cmd,3,1,0,0);
  vkCmdEndRenderPass(cmd);
  b.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
  im.srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;im.oldLayout=im.newLayout;
  vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,0,0,NULL,1,&b,1,&im);
  return;
 }
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
