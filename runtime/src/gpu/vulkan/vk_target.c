#include "vk_target.h"
#include "vk_depth_quant.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include "color_spv.h"
#include "fixed_layout_assert.h"
#include "vk_combiner_program.h"
#include "combiner_program_assert.h"
typedef struct {
    VkPipelineColorBlendAttachmentState blend;
    VkPipelineDepthStencilStateCreateInfo depth;
    VkCullModeFlags cull;
    VkFrontFace front;
    unsigned shader,fixed,alpha_shader,specialized;
    ChVkCombinerProgram program;
} PipelineKey;
#define BASE_PIPELINE_SLOTS 32u
#define PIPELINE_SLOTS 128u
#define BATCH_DRAWS 64u
typedef struct {
    PipelineKey key;
    uint64_t used;
    VkDescriptorSetLayout descriptor_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor_set,sets[BATCH_DRAWS];
    uint32_t set_count,set_used;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    int has_descriptors;
    int pending; /* Referenced by the current batch until its fence completes. */
} PipelineResources;
struct ChVkTarget {
    uint32_t family;
    VkPhysicalDevice physical;
    VkRenderPass pass;
    VkImageView view;
    VkFramebuffer framebuffer;
    const ChVkTexture *const *stage_images;
    ChVkTexture *dummy,*dummy_cube;
    const Nv2aCombiner *rc;
    const ChVkFixedParams *fixed;
    VkDeviceSize vertex_offset;
    PipelineResources pipelines[PIPELINE_SLOTS], *active_pipeline;
    VkPipelineCache pipeline_cache;
    int borrowed_pipeline_cache;
    float blend_constants[4];
    uint64_t pipeline_clock;
    int reuse_resources;
    int split_alpha;
    int specialize_rc;
    VkBuffer geometry;
    VkDeviceMemory geometry_memory;
    void *geometry_map;
    VkDeviceSize geometry_capacity;
    int geometry_coherent;
    ChVkTargetResourceStats resource_stats;
    VkDevice device;
    VkQueue queue;
    VkImage depth;
    VkFormat depth_format;
    ChVkDepthQuant *depth_quant;
    int quantized_depth;
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
    ChVkTargetTimingStats timing_stats,batch_report;
    uint64_t batch_report_start;
    int count_requested;
    uint64_t samples;
    struct { uint32_t alpha[3]; ChVkEffect effect; } push;
    int effect;
    int defer_color_readback;
    int batch_enabled;
    uint32_t pending,retired_count;
    VkDeviceSize geometry_used;
    struct {ChVkTexture *image;void (*release)(void *,ChVkTexture *);void *context;int owned;} retired[BATCH_DRAWS*5];
    int initialized;
    int failed;
};
#define TRY(expr) do { result=(expr); if(result!=VK_SUCCESS) goto fail; } while(0)
static uint64_t time_ns(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;
}
static VkResult timestamp_elapsed(ChVkTarget *t,uint64_t *ns) {
    uint64_t stamps[2];
    VkResult r=vkGetQueryPoolResults(t->device,t->timing,0,2,sizeof stamps,stamps,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT);
    if(r!=VK_SUCCESS) return r;
    uint64_t mask=t->timestamp_bits==64?UINT64_MAX:((UINT64_C(1)<<t->timestamp_bits)-1);
    *ns=(uint64_t)(((stamps[1]-stamps[0])&mask)*t->timestamp_period);
    return VK_SUCCESS;
}
void ch_vk_target_timing_stats(const ChVkTarget *t,ChVkTargetTimingStats *out) {
    if(!out) return;
    *out=t?t->timing_stats:(ChVkTargetTimingStats){0};
    out->available=t && t->timing!=VK_NULL_HANDLE;
}
static void report_batch_timing(ChVkTarget *t,uint64_t now) {
    if(!t->batch_report_start) t->batch_report_start=now;
    if(now-t->batch_report_start<1000000000) return;
    const ChVkTargetTimingStats *s=&t->timing_stats,*b=&t->batch_report;
    fprintf(stderr,"[Vulkan batch GPU] %ux%u draws=%llu submissions=%llu GPU %.2fms host record %.2fms finish %.2fms (submit %.2fms wait %.2fms retire %.2fms)\n",
        t->width,t->height,(unsigned long long)(s->batch_draws-b->batch_draws),
        (unsigned long long)(s->batch_submissions-b->batch_submissions),
        (s->batch_gpu_ns-b->batch_gpu_ns)/1e6,(s->batch_record_ns-b->batch_record_ns)/1e6,
        (s->batch_finish_ns-b->batch_finish_ns)/1e6,(s->batch_submit_ns-b->batch_submit_ns)/1e6,
        (s->batch_wait_ns-b->batch_wait_ns)/1e6,
        (s->batch_retire_ns-b->batch_retire_ns)/1e6);
    t->batch_report=*s;t->batch_report_start=now;
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
static void destroy_pipeline(ChVkTarget *t,PipelineResources *p) {
    if(p->pipeline) vkDestroyPipeline(t->device,p->pipeline,NULL);
    if(p->descriptor_pool) vkDestroyDescriptorPool(t->device,p->descriptor_pool,NULL);
    if(p->pipeline_layout) vkDestroyPipelineLayout(t->device,p->pipeline_layout,NULL);
    if(p->descriptor_layout) vkDestroyDescriptorSetLayout(t->device,p->descriptor_layout,NULL);
    memset(p,0,sizeof *p);
}
static void destroy_geometry(ChVkTarget *t) {
    if(t->geometry_map) vkUnmapMemory(t->device,t->geometry_memory);
    if(t->geometry) vkDestroyBuffer(t->device,t->geometry,NULL);
    if(t->geometry_memory) vkFreeMemory(t->device,t->geometry_memory,NULL);
    t->geometry=VK_NULL_HANDLE;t->geometry_memory=VK_NULL_HANDLE;
    t->geometry_map=NULL;t->geometry_capacity=0;
}
static VkResult reserve_geometry(ChVkTarget *t,VkDeviceSize size) {
    if(size<=t->geometry_capacity) return VK_SUCCESS;
    VkDeviceSize capacity=t->reuse_resources?(t->batch_enabled?(4u<<20):65536):size;
    while(capacity<size) {
        if(capacity>SIZE_MAX/2) {capacity=size;break;}
        capacity*=2;
    }
    VkResult result;
    VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;void *mapped=NULL;
    VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=capacity,
        .usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    TRY(vkCreateBuffer(t->device,&info,NULL,&buffer));
    VkMemoryRequirements req;vkGetBufferMemoryRequirements(t->device,buffer,&req);
    VkMemoryAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size};
    int coherent=memory_type(t->physical,req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,&alloc.memoryTypeIndex);
    if(!coherent && !memory_type(t->physical,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,&alloc.memoryTypeIndex)) {
        result=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;
    }
    TRY(vkAllocateMemory(t->device,&alloc,NULL,&memory));
    TRY(vkBindBufferMemory(t->device,buffer,memory,0));
    TRY(vkMapMemory(t->device,memory,0,VK_WHOLE_SIZE,0,&mapped));
    /* The caller finishes pending work before growing the arena. Publish only
     * a fully allocated/mapped replacement. */
    destroy_geometry(t);
    t->geometry=buffer;t->geometry_memory=memory;t->geometry_map=mapped;
    t->geometry_capacity=capacity;t->geometry_coherent=coherent;
    t->resource_stats.geometry_allocations++;
    return VK_SUCCESS;
fail:
    if(mapped) vkUnmapMemory(t->device,memory);
    if(buffer) vkDestroyBuffer(t->device,buffer,NULL);
    if(memory) vkFreeMemory(t->device,memory,NULL);
    return result;
}
void ch_vk_target_resource_stats(const ChVkTarget *t,ChVkTargetResourceStats *out) {
    if(!out) return;
    *out=t?t->resource_stats:(ChVkTargetResourceStats){0};
    if(t) out->geometry_capacity=t->geometry_capacity;
}
static void retire_samples(ChVkTarget *t) {
    for(unsigned i=0;i<t->retired_count;i++) {
        if(t->retired[i].owned) ch_vk_texture_destroy(t->retired[i].image);
        else if(t->retired[i].release) t->retired[i].release(t->retired[i].context,t->retired[i].image);
    }
    t->retired_count=0;
}
uint32_t ch_vk_target_pending(const ChVkTarget *t) {return t?t->pending:0;}
VkResult ch_vk_target_batching(ChVkTarget *t,int enabled) {
    if(!t || t->failed || (enabled && !t->batch_enabled && t->resource_stats.pipeline_builds)) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=ch_vk_target_finish(t);if(r==VK_SUCCESS) t->batch_enabled=!!enabled;return r;
}
VkResult ch_vk_target_retire_sample(ChVkTarget *t,ChVkTexture *image,
    void (*release)(void *,ChVkTexture *),void *context,int owned) {
    if(!image) return VK_SUCCESS;
    VkResult r=VK_SUCCESS;
    if(t && t->pending && t->retired_count==BATCH_DRAWS*5) r=ch_vk_target_finish(t);
    if(t && t->pending && r==VK_SUCCESS) {
        unsigned i=t->retired_count++;
        t->retired[i].image=image;t->retired[i].release=release;t->retired[i].context=context;t->retired[i].owned=owned;
    } else {
        if(owned) ch_vk_texture_destroy(image);else if(release) release(context,image);
    }
    return r;
}
VkResult ch_vk_target_pipeline_cache(ChVkTarget *t,VkPipelineCache cache) {
    if(!t || !cache || t->pipeline_cache) return VK_ERROR_INITIALIZATION_FAILED;
    t->pipeline_cache=cache;t->borrowed_pipeline_cache=1;
    return VK_SUCCESS;
}
void ch_vk_target_destroy(ChVkTarget *t) {
    if(!t) return;
    /* Finish deferred work before destroying anything it references. */
    if(!t->failed) (void)ch_vk_target_finish(t);
    if(t->failed) vkQueueWaitIdle(t->queue);
    retire_samples(t);
    for(unsigned i=0;i<PIPELINE_SLOTS;i++) destroy_pipeline(t,&t->pipelines[i]);
    if(t->pipeline_cache && !t->borrowed_pipeline_cache) vkDestroyPipelineCache(t->device,t->pipeline_cache,NULL);
    destroy_geometry(t);
    if(t->framebuffer) vkDestroyFramebuffer(t->device,t->framebuffer,NULL);
    if(t->view) vkDestroyImageView(t->device,t->view,NULL);
    if(t->pass) vkDestroyRenderPass(t->device,t->pass,NULL);
    ch_vk_texture_destroy(t->dummy);
    ch_vk_texture_destroy(t->dummy_cube);
    if(t->queries) vkDestroyQueryPool(t->device,t->queries,NULL);
    if(t->timing) vkDestroyQueryPool(t->device,t->timing,NULL);
    if(t->fence) vkDestroyFence(t->device,t->fence,NULL);
    if(t->pool) vkDestroyCommandPool(t->device,t->pool,NULL);
    ch_vk_depth_quant_destroy(t->depth_quant);
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
    const char *reuse=getenv("CHIHIRO_VULKAN_REUSE_RESOURCES");
    t->reuse_resources=!reuse || strcmp(reuse,"0");
    const char *split=getenv("CHIHIRO_VULKAN_NOAT");
    if(!split) split=getenv("VC3_VULKAN_NOAT");
    /* Keep opt-in: the first Quest pair regressed and warm repeats show no
     * benefit. A discard-free module is not automatically faster. */
    t->split_alpha=split && atoi(split);
    const char *specialize=getenv("CHIHIRO_VULKAN_SPECIALIZE_RC");
    t->specialize_rc=specialize && atoi(specialize);
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
        .size=t->bytes+t->bytes/4,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
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
    const char *timestamps=getenv("CHIHIRO_VULKAN_GPU_TIMESTAMPS");
    if(profile && atoi(profile) && (!timestamps || atoi(timestamps))) {
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
static void record_depth_transfer(ChVkTarget *t,int upload) {
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
}
static VkResult finish_batch(ChVkTarget *t,int read_depth) {
    if(!t || t->failed) return VK_ERROR_INITIALIZATION_FAILED;
    if(!t->pending) return VK_SUCCESS;
    VkResult result;
    uint64_t finish_start=t->timing?time_ns():0;
    vkCmdEndRenderPass(t->command);
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=t->image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    if(read_depth) record_depth_transfer(t,0);
    if(t->timing) vkCmdWriteTimestamp(t->command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,t->timing,1);
    TRY(vkEndCommandBuffer(t->command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&t->command};
    uint64_t submit_start=t->timing?time_ns():0;
    TRY(vkQueueSubmit(t->queue,1,&submit,t->fence));
    uint64_t wait_start=t->timing?time_ns():0;
    if(t->timing) t->timing_stats.batch_submit_ns+=wait_start-submit_start;
    TRY(vkWaitForFences(t->device,1,&t->fence,VK_TRUE,5000000000ULL));
    if(t->timing) {
        t->timing_stats.batch_wait_ns+=time_ns()-wait_start;
        uint64_t elapsed;TRY(timestamp_elapsed(t,&elapsed));
        t->timing_stats.batch_gpu_ns+=elapsed;
        t->timing_stats.batch_draws+=t->pending;t->timing_stats.batch_submissions++;
    }
    t->layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;t->resource_stats.batch_submissions++;
    if(read_depth) t->resource_stats.combined_depth_readbacks++;
    t->pending=0;t->geometry_used=0;
    for(unsigned i=0;i<PIPELINE_SLOTS;i++) {t->pipelines[i].set_used=0;t->pipelines[i].pending=0;}
    uint64_t retire_start=t->timing?time_ns():0;
    retire_samples(t);
    if(t->timing) {
        uint64_t now=time_ns();t->timing_stats.batch_retire_ns+=now-retire_start;
        t->timing_stats.batch_finish_ns+=now-finish_start;report_batch_timing(t,now);
    }
    return VK_SUCCESS;
fail:
    t->failed=1;vkQueueWaitIdle(t->queue);t->pending=0;
    retire_samples(t);return result;
}
VkResult ch_vk_target_finish(ChVkTarget *t) {return finish_batch(t,0);}
VkResult ch_vk_target_depth_quantization(ChVkTarget *t,int enabled) {
    if(!t)return VK_ERROR_INITIALIZATION_FAILED;
    if(!enabled) {VkResult r=ch_vk_target_finish(t);if(r==VK_SUCCESS)t->quantized_depth=0;return r;}
    if(t->quantized_depth)return VK_SUCCESS;
    if(!t || t->failed || !t->initialized || t->depth_format!=VK_FORMAT_D32_SFLOAT_S8_UINT)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(t->physical,&props);
    if(t->bytes>props.limits.maxStorageBufferRange || t->bytes/4>UINT32_MAX-63 ||
       (t->bytes/4+63)/64>props.limits.maxComputeWorkGroupCount[0] ||
       props.limits.maxComputeWorkGroupInvocations<64 || props.limits.maxComputeWorkGroupSize[0]<64)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(t->physical,&n,NULL);
    VkQueueFamilyProperties *families=calloc(n,sizeof *families);
    if(!families)return VK_ERROR_OUT_OF_HOST_MEMORY;
    vkGetPhysicalDeviceQueueFamilyProperties(t->physical,&n,families);
    int compute=t->family<n && (families[t->family].queueFlags&VK_QUEUE_COMPUTE_BIT);free(families);
    if(!compute)return VK_ERROR_FEATURE_NOT_PRESENT;
    VkResult result;
    TRY(ch_vk_target_finish(t));
    if(!t->depth_quant) TRY(ch_vk_depth_quant_create(t->device,t->read,t->bytes,&t->depth_quant));
    t->quantized_depth=1;return VK_SUCCESS;
fail:return result;
}
VkResult ch_vk_target_quantize_depth(ChVkTarget *t) {
    VkResult result;int enabled=t?t->quantized_depth:0;
    TRY(ch_vk_target_depth_quantization(t,1));
    t->quantized_depth=enabled;
    TRY(ch_vk_target_finish(t));
    TRY(vkResetCommandBuffer(t->command,0));TRY(vkResetFences(t->device,1,&t->fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vkBeginCommandBuffer(t->command,&begin));
    ch_vk_depth_quant_record(t->depth_quant,t->command,t->depth,t->width,t->height);
    TRY(vkEndCommandBuffer(t->command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&t->command};
    TRY(vkQueueSubmit(t->queue,1,&submit,t->fence));
    TRY(vkWaitForFences(t->device,1,&t->fence,VK_TRUE,5000000000ULL));
    return VK_SUCCESS;
fail:t->failed=1;return result;
}
static VkResult record_batch(ChVkTarget *t,VkBuffer geometry,VkDeviceSize index_offset,uint32_t index_count) {
    VkResult result;
    uint64_t record_start=t->timing?time_ns():0;
    if(!t->pending) {
        TRY(vkResetCommandBuffer(t->command,0));TRY(vkResetFences(t->device,1,&t->fence));
        VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        TRY(vkBeginCommandBuffer(t->command,&begin));
        if(t->timing) {
            vkCmdResetQueryPool(t->command,t->timing,0,2);
            vkCmdWriteTimestamp(t->command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,t->timing,0);
        }
        VkImageMemoryBarrier barriers[2]={
            {.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
                .dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .oldLayout=t->layout,.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                .image=t->image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}},
            {.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                .dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                .oldLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                .image=t->depth,.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1}}};
        vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            0,0,NULL,0,NULL,2,barriers);
        VkRenderPassBeginInfo pass={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=t->pass,
            .framebuffer=t->framebuffer,.renderArea={{0,0},{t->width,t->height}}};
        vkCmdBeginRenderPass(t->command,&pass,VK_SUBPASS_CONTENTS_INLINE);
    }
    vkCmdBindPipeline(t->command,VK_PIPELINE_BIND_POINT_GRAPHICS,t->active_pipeline->pipeline);
    vkCmdSetBlendConstants(t->command,t->blend_constants);
    if(t->active_pipeline->has_descriptors) vkCmdBindDescriptorSets(t->command,VK_PIPELINE_BIND_POINT_GRAPHICS,
        t->active_pipeline->pipeline_layout,0,1,&t->active_pipeline->descriptor_set,0,NULL);
    vkCmdPushConstants(t->command,t->active_pipeline->pipeline_layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(t->push),t->push.alpha);
    VkDeviceSize vertex_offset=t->vertex_offset;
    vkCmdBindVertexBuffers(t->command,0,1,&geometry,&vertex_offset);
    vkCmdBindIndexBuffer(t->command,geometry,index_offset,VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(t->command,index_count,1,0,0,0);
    if(t->quantized_depth && t->active_pipeline->key.depth.depthTestEnable &&
       t->active_pipeline->key.depth.depthWriteEnable) {
        vkCmdEndRenderPass(t->command);
        ch_vk_depth_quant_record(t->depth_quant,t->command,t->depth,t->width,t->height);
        VkMemoryBarrier color={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
        vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,1,&color,0,NULL,0,NULL);
        VkRenderPassBeginInfo pass={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=t->pass,
            .framebuffer=t->framebuffer,.renderArea={{0,0},{t->width,t->height}}};
        vkCmdBeginRenderPass(t->command,&pass,VK_SUBPASS_CONTENTS_INLINE);
    }
    t->active_pipeline->pending=1;
    t->pending++;t->resource_stats.batch_draws++;
    if(t->timing) t->timing_stats.batch_record_ns+=time_ns()-record_start;
    return VK_SUCCESS;
fail:
    t->failed=1;return result;
}
static VkResult render_read(ChVkTarget *t, const float rgba[4], uint32_t clear_flags, float clear_depth, uint32_t clear_stencil,
    VkBuffer geometry, VkDeviceSize index_offset, uint32_t index_count,
    void *pixels, VkDeviceSize bytes) {
    if(!t || t->failed || !pixels || bytes<t->bytes ||
       (clear_flags & ~7u) || ((clear_flags & 1) && !rgba) ||
       (!t->initialized && clear_flags!=7))
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result;
    TRY(ch_vk_target_finish(t));
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
        vkCmdBindPipeline(t->command,VK_PIPELINE_BIND_POINT_GRAPHICS,t->active_pipeline->pipeline);
    vkCmdSetBlendConstants(t->command,t->blend_constants);
        if(t->active_pipeline->has_descriptors) vkCmdBindDescriptorSets(t->command,VK_PIPELINE_BIND_POINT_GRAPHICS,
            t->active_pipeline->pipeline_layout,0,1,&t->active_pipeline->descriptor_set,0,NULL);
        vkCmdPushConstants(t->command,t->active_pipeline->pipeline_layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(t->push),t->push.alpha);
        VkDeviceSize offset=t->vertex_offset;
        vkCmdBindVertexBuffers(t->command,0,1,&geometry,&offset);
        vkCmdBindIndexBuffer(t->command,geometry,index_offset,VK_INDEX_TYPE_UINT16);
        if(t->count_requested) vkCmdBeginQuery(t->command,t->queries,0,VK_QUERY_CONTROL_PRECISE_BIT);
        vkCmdDrawIndexed(t->command,index_count,1,0,0,0);
        if(t->count_requested) vkCmdEndQuery(t->command,t->queries,0);
        vkCmdEndRenderPass(t->command);
    }
    if(t->quantized_depth && ((clear_flags&2) || (geometry &&
       t->active_pipeline->key.depth.depthTestEnable && t->active_pipeline->key.depth.depthWriteEnable)))
        ch_vk_depth_quant_record(t->depth_quant,t->command,t->depth,t->width,t->height);
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
        uint64_t elapsed;TRY(timestamp_elapsed(t,&elapsed));
        uint64_t now=time_ns();
        t->timing_gpu+=elapsed;
        t->timing_stats.sync_render_gpu_ns+=elapsed;
        t->timing_stats.sync_render_host_ns+=now-cpu_start;t->timing_stats.sync_render_submissions++;
        t->timing_cpu+=now-cpu_start;t->timing_calls++;
        if(!t->timing_start) t->timing_start=cpu_start;
        if(now-t->timing_start>=1000000000) {
            fprintf(stderr,"[Vulkan GPU] %ux%u %llu synchronous submissions (excludes command batches): GPU %.2fms host record/submit/wait %.2fms\n",
                t->width,t->height,(unsigned long long)t->timing_calls,t->timing_gpu/1e6,t->timing_cpu/1e6);
            fprintf(stderr,"[Vulkan resources] %ux%u: pipelines builds=%llu hits=%llu evictions=%llu geometry allocations=%llu capacity=%lluKiB alpha-free builds/hits=%llu/%llu specialized builds/hits=%llu/%llu pipeline create total/max=%.2f/%.2fms\n",
                t->width,t->height,(unsigned long long)t->resource_stats.pipeline_builds,
                (unsigned long long)t->resource_stats.pipeline_hits,(unsigned long long)t->resource_stats.pipeline_evictions,
                (unsigned long long)t->resource_stats.geometry_allocations,(unsigned long long)(t->geometry_capacity/1024),
                (unsigned long long)t->resource_stats.no_alpha_pipeline_builds,
                (unsigned long long)t->resource_stats.no_alpha_pipeline_hits,
                (unsigned long long)t->resource_stats.specialized_pipeline_builds,
                (unsigned long long)t->resource_stats.specialized_pipeline_hits,
                t->resource_stats.pipeline_create_ns/1e6,t->resource_stats.pipeline_create_max_ns/1e6);
            t->timing_start=now;t->timing_gpu=t->timing_cpu=t->timing_calls=0;
        }
    }
    if(geometry && t->count_requested)
        TRY(vkGetQueryPoolResults(t->device,t->queries,0,1,sizeof(t->samples),&t->samples,
            sizeof(t->samples),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
    t->layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    t->initialized=1;
    for(unsigned i=0;i<PIPELINE_SLOTS;i++) t->pipelines[i].set_used=0;
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
    if(t) {if(!enabled && t->pending) (void)ch_vk_target_finish(t);t->defer_color_readback=!!enabled;}
}
VkResult ch_vk_target_read_color(ChVkTarget *t,void *pixels,VkDeviceSize bytes) {
    if(!t) return VK_ERROR_INITIALIZATION_FAILED;
    int deferred=t->defer_color_readback;
    t->defer_color_readback=0;
    VkResult result=render_read(t,NULL,0,1,0,VK_NULL_HANDLE,0,0,pixels,bytes);
    t->defer_color_readback=deferred;
    return result;
}
VkResult ch_vk_target_snapshot_texture(ChVkTarget *t,uint32_t levels,ChVkTexture **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;
    if(!t || t->failed || !t->initialized) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result=ch_vk_target_finish(t);
    if(result!=VK_SUCCESS) return result;
    if(t->layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) return VK_ERROR_INITIALIZATION_FAILED;
    return ch_vk_texture_copy_image(t->physical,t->device,t->queue,t->family,t->width,t->height,t->image,levels,out);
}
VkResult ch_vk_target_refresh_texture(ChVkTarget *t,ChVkTexture *texture) {
    if(!t || t->failed || !t->initialized) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=ch_vk_target_finish(t);
    if(r!=VK_SUCCESS) return r;
    if(t->layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) return VK_ERROR_INITIALIZATION_FAILED;
    return ch_vk_texture_copy_from_image(texture,t->device,t->image,t->width,t->height);
}
VkResult ch_vk_target_clear_read(ChVkTarget *t, const float rgba[4],
    void *pixels, VkDeviceSize bytes) {
    return render_read(t,rgba,7,1.0f,0,VK_NULL_HANDLE,0,0,pixels,bytes);
}
static VkResult create_pipeline(ChVkTarget *t, const VkPipelineColorBlendAttachmentState *blend, const float constants[4], const VkPipelineDepthStencilStateCreateInfo *depth_state, VkCullModeFlags cull, VkFrontFace front, const ChVkTexture *texture,const PipelineKey *key) {
    VkResult result;
    VkShaderModule vert=VK_NULL_HANDLE, frag=VK_NULL_HANDLE;
    if(!t->pass) {
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
    }
    VkShaderModuleCreateInfo shader={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=t->fixed?sizeof(color_fixed_spv):sizeof(color_vert_spv),.pCode=t->fixed?color_fixed_spv:color_vert_spv};
    TRY(vkCreateShaderModule(t->device,&shader,NULL,&vert));
    if(key->specialized) {
        shader.codeSize=key->alpha_shader?sizeof(color_combiner_specialized_spv):sizeof(color_combiner_specialized_noat_spv);
        shader.pCode=key->alpha_shader?color_combiner_specialized_spv:color_combiner_specialized_noat_spv;
    } else if(key->alpha_shader) {
        shader.codeSize=t->effect?sizeof(color_effect_spv):t->rc?sizeof(color_combiner_spv):texture?sizeof(color_textured_spv):sizeof(color_frag_spv);
        shader.pCode=t->effect?color_effect_spv:t->rc?color_combiner_spv:texture?color_textured_spv:color_frag_spv;
    } else {
        shader.codeSize=t->effect?sizeof(color_effect_noat_spv):t->rc?sizeof(color_combiner_noat_spv):texture?sizeof(color_textured_noat_spv):sizeof(color_frag_noat_spv);
        shader.pCode=t->effect?color_effect_noat_spv:t->rc?color_combiner_noat_spv:texture?color_textured_noat_spv:color_frag_noat_spv;
    }
    TRY(vkCreateShaderModule(t->device,&shader,NULL,&frag));
    VkPipelineShaderStageCreateInfo stages[]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vert,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=frag,.pName="main"}};
    VkSpecializationMapEntry entries[sizeof(key->program)/sizeof(uint32_t)];
    for(unsigned i=0;i<sizeof entries/sizeof entries[0];i++)
        entries[i]=(VkSpecializationMapEntry){.constantID=i,.offset=i*sizeof(uint32_t),.size=sizeof(uint32_t)};
    VkSpecializationInfo specialization={.mapEntryCount=sizeof entries/sizeof entries[0],.pMapEntries=entries,
        .dataSize=sizeof key->program,.pData=&key->program};
    if(key->specialized) stages[1].pSpecializationInfo=&specialization;
    VkVertexInputBindingDescription binding={0,t->fixed?sizeof(ChVkFixedVertex):sizeof(ChVkColorVertex),VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription fixed_attrs[8];
    for(unsigned i=0;i<8;i++) fixed_attrs[i]=(VkVertexInputAttributeDescription){i,0,VK_FORMAT_R32G32B32A32_SFLOAT,i*16};
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
        .vertexAttributeDescriptionCount=t->fixed?8:9,.pVertexAttributeDescriptions=t->fixed?fixed_attrs:attrs};
    VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport={0,0,(float)t->width,(float)t->height,0,1};
    VkRect2D scissor={{0,0},{t->width,t->height}};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=cull,.frontFace=front,.lineWidth=1,.depthClampEnable=t->fixed!=NULL};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount=1,.pAttachments=blend};
    memcpy(cb.blendConstants,constants,sizeof(cb.blendConstants));
    VkPushConstantRange alpha_range={VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(t->push)};
    VkPipelineLayoutCreateInfo layout={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount=1,.pPushConstantRanges=&alpha_range};
    t->active_pipeline->has_descriptors=texture!=NULL || t->rc!=NULL || t->fixed!=NULL;
    if(t->active_pipeline->has_descriptors) {
        VkDescriptorSetLayoutBinding bindings[4];unsigned n=0;
        uint32_t images=t->rc?8:texture?1:0,buffers=(t->rc!=NULL)+(t->fixed!=NULL);
        if(images) bindings[n++]=(VkDescriptorSetLayoutBinding){.binding=0,
            .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=t->rc?4:1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
        if(t->rc) {
            bindings[n++]=(VkDescriptorSetLayoutBinding){.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
            bindings[n++]=(VkDescriptorSetLayoutBinding){.binding=2,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=4,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
        }
        if(t->fixed) bindings[n++]=(VkDescriptorSetLayoutBinding){.binding=3,
            .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_VERTEX_BIT};
        VkDescriptorSetLayoutCreateInfo dsl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=n,.pBindings=bindings};
        TRY(vkCreateDescriptorSetLayout(t->device,&dsl,NULL,&t->active_pipeline->descriptor_layout));
        VkDescriptorPoolSize sizes[2];n=0;
        if(images) sizes[n++]=(VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,images};
        if(buffers) sizes[n++]=(VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,buffers};
        for(unsigned i=0;i<n;i++) sizes[i].descriptorCount*=t->batch_enabled?BATCH_DRAWS:1;
        VkDescriptorPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=t->batch_enabled?BATCH_DRAWS:1,.poolSizeCount=n,.pPoolSizes=sizes};
        TRY(vkCreateDescriptorPool(t->device,&pool,NULL,&t->active_pipeline->descriptor_pool));
        VkDescriptorSetAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool=t->active_pipeline->descriptor_pool,.descriptorSetCount=1,.pSetLayouts=&t->active_pipeline->descriptor_layout};
        TRY(vkAllocateDescriptorSets(t->device,&alloc,&t->active_pipeline->descriptor_set));
        t->active_pipeline->sets[0]=t->active_pipeline->descriptor_set;t->active_pipeline->set_count=1;
        layout.setLayoutCount=1;layout.pSetLayouts=&t->active_pipeline->descriptor_layout;
    }
    TRY(vkCreatePipelineLayout(t->device,&layout,NULL,&t->active_pipeline->pipeline_layout));
    VkDynamicState dynamic_state=VK_DYNAMIC_STATE_BLEND_CONSTANTS;
    VkPipelineDynamicStateCreateInfo dynamic={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount=1,.pDynamicStates=&dynamic_state};
    VkGraphicsPipelineCreateInfo pipeline={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount=2,.pStages=stages,.pVertexInputState=&input,.pInputAssemblyState=&assembly,
        .pViewportState=&vp,.pRasterizationState=&raster,.pMultisampleState=&ms,
        .pDynamicState=&dynamic,.pDepthStencilState=depth_state,.pColorBlendState=&cb,.layout=t->active_pipeline->pipeline_layout,.renderPass=t->pass};
    /* Retain driver compilation results across synchronous pipeline changes.
     * This does not reuse a VkPipeline with a different render state. */
    if(!t->pipeline_cache) {
        VkPipelineCacheCreateInfo cache={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        TRY(vkCreatePipelineCache(t->device,&cache,NULL,&t->pipeline_cache));
    }
    TRY(vkCreateGraphicsPipelines(t->device,t->pipeline_cache,1,&pipeline,NULL,&t->active_pipeline->pipeline));
fail:
    if(vert) vkDestroyShaderModule(t->device,vert,NULL);
    if(frag) vkDestroyShaderModule(t->device,frag,NULL);
    if(result!=VK_SUCCESS) t->failed=1;
    return result;
}
static void update_images(ChVkTarget *t,const ChVkTexture *texture)
{
    if(!texture && !t->rc) return;
    VkDescriptorImageInfo images[4],cubes[4];
    unsigned count=t->rc?4:1;
    for(unsigned i=0;i<count;i++) {
        const ChVkTexture *stage=t->rc?(t->stage_images?t->stage_images[i]:NULL):texture;
        images[i]=ch_vk_texture_descriptor(stage && !ch_vk_texture_is_cube(stage)?stage:t->dummy);
        if(t->rc) cubes[i]=ch_vk_texture_descriptor(ch_vk_texture_is_cube(stage)?stage:t->dummy_cube);
    }
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet=t->active_pipeline->descriptor_set,.dstBinding=0,.descriptorCount=count,
        .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=images};
    vkUpdateDescriptorSets(t->device,1,&write,0,NULL);
    if(t->rc) {write.dstBinding=2;write.pImageInfo=cubes;vkUpdateDescriptorSets(t->device,1,&write,0,NULL);}
}
static VkResult draw_full(ChVkTarget *t, const float rgba[4],
    const void *vertices, uint32_t vertex_count,
    const uint16_t *indices, uint32_t index_count, const Nv2aRenderState *state,
    const ChVkTexture *texture, uint32_t *samples, const Nv2aCombiner *rc, uint32_t fog_color, const ChVkTexture *const stage_images[4], const ChVkEffect *effect, int coverage_alpha, void *pixels, VkDeviceSize bytes, const ChVkFixedParams *fixed) {
    if(!t || t->failed || !vertices || !indices || !vertex_count || !index_count ||
       index_count%3 || !pixels || bytes<t->bytes)
        return VK_ERROR_INITIALIZATION_FAILED;
    for(uint32_t i=0;i<index_count;i++) if(indices[i]>=vertex_count)
        return VK_ERROR_INITIALIZATION_FAILED;
    if(samples && !t->queries) return VK_ERROR_FEATURE_NOT_PRESENT;
    if(rc && texture) return VK_ERROR_FEATURE_NOT_PRESENT;
    if(effect && (effect->kind<1 || effect->kind>2 || rc || texture)) return VK_ERROR_FEATURE_NOT_PRESENT;
    if(texture && ch_vk_texture_is_cube(texture)) return VK_ERROR_FEATURE_NOT_PRESENT;
    t->effect=effect!=NULL;
    if(effect) t->push.effect=*effect; else memset(&t->push.effect,0,sizeof(t->push.effect));
    t->rc=rc;t->fixed=fixed;t->stage_images=stage_images;
    t->vertex_offset=fixed?(256+sizeof(*fixed)+255)/256*256:rc?256:0;
    size_t vertex_stride=fixed?sizeof(ChVkFixedVertex):sizeof(ChVkColorVertex);
    if(rc && !t->dummy) {
        const uint32_t white=0xffffffff;
        VkResult r=ch_vk_target_upload_texture(t,1,1,&white,&t->dummy);
        if(r!=VK_SUCCESS) return r;
    }
    if(rc && !t->dummy_cube) {
        const uint32_t white[6]={0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff};
        VkResult r=ch_vk_target_upload_cube(t,1,white,&t->dummy_cube);
        if(r!=VK_SUCCESS) return r;
    }
    t->count_requested=samples!=NULL;
    VkResult result;
    if(!state) return VK_ERROR_INITIALIZATION_FAILED;
    int batched=t->batch_enabled && t->reuse_resources && t->defer_color_readback && !samples && !rgba && t->initialized;
    if((!batched || t->pending==BATCH_DRAWS) && t->pending) TRY(ch_vk_target_finish(t));
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
    /* Like the reference's retained GL programs/state, keep bounded native
     * pipelines across nonconsecutive state changes. Descriptor data is still
     * rewritten for every draw, after the previous synchronous fence. */
    PipelineKey key={0};
    key.blend=blend;
    key.depth=depth_state; key.cull=cull; key.front=front;
    key.shader=effect?3:rc?2:texture?1:0;key.fixed=fixed!=NULL;
    key.alpha_shader=!!state->alpha_test_enable || !t->split_alpha;
    key.specialized=key.shader==2 && t->specialize_rc;
    if(key.specialized) ch_vk_combiner_program(rc,&key.program);
    PipelineResources *entry=NULL,*oldest=NULL;
    unsigned capacity=t->reuse_resources?(t->specialize_rc?PIPELINE_SLOTS:BASE_PIPELINE_SLOTS):1;
    for(unsigned i=0;i<capacity;i++) {
        PipelineResources *p=&t->pipelines[i];
        if(p->pipeline && !memcmp(&key,&p->key,sizeof key)) {entry=p;break;}
        if(!oldest || p->used<oldest->used) oldest=p;
    }
    if(entry) {t->resource_stats.pipeline_hits++;if(!key.alpha_shader) t->resource_stats.no_alpha_pipeline_hits++;
        if(key.specialized) t->resource_stats.specialized_pipeline_hits++;}
    else {
        entry=oldest;
        if(entry->pipeline) {
            if(entry->pending) TRY(ch_vk_target_finish(t));
            t->resource_stats.pipeline_evictions++;
        }
        destroy_pipeline(t,entry);t->active_pipeline=entry;
        const char *pipeline_log=getenv("CHIHIRO_VULKAN_PIPELINE_LOG");
        int log_pipeline=pipeline_log && atoi(pipeline_log);
        if(log_pipeline) {
            fprintf(stderr,"[Vulkan pipeline begin] target=%ux%u shader=%u fixed=%u specialized=%u alpha=%u control=%08x\n",
                t->width,t->height,key.shader,key.fixed,key.specialized,key.alpha_shader,key.program.control);
            fflush(stderr);
        }
        uint64_t create_start=time_ns();
        result=create_pipeline(t,&blend,constants,&depth_state,cull,front,texture,&key);
        if(result!=VK_SUCCESS) return result;
        uint64_t create_ns=time_ns()-create_start;t->resource_stats.pipeline_create_ns+=create_ns;
        if(log_pipeline) fprintf(stderr,"[Vulkan pipeline end] %.3fms\n",create_ns/1e6);
        if(create_ns>t->resource_stats.pipeline_create_max_ns) t->resource_stats.pipeline_create_max_ns=create_ns;
        entry->key=key;t->resource_stats.pipeline_builds++;
        if(!key.alpha_shader) t->resource_stats.no_alpha_pipeline_builds++;
        if(key.specialized) t->resource_stats.specialized_pipeline_builds++;
    }
    memcpy(t->blend_constants,constants,sizeof constants);
    t->active_pipeline=entry;entry->used=++t->pipeline_clock;
    VkDeviceSize header=t->fixed?(256+sizeof(*t->fixed)+255)/256*256:t->rc?256:0;
    VkDeviceSize payload=header+(VkDeviceSize)vertex_count*vertex_stride+(VkDeviceSize)index_count*sizeof(*indices);
    VkDeviceSize base=t->pending?(t->geometry_used+255)/256*256:0;
    if(base>UINT64_MAX-payload) {result=VK_ERROR_OUT_OF_HOST_MEMORY;goto fail;}
    if(t->pending && base+payload>t->geometry_capacity) {TRY(ch_vk_target_finish(t));base=0;}
    if(entry->has_descriptors) {
        if(entry->set_used==entry->set_count) {
            VkDescriptorSetAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool=entry->descriptor_pool,.descriptorSetCount=1,.pSetLayouts=&entry->descriptor_layout};
            TRY(vkAllocateDescriptorSets(t->device,&alloc,&entry->sets[entry->set_count]));entry->set_count++;
        }
        entry->descriptor_set=entry->sets[entry->set_used++];
    }
    t->vertex_offset=base+header;
    update_images(t,texture);
    VkDeviceSize offset=t->vertex_offset+(VkDeviceSize)vertex_count*vertex_stride;
    VkDeviceSize size=offset+(VkDeviceSize)index_count*sizeof(*indices);
    if(size>SIZE_MAX) return VK_ERROR_OUT_OF_HOST_MEMORY;
    TRY(reserve_geometry(t,size));
    VkBuffer buffer=t->geometry;
    void *mapped=t->geometry_map;
    if(rc) {
        memset((char *)mapped+base,0,256); memcpy((char *)mapped+base,rc,sizeof(*rc));
        float *fog=(float *)((char *)mapped+base+224);
        fog[0]=((fog_color>>16)&255)/255.f; fog[1]=((fog_color>>8)&255)/255.f;
        fog[2]=(fog_color&255)/255.f; fog[3]=(fog_color>>24)/255.f;
        uint32_t *on=(uint32_t *)((char *)mapped+base+240);
        for(unsigned i=0;i<4;i++) on[i]=stage_images && stage_images[i]?
            (ch_vk_texture_is_cube(stage_images[i])?3:1):0;
        VkDescriptorBufferInfo info={buffer,base,256};
        VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=t->active_pipeline->descriptor_set,
            .dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&info};
        vkUpdateDescriptorSets(t->device,1,&write,0,NULL);
    }
    if(fixed) {
        memcpy((char *)mapped+base+256,fixed,sizeof *fixed);
        VkDescriptorBufferInfo info={buffer,base+256,sizeof *fixed};
        VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=t->active_pipeline->descriptor_set,
            .dstBinding=3,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&info};
        vkUpdateDescriptorSets(t->device,1,&write,0,NULL);
    }
    memcpy((char *)mapped+t->vertex_offset,vertices,(size_t)vertex_count*vertex_stride);
    memcpy((char *)mapped+offset,indices,(size_t)(size-offset));
    if(!t->geometry_coherent) {
        VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(t->physical,&properties);
        VkDeviceSize atom=properties.limits.nonCoherentAtomSize;
        VkDeviceSize flushed=(size+atom-1)/atom*atom;
        VkMappedMemoryRange flush={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory=t->geometry_memory,.size=flushed<=t->geometry_capacity?flushed:VK_WHOLE_SIZE};
        TRY(vkFlushMappedMemoryRanges(t->device,1,&flush));
    }
    if(batched) {
        result=record_batch(t,buffer,offset,index_count);t->geometry_used=size;
    } else result=render_read(t,rgba,rgba?7:0,1.0f,0,buffer,offset,index_count,pixels,bytes);
    if(result==VK_SUCCESS && samples) *samples=(uint32_t)t->samples;
