#include "vk_interp.h"
#include "frame_interp_spv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* The OpenGL interpolator's tuning (gl_backend.c, interp_setup and
 * search_pass), kept as it is: mean block luma error above CUT is a scene
 * cut; per-pixel colour error from LO to HI fades from interpolated to the
 * real frame; a pixel of motion costs LAMBDA, the zero vector starts ZBIAS
 * ahead in the search and SEL_ZBIAS per pixel. */
#define CUT 0.10f
#define LO 0.06f
#define HI 0.20f
#define LAMBDA 0.0004f
#define ZBIAS 0.004f
#define SEL_ZBIAS 0.01f
#define LEVELS 4u   /* 1, 1/2, 1/4, 1/8 */
enum {B_IMG,B_FIELD,B_SEL,B_STATS,B_COUNT};
typedef struct {uint32_t pass;int32_t w,h,fw,fh,level,win,range,coarse,src,dst;float t,lambda,zbias,lo,hi,cut;} Push;
struct ChVkInterp {
    VkPhysicalDevice physical;VkDevice device;VkQueue queue;
    VkCommandPool pool;VkCommandBuffer cmd;VkFence fence;
    VkDescriptorSetLayout dsl;VkDescriptorPool dpool;VkDescriptorSet set;
    VkPipelineLayout layout;VkPipeline pipeline;VkSampler sampler;
    VkBuffer buf[B_COUNT];VkDeviceMemory mem[B_COUNT];
    VkImage image[2];VkDeviceMemory image_mem[2];VkImageView view[2];   /* A, B */
    unsigned char *img;          /* B_IMG, mapped: A, B (uploads), OUT */
    uint32_t w,h;
    /* CHIHIRO_VULKAN_INTERP_LOG=1 (diagnostic): GPU time per stage, every
     * 300 made frames: upload, the four search levels, select, mix. */
    VkQueryPool queries;float period;double stage_ms[7];uint64_t timed;
};
static int memory_type(VkPhysicalDevice physical,uint32_t bits,VkMemoryPropertyFlags want,VkMemoryPropertyFlags prefer) {
    VkPhysicalDeviceMemoryProperties m;vkGetPhysicalDeviceMemoryProperties(physical,&m);
    int found=-1;
    for(uint32_t i=0;i<m.memoryTypeCount;i++) {
        VkMemoryPropertyFlags f=m.memoryTypes[i].propertyFlags;
        if(!(bits&(1u<<i)) || (f&want)!=want) continue;
        if((f&prefer)==prefer) return (int)i;
        if(found<0) found=(int)i;
    }
    return found;
}
static void free_targets(ChVkInterp *p) {
    if(p->img) {vkUnmapMemory(p->device,p->mem[B_IMG]);p->img=NULL;}
    for(int i=0;i<B_COUNT;i++) {
        if(p->buf[i]) vkDestroyBuffer(p->device,p->buf[i],NULL);
        if(p->mem[i]) vkFreeMemory(p->device,p->mem[i],NULL);
        p->buf[i]=VK_NULL_HANDLE;p->mem[i]=VK_NULL_HANDLE;
    }
    for(int k=0;k<2;k++) {
        if(p->view[k]) vkDestroyImageView(p->device,p->view[k],NULL);
        if(p->image[k]) vkDestroyImage(p->device,p->image[k],NULL);
        if(p->image_mem[k]) vkFreeMemory(p->device,p->image_mem[k],NULL);
        p->view[k]=VK_NULL_HANDLE;p->image[k]=VK_NULL_HANDLE;p->image_mem[k]=VK_NULL_HANDLE;
    }
    p->w=p->h=0;
}
static VkResult buffer(ChVkInterp *p,int i,VkDeviceSize bytes,int host) {
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,
        .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkResult r=vkCreateBuffer(p->device,&bi,NULL,&p->buf[i]);if(r!=VK_SUCCESS) return r;
    VkMemoryRequirements req;vkGetBufferMemoryRequirements(p->device,p->buf[i],&req);
    int type=host?memory_type(p->physical,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                 :memory_type(p->physical,req.memoryTypeBits,0,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(type<0) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=(uint32_t)type};
    r=vkAllocateMemory(p->device,&ai,NULL,&p->mem[i]);if(r!=VK_SUCCESS) return r;
    return vkBindBufferMemory(p->device,p->buf[i],p->mem[i],0);
}
static VkResult image(ChVkInterp *p,int k,uint32_t w,uint32_t h) {
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
        .extent={w,h,1},.mipLevels=LEVELS,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult r=vkCreateImage(p->device,&ii,NULL,&p->image[k]);if(r!=VK_SUCCESS) return r;
    VkMemoryRequirements req;vkGetImageMemoryRequirements(p->device,p->image[k],&req);
    int type=memory_type(p->physical,req.memoryTypeBits,0,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(type<0) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=(uint32_t)type};
    r=vkAllocateMemory(p->device,&ai,NULL,&p->image_mem[k]);if(r!=VK_SUCCESS) return r;
    r=vkBindImageMemory(p->device,p->image[k],p->image_mem[k],0);if(r!=VK_SUCCESS) return r;
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=p->image[k],.viewType=VK_IMAGE_VIEW_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,LEVELS,0,1}};
    return vkCreateImageView(p->device,&vi,NULL,&p->view[k]);
}
static VkResult resize(ChVkInterp *p,uint32_t w,uint32_t h) {
    if(p->w==w && p->h==h) return VK_SUCCESS;
    vkDeviceWaitIdle(p->device);
    free_targets(p);
    VkDeviceSize n=(VkDeviceSize)w*h,fields=(VkDeviceSize)((w+7)/8)*((h+7)/8);
    VkResult r=buffer(p,B_IMG,n*4*3,1);
    if(r==VK_SUCCESS) r=buffer(p,B_FIELD,fields*16*2,0);
    if(r==VK_SUCCESS) r=buffer(p,B_SEL,n*16,0);
    if(r==VK_SUCCESS) r=buffer(p,B_STATS,16,0);
    if(r==VK_SUCCESS) r=image(p,0,w,h);
    if(r==VK_SUCCESS) r=image(p,1,w,h);
    if(r==VK_SUCCESS) r=vkMapMemory(p->device,p->mem[B_IMG],0,VK_WHOLE_SIZE,0,(void **)&p->img);
    if(r!=VK_SUCCESS) {free_targets(p);return r;}
    VkDescriptorBufferInfo bi[B_COUNT];VkDescriptorImageInfo ii[2];VkWriteDescriptorSet write[B_COUNT+2];
    static const uint32_t binding[B_COUNT]={0,3,4,5};
    for(int i=0;i<B_COUNT;i++) {
        bi[i]=(VkDescriptorBufferInfo){p->buf[i],0,VK_WHOLE_SIZE};
        write[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=p->set,.dstBinding=binding[i],
            .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&bi[i]};
    }
    for(int k=0;k<2;k++) {
        ii[k]=(VkDescriptorImageInfo){p->sampler,p->view[k],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        write[B_COUNT+k]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=p->set,.dstBinding=(uint32_t)(1+k),
            .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&ii[k]};
    }
    vkUpdateDescriptorSets(p->device,B_COUNT+2,write,0,NULL);
    p->w=w;p->h=h;
    return VK_SUCCESS;
}
void ch_vk_interp_destroy(ChVkInterp *p) {
    if(!p) return;
    if(p->device) vkDeviceWaitIdle(p->device);
    free_targets(p);
    if(p->queries) vkDestroyQueryPool(p->device,p->queries,NULL);
    if(p->sampler) vkDestroySampler(p->device,p->sampler,NULL);
    if(p->pipeline) vkDestroyPipeline(p->device,p->pipeline,NULL);
    if(p->layout) vkDestroyPipelineLayout(p->device,p->layout,NULL);
    if(p->dpool) vkDestroyDescriptorPool(p->device,p->dpool,NULL);
    if(p->dsl) vkDestroyDescriptorSetLayout(p->device,p->dsl,NULL);
    if(p->fence) vkDestroyFence(p->device,p->fence,NULL);
    if(p->pool) vkDestroyCommandPool(p->device,p->pool,NULL);
    free(p);
}
VkResult ch_vk_interp_create(VkPhysicalDevice physical,VkDevice device,VkQueue queue,uint32_t family,VkPipelineCache cache,ChVkInterp **out) {
    if(!out || !physical || !device || !queue) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;ChVkInterp *p=calloc(1,sizeof *p);if(!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->physical=physical;p->device=device;p->queue=queue;
    VkResult r;VkShaderModule shader=VK_NULL_HANDLE;
#define TRY(x) do {r=(x);if(r!=VK_SUCCESS) goto fail;} while(0)
    VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};
    TRY(vkCreateCommandPool(device,&pi,NULL,&p->pool));
    VkCommandBufferAllocateInfo ci={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=p->pool,
        .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    TRY(vkAllocateCommandBuffers(device,&ci,&p->cmd));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(device,&fi,NULL,&p->fence));
    /* Linear within a level, the level asked for exactly (textureLod with
     * whole levels), edges clamped: the OpenGL interpolator's textures. */
    VkSamplerCreateInfo si={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_LINEAR,.minFilter=VK_FILTER_LINEAR,
        .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod=(float)(LEVELS-1)};
    TRY(vkCreateSampler(device,&si,NULL,&p->sampler));
    VkDescriptorSetLayoutBinding bind[6];
    for(uint32_t i=0;i<6;i++) bind[i]=(VkDescriptorSetLayoutBinding){i,
        i==1 || i==2?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL};
    VkDescriptorSetLayoutCreateInfo dl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=6,.pBindings=bind};
    TRY(vkCreateDescriptorSetLayout(device,&dl,NULL,&p->dsl));
    VkDescriptorPoolSize ps[2]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,B_COUNT},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2}};
    VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=ps};
    TRY(vkCreateDescriptorPool(device,&dp,NULL,&p->dpool));
    VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=p->dpool,.descriptorSetCount=1,.pSetLayouts=&p->dsl};
    TRY(vkAllocateDescriptorSets(device,&da,&p->set));
    VkPushConstantRange push={VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Push)};
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&p->dsl,
        .pushConstantRangeCount=1,.pPushConstantRanges=&push};
    TRY(vkCreatePipelineLayout(device,&li,NULL,&p->layout));
    VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof frame_interp_spv,.pCode=frame_interp_spv};
    TRY(vkCreateShaderModule(device,&mi,NULL,&shader));
    VkComputePipelineCreateInfo cp={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"},
        .layout=p->layout};
    TRY(vkCreateComputePipelines(device,cache,1,&cp,NULL,&p->pipeline));
    vkDestroyShaderModule(device,shader,NULL);
    const char *log=getenv("CHIHIRO_VULKAN_INTERP_LOG");
    if(log && atoi(log)) {
        VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(physical,&props);
        VkQueryPoolCreateInfo qi={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=8};
        if(props.limits.timestampComputeAndGraphics && vkCreateQueryPool(device,&qi,NULL,&p->queries)==VK_SUCCESS)
            p->period=props.limits.timestampPeriod;
    }
    *out=p;return VK_SUCCESS;