fail:
    if(t->failed) vkQueueWaitIdle(t->queue);
    if(!t->pending) for(unsigned i=0;i<PIPELINE_SLOTS;i++) t->pipelines[i].set_used=0;
    if(!t->reuse_resources || t->failed) destroy_geometry(t);
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
VkResult ch_vk_target_upload_texture_mips(ChVkTarget *t,uint32_t width,uint32_t height,
    const uint32_t *argb,uint32_t levels,ChVkTexture **out) {
    if(!t || t->failed) return VK_ERROR_INITIALIZATION_FAILED;
    return ch_vk_texture_upload_mips(t->physical,t->device,t->queue,t->family,width,height,argb,levels,out);
}
VkResult ch_vk_target_upload_cube(ChVkTarget *t,uint32_t size,const uint32_t *faces,ChVkTexture **out) {
    if(!t || t->failed) return VK_ERROR_INITIALIZATION_FAILED;
    return ch_vk_texture_upload_cube(t->physical,t->device,t->queue,t->family,size,faces,out);
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
VkFence ch_vk_target_completion_fence(const ChVkTarget *t) { return t && !t->pending?t->fence:VK_NULL_HANDLE; }
VkResult ch_vk_target_export_image(ChVkTarget *t,VkImage destination,void *verify,VkDeviceSize bytes) {
    if(!t || t->failed || !t->initialized || !destination || destination==t->image ||
        (verify && bytes<t->bytes)) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result;
    TRY(ch_vk_target_finish(t));
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
    TRY(ch_vk_target_finish(t));
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

static VkResult transfer_depth_stencil(ChVkTarget *t,void *depth,void *stencil,size_t pixels,int upload,int packed) {
    if(!t || t->failed || !t->initialized || !depth || (!packed && !stencil) || pixels<t->bytes/4)
        return VK_ERROR_INITIALIZATION_FAILED;
    if(packed && t->depth_format!=VK_FORMAT_D24_UNORM_S8_UINT) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if(upload && !packed) for(size_t i=0;i<t->bytes/4;i++) {
        float d=((const float *)depth)[i];
        if(!isfinite(d) || d<0 || d>1) return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkResult result; void *mapped;
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=t->read_mem,.size=VK_WHOLE_SIZE};
    if(!upload && t->pending) {
        TRY(finish_batch(t,1));
        goto readback;
    }

    if(upload) {
        TRY(vkMapMemory(t->device,t->read_mem,0,VK_WHOLE_SIZE,0,&mapped));
        if(packed) {
            uint32_t *z=mapped;uint8_t *s=(uint8_t *)mapped+t->bytes;
            const uint32_t *words=depth;
            for(size_t i=0;i<t->bytes/4;i++) {z[i]=words[i]>>8;s[i]=(uint8_t)words[i];}
        } else if(t->depth_format==VK_FORMAT_D24_UNORM_S8_UINT) {
            uint32_t *z=mapped;
            for(size_t i=0;i<t->bytes/4;i++)
                z[i]=(uint32_t)((double)((const float *)depth)[i]*16777215.0+0.5);
        } else memcpy(mapped,depth,(size_t)t->bytes);
        if(!packed) memcpy((char *)mapped+t->bytes,stencil,(size_t)t->bytes/4);
        result=vkFlushMappedMemoryRanges(t->device,1,&range);
        vkUnmapMemory(t->device,t->read_mem);
        if(result!=VK_SUCCESS) goto fail;
    }
    TRY(ch_vk_target_finish(t));
    TRY(vkResetCommandBuffer(t->command,0)); TRY(vkResetFences(t->device,1,&t->fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vkBeginCommandBuffer(t->command,&begin));
    record_depth_transfer(t,upload);
    TRY(vkEndCommandBuffer(t->command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&t->command};
    TRY(vkQueueSubmit(t->queue,1,&submit,t->fence));
    TRY(vkWaitForFences(t->device,1,&t->fence,VK_TRUE,5000000000ULL));
readback:
    if(!upload) {
        TRY(vkMapMemory(t->device,t->read_mem,0,VK_WHOLE_SIZE,0,&mapped));
        result=vkInvalidateMappedMemoryRanges(t->device,1,&range);
        if(result==VK_SUCCESS) {
            if(packed) {
                const uint32_t *z=mapped;const uint8_t *s=(const uint8_t *)mapped+t->bytes;
                uint32_t *words=depth;
                for(size_t i=0;i<t->bytes/4;i++) words[i]=((z[i]&0xffffffu)<<8)|s[i];
            } else if(t->depth_format==VK_FORMAT_D24_UNORM_S8_UINT) {
                const uint32_t *z=mapped;
                for(size_t i=0;i<t->bytes/4;i++)
                    ((float *)depth)[i]=(float)((double)(z[i]&0xffffffu)/16777215.0);
            } else memcpy(depth,mapped,(size_t)t->bytes);
            if(!packed) memcpy(stencil,(char *)mapped+t->bytes,(size_t)t->bytes/4);
        }
        vkUnmapMemory(t->device,t->read_mem);
        if(result!=VK_SUCCESS) goto fail;
    }
    return VK_SUCCESS;
fail:
    t->failed=1; return result;
}
VkResult ch_vk_target_read_depth_stencil(ChVkTarget *t,float *depth,uint8_t *stencil,size_t pixels) {
    return transfer_depth_stencil(t,depth,stencil,pixels,0,0);
}
VkResult ch_vk_target_write_depth_stencil(ChVkTarget *t,const float *depth,const uint8_t *stencil,size_t pixels) {
    return transfer_depth_stencil(t,(void *)depth,(void *)stencil,pixels,1,0);
}
VkResult ch_vk_target_read_z24s8(ChVkTarget *t,uint32_t *words,size_t pixels) {
    return transfer_depth_stencil(t,words,NULL,pixels,0,1);
}
VkResult ch_vk_target_write_z24s8(ChVkTarget *t,const uint32_t *words,size_t pixels) {
    return transfer_depth_stencil(t,(void *)words,NULL,pixels,1,1);
}

VkResult ch_vk_target_copy_depth_stencil(ChVkTarget *source,ChVkTarget *destination) {
    ChVkTarget *s=source,*t=destination;
    if(!s || !t || s->failed || t->failed || !s->initialized || !t->initialized ||
       s->device!=t->device || s->queue!=t->queue || s->family!=t->family ||
       s->width!=t->width || s->height!=t->height || s->depth_format!=t->depth_format)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result;
    TRY(ch_vk_target_finish(s));
    if(s==t) return VK_SUCCESS;
    TRY(ch_vk_target_finish(t));
    TRY(vkResetCommandBuffer(t->command,0));TRY(vkResetFences(t->device,1,&t->fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vkBeginCommandBuffer(t->command,&begin));
    VkImageMemoryBarrier barriers[2];
    for(unsigned i=0;i<2;i++) barriers[i]=(VkImageMemoryBarrier){
        .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask=i?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .newLayout=i?VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=i?t->depth:s->depth,.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1}};
    VkPipelineStageFlags depth_stages=VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    vkCmdPipelineBarrier(t->command,depth_stages,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,2,barriers);
    VkImageCopy regions[2];
    for(unsigned i=0;i<2;i++) regions[i]=(VkImageCopy){
        .srcSubresource={i?VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1},
        .dstSubresource={i?VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1},
        .extent={t->width,t->height,1}};
    vkCmdCopyImage(t->command,s->depth,barriers[0].newLayout,t->depth,barriers[1].newLayout,2,regions);
    for(unsigned i=0;i<2;i++) {
        barriers[i].srcAccessMask=barriers[i].dstAccessMask;
        barriers[i].dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barriers[i].oldLayout=barriers[i].newLayout;
        barriers[i].newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TRANSFER_BIT,depth_stages,0,0,NULL,0,NULL,2,barriers);
    TRY(vkEndCommandBuffer(t->command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&t->command};
    TRY(vkQueueSubmit(t->queue,1,&submit,t->fence));
    TRY(vkWaitForFences(t->device,1,&t->fence,VK_TRUE,5000000000ULL));
    return VK_SUCCESS;
fail:
    s->failed=t->failed=1;return result;
}

VkResult ch_vk_target_draw_combiner_read(ChVkTarget *t,const float rgba[4],const ChVkColorVertex *v,uint32_t n,
    const uint16_t *indices,uint32_t count,const Nv2aRenderState *state,const ChVkTexture *texture,
    uint32_t *samples,const Nv2aCombiner *rc,uint32_t fog,const ChVkTexture *const stages[4],void *pixels,VkDeviceSize bytes) {
    return draw_full(t,rgba,v,n,indices,count,state,texture,samples,rc,fog,stages,NULL,0,pixels,bytes,NULL);
}
VkResult ch_vk_target_draw_effect_read(ChVkTarget *t,const ChVkColorVertex *v,uint32_t n,
    const uint16_t *indices,uint32_t count,const Nv2aRenderState *state,const ChVkEffect *effect,void *pixels,VkDeviceSize bytes) {
    if(!effect) return VK_ERROR_INITIALIZATION_FAILED;
    return draw_full(t,NULL,v,n,indices,count,state,NULL,NULL,NULL,0,NULL,effect,0,pixels,bytes,NULL);
}

VkResult ch_vk_target_draw_panel_read(ChVkTarget *t,const ChVkColorVertex *v,uint32_t n,
    const uint16_t *indices,uint32_t count,const Nv2aRenderState *state,const ChVkTexture *texture,
    const Nv2aCombiner *rc,uint32_t fog,const ChVkTexture *const stages[4],int coverage_alpha,void *pixels,VkDeviceSize bytes) {
    if(coverage_alpha!=0 && coverage_alpha!=1) return VK_ERROR_INITIALIZATION_FAILED;
    return draw_full(t,NULL,v,n,indices,count,state,texture,NULL,rc,fog,stages,NULL,coverage_alpha,pixels,bytes,NULL);
}

VkResult ch_vk_target_draw_fixed_read(ChVkTarget *t,const ChVkFixedParams *fixed,
    const ChVkFixedVertex *v,uint32_t n,const uint16_t *indices,uint32_t count,
    const Nv2aRenderState *state,const ChVkTexture *texture,uint32_t *samples,
    const Nv2aCombiner *rc,uint32_t fog,const ChVkTexture *const stages[4],void *pixels,VkDeviceSize bytes) {
    if(!fixed || !fixed->surface[0] || !fixed->surface[1] || !(fixed->depth[1]>fixed->depth[0]))
        return VK_ERROR_INITIALIZATION_FAILED;
    return draw_full(t,NULL,v,n,indices,count,state,texture,samples,rc,fog,stages,NULL,0,pixels,bytes,fixed);
}