fail:
    if(shader) vkDestroyShaderModule(device,shader,NULL);
    ch_vk_interp_destroy(p);return r;
#undef TRY
}
static void barrier(VkCommandBuffer cmd,VkPipelineStageFlags src,VkAccessFlags sa,VkPipelineStageFlags dst,VkAccessFlags da) {
    VkMemoryBarrier m={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=sa,.dstAccessMask=da};
    vkCmdPipelineBarrier(cmd,src,dst,0,1,&m,0,NULL,0,NULL);
}
static void layout(VkCommandBuffer cmd,VkImage image,uint32_t level,uint32_t count,VkImageLayout from,VkImageLayout to,
    VkAccessFlags sa,VkAccessFlags da,VkPipelineStageFlags src,VkPipelineStageFlags dst) {
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=sa,.dstAccessMask=da,.oldLayout=from,.newLayout=to,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,level,count,0,1}};
    vkCmdPipelineBarrier(cmd,src,dst,0,0,NULL,0,NULL,1,&b);
}
/* Level 0 from the upload buffer, then each level blitted (linear: a 2x2
 * mean) from the one above, as glGenerateMipmap; all of it then sampled. */
static void upload(ChVkInterp *p,int k) {
    VkCommandBuffer cmd=p->cmd;VkImage im=p->image[k];
    layout(cmd,im,0,LEVELS,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy={.bufferOffset=(VkDeviceSize)p->w*p->h*4*(VkDeviceSize)k,
        .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={p->w,p->h,1}};
    vkCmdCopyBufferToImage(cmd,p->buf[B_IMG],im,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    for(uint32_t l=1;l<LEVELS;l++) {
        layout(cmd,im,l-1,1,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        int32_t sw=(int32_t)(p->w>>(l-1)),sh=(int32_t)(p->h>>(l-1)),dw=(int32_t)(p->w>>l),dh=(int32_t)(p->h>>l);
        VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,l-1,0,1},.srcOffsets={{0,0,0},{sw?sw:1,sh?sh:1,1}},
            .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,l,0,1},.dstOffsets={{0,0,0},{dw?dw:1,dh?dh:1,1}}};
        vkCmdBlitImage(cmd,im,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,im,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_LINEAR);
    }
    layout(cmd,im,0,LEVELS-1,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,
        VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    layout(cmd,im,LEVELS-1,1,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}
static void stamp(ChVkInterp *p,uint32_t i) {
    if(p->queries) vkCmdWriteTimestamp(p->cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,p->queries,i);
}
static void dispatch(ChVkInterp *p,Push *c,uint32_t items,uint32_t after) {
    /* CHIHIRO_VULKAN_INTERP_SKIP=mask (diagnostic, for timing by
     * elimination where timestamps cannot see inside a pass sequence): bit
     * i leaves out stage i (1-4 search levels, 5 select, 6 mix). */
    static int skip=-1;
    if(skip<0) {const char *e=getenv("CHIHIRO_VULKAN_INTERP_SKIP");skip=e?(int)strtol(e,NULL,0):0;}
    if(skip&(1<<(after-1))) return;
    vkCmdPushConstants(p->cmd,p->layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof *c,c);
    vkCmdDispatch(p->cmd,(items+63)/64,1,1);
    barrier(p->cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    stamp(p,after);
}
VkResult ch_vk_interp_make(ChVkInterp *p,const void *a,const void *b,uint32_t w,uint32_t h,float t,void *out) {
    if(!p || !a || !b || !out || w<8 || h<8 || w>4096 || h>4096 || !(t>0.f && t<1.f)) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=resize(p,w,h);if(r!=VK_SUCCESS) return r;
    size_t n=(size_t)w*h*4;
    memcpy(p->img,a,n);memcpy(p->img+n,b,n);
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    r=vkBeginCommandBuffer(p->cmd,&bi);if(r!=VK_SUCCESS) return r;
    if(p->queries) vkCmdResetQueryPool(p->cmd,p->queries,0,8);
    stamp(p,0);
    barrier(p->cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    upload(p,0);upload(p,1);
    stamp(p,1);
    vkCmdFillBuffer(p->cmd,p->buf[B_STATS],0,VK_WHOLE_SIZE,0);
    barrier(p->cmd,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(p->cmd,VK_PIPELINE_BIND_POINT_COMPUTE,p->pipeline);
    vkCmdBindDescriptorSets(p->cmd,VK_PIPELINE_BIND_POINT_COMPUTE,p->layout,0,1,&p->set,0,NULL);
    Push c={.w=(int32_t)w,.h=(int32_t)h,.fw=(int32_t)((w+7)/8),.fh=(int32_t)((h+7)/8),.t=t,
        .lambda=LAMBDA,.zbias=ZBIAS,.lo=LO,.hi=HI,.cut=CUT};
    /* Coarse to fine (gl_backend.c search_pass): 1/8 size, 32x32 window,
     * +-64 px; then +-1 step at 1/4, 1/2 and full size around the block's
     * vector and its neighbours'. Fields ping-pong; the last is field 1. */
    static const struct {int coarse,level,win,range;} steps[4]={{1,3,2,8},{0,2,3,1},{0,1,4,1},{0,0,6,1}};
    for(int i=0;i<4;i++) {
        c.pass=2;c.coarse=steps[i].coarse;c.level=steps[i].level;c.win=steps[i].win;c.range=steps[i].range;
        c.dst=i&1;c.src=c.dst^1;
        dispatch(p,&c,(uint32_t)(c.fw*c.fh),(uint32_t)(2+i));
    }
    c.pass=3;c.src=1;c.zbias=SEL_ZBIAS;dispatch(p,&c,w*h,6);
    c.pass=4;c.zbias=ZBIAS;dispatch(p,&c,w*h,7);
    barrier(p->cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
    r=vkEndCommandBuffer(p->cmd);if(r!=VK_SUCCESS) return r;
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&p->cmd};
    r=vkQueueSubmit(p->queue,1,&si,p->fence);if(r!=VK_SUCCESS) return r;
    r=vkWaitForFences(p->device,1,&p->fence,VK_TRUE,UINT64_MAX);
    vkResetFences(p->device,1,&p->fence);
    if(r!=VK_SUCCESS) return r;
    memcpy(out,p->img+2*n,n);
    uint64_t ts[8];
    if(p->queries && vkGetQueryPoolResults(p->device,p->queries,0,8,sizeof ts,ts,sizeof ts[0],VK_QUERY_RESULT_64_BIT)==VK_SUCCESS) {
        for(int i=0;i<7;i++) p->stage_ms[i]+=(double)(ts[i+1]-ts[i])*p->period/1e6;
        if(++p->timed%300==0) {
            static const char *name[7]={"upload","search/8","search/4","search/2","search/1","select","mix"};
            fprintf(stderr,"[Vulkan interp GPU]");
            for(int i=0;i<7;i++) {fprintf(stderr," %s %.2f",name[i],p->stage_ms[i]/300.);p->stage_ms[i]=0;}
            fprintf(stderr," ms\n");
        }
    }
    return VK_SUCCESS;
}
