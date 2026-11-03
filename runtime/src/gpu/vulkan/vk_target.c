#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* dladdr (CHIHIRO_VULKAN_WAIT_STACKS) */
#endif
#include "vk_target.h"
#include "vk_depth_quant.h"
#include "vk_depth_pack.h"
#include "vk_image_read.h"
#include "vk_color_pack.h"
#include "vk_depth_quant_math.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include "color_spv.h"
#include "fixed_layout_assert.h"
#include "vk_combiner_program.h"
#include "combiner_program_assert.h"
typedef struct {
    VkPipelineColorBlendAttachmentState blend;
    VkPipelineDepthStencilStateCreateInfo depth;
    VkCullModeFlags cull;
    VkFrontFace front;
    unsigned shader,fixed,alpha_shader,specialized,fragment_depth;
    ChVkCombinerProgram program;
    uint32_t sample_static,sample_types;
    uint32_t vertex_count,vertex_program[136][4],vertex_fetch_specialized,vertex_formats[16];
    /* IDs match fixed.vert; numeric matrices/materials remain dynamic. */
    uint32_t fixed_fetch[25];
} PipelineKey;
_Static_assert(offsetof(PipelineKey,vertex_program)==offsetof(PipelineKey,vertex_count)+4,"vertex specialization layout");
_Static_assert(offsetof(PipelineKey,sample_static)==offsetof(PipelineKey,program)+sizeof(ChVkCombinerProgram),"fragment fetch specialization layout");
_Static_assert(offsetof(PipelineKey,vertex_fetch_specialized)==offsetof(PipelineKey,vertex_count)+545*4,"vertex fetch specialization layout");
#define BASE_PIPELINE_SLOTS 32u
#define PIPELINE_SLOTS 128u
/* Bound descriptor/snapshot lifetimes without forcing a wait every 64 draws.
 * Queries, guest publication, resource pressure and target changes still finish. */
#define BATCH_DRAWS CH_VK_MAX_BATCH_DRAWS
#define RING_SLOTS 4
typedef struct {ChVkTexture *image;void (*release)(void *,ChVkTexture *);void *context;int owned;} Retired;
typedef struct {
    PipelineKey key;
    uint64_t used;
    VkDescriptorSetLayout descriptor_layout;
    VkDescriptorPool descriptor_pool;
    /* One bank per batch slot: an in-flight batch keeps its sets. */
    VkDescriptorSet descriptor_set,sets[RING_SLOTS][BATCH_DRAWS];
    uint32_t set_count[RING_SLOTS],set_used;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    int has_descriptors;
    unsigned pending; /* Bit per batch slot referencing it until that slot's fence completes. */
} PipelineResources;
struct ChVkTarget {
    uint32_t family;
    VkPhysicalDevice physical;
    VkRenderPass pass;
    VkImageView view;
    VkFramebuffer framebuffer;
    /* foveation: a rate image and framebuffer per dynamic-resolution level */
    VkImage rate_image[4]; VkDeviceMemory rate_memory[4]; VkImageView rate_view[4]; VkFramebuffer fov_fb[4];
    const ChVkTexture *const *stage_images;
    ChVkTexture *dummy,*dummy_cube;
    const Nv2aCombiner *rc;
    const ChVkFixedParams *fixed;
    VkDeviceSize vertex_offset;
    PipelineResources pipelines[PIPELINE_SLOTS], *active_pipeline;
    unsigned pipeline_extent; /* Initialized prefix, including failed creations. */
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
    VkDeviceSize noncoherent_atom;
    ChVkTargetResourceStats resource_stats;
    VkDevice device;
    VkQueue queue;
    VkImage depth;
    VkFormat depth_format;
    ChVkDepthQuant *depth_quant,*depth_quant_draw,*depth_quant_input,*depth_quant_inline;
    int quantized_depth,input_pass,input_depth,packed_transfer,depth_morton;
    ChVkDepthPack *depth_pack;
    ChVkImageRead *depth_read,*color_read,*paired_color_read,*depth_color_read;
    int sampled_color_read,sampleable;
    int sampled_depth_read;
    ChVkColorPack *color_pack,*paired_color_pack;
    int specialize_fetch;
    int color_bgra; /* 0 RGBA, 1 linear BGRA, 2 rectangular Morton BGRA */
    VkDeviceMemory depth_mem;
    VkImageView depth_view;
    VkImage image;
    VkDeviceMemory image_mem, read_mem, paired_color_mem;
    void *read_map,*paired_color_map;
    VkDeviceSize read_allocation,paired_color_allocation;
    int read_coherent,paired_color_coherent;
    VkBuffer paired_color;
    VkBuffer read;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkFence fence;
    VkCommandBuffer upload_command;
    VkFence upload_fence;
    int upload_pending,async_depth_uploads,async_depth_copies;
    /* One queued copy edge; GPU ownership lasts through its source fence.
     * A new copy drains existing edges, so this graph cannot form a cycle. */
    ChVkTarget *copy_source,*copy_destination;
    /* A texture snapshot/refresh was submitted with this batch without a host
     * wait: the copy is ordered before later work on the shared queue, and
     * this target retires the batch before it next records or reads. */
    int deferred_retire;
    int color_readback_active,color_readback_batched;
    void *color_readback_pixels;
    uint32_t width, height;
    VkDeviceSize bytes;
    VkImageLayout layout;
    VkQueryPool queries,timing;
    float timestamp_period;
    uint32_t timestamp_bits;
    uint64_t timing_start,timing_gpu,timing_cpu,timing_calls;
    ChVkTargetTimingStats timing_stats,batch_report;
    uint64_t batch_report_start;
    uint64_t depth_uploads,depth_upload_ns,depth_upload_wait_ns;
    int count_requested;
    uint64_t samples;
    struct { uint32_t alpha[3]; ChVkEffect effect; } push,recorded_push;
    VkPipeline recorded_pipeline;
    VkPipelineLayout recorded_push_layout;
    float recorded_blend[4];
    int recorded_blend_valid,recorded_stencil_valid;
    uint32_t stencil_reference,recorded_stencil_reference;
    int effect;
    int defer_color_readback;
    int submitted,readback_active,readback_batched;
    uint64_t submitted_finish_start;
    struct {void *depth,*stencil,*rgba;size_t pixels,color_bytes;int packed;} readback_request;
    int batch_enabled;
    uint32_t batch_limit;
    uint32_t pending,pending_clears,retired_count;
    VkDeviceSize geometry_used;
    Retired *retired;
    /* Batch ring: the recording batch's command buffer, fence, timing pool,
     * geometry arena and retired samples live in the fields above; the other
     * slot keeps its own while a submitted batch is in flight. An in-flight
     * slot is retired before any CPU read of this target's results. */
    int ring;unsigned slot;
    struct {
        VkCommandBuffer command;VkFence fence;VkQueryPool timing;
        VkBuffer geometry;VkDeviceMemory geometry_memory;void *geometry_map;
        VkDeviceSize geometry_capacity;int geometry_coherent;
        Retired *retired;uint32_t retired_count;
        int inflight,reason,tail_stamped;uint64_t finish_start;
    } slots[RING_SLOTS];
    int initialized;
    int failed;
    struct { const char *caller; int readback; uint64_t count,wait_ns,gpu_ns,tail_ns; } finish_reasons[32];
    int tail_stamped; /* timing slot 2: the render pass ended, readbacks/copies follow */
    /* CHIHIRO_VULKAN_DRAW_TIMING=1: a timestamp after every batched draw. */
    VkQueryPool draw_pool[RING_SLOTS]; uint32_t draw_n[RING_SLOTS];
    struct {uint8_t bucket;uint32_t indices;} draw_meta[RING_SLOTS][BATCH_DRAWS+2];
    int submitted_reason;
    /* Lending (ch_vk_target_lend_texture): the target whose batch samples this
     * image in place, and as a reader the images its batch holds. A lent
     * image is SHADER_READ_ONLY only inside the reader's batch: the reader
     * moves it there before its render pass and back to TRANSFER_SRC after. */
    struct ChVkTarget *live_reader;
    struct {struct ChVkTarget *source;int recorded;} live[8]; unsigned live_count;
    struct {uint32_t key[12];ChVkTexture *texture;} lent[8]; unsigned lent_count;
};
static VkResult finish_batch_named(ChVkTarget *,int,const char *);
static void live_release(ChVkTarget *);
static VkFramebuffer target_framebuffer(const ChVkTarget *);   /* foveation level's */
static VkResult drain_ring(ChVkTarget *);
static VkResult wait_copy_source(ChVkTarget *);
static VkResult retire_copy_out(ChVkTarget *);
static void record_depth_copy(VkCommandBuffer,ChVkTarget *,ChVkTarget *);
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
    fprintf(stderr,"[Vulkan batch boundaries] %ux%u:",t->width,t->height);
    for(unsigned i=0;i<32;i++) if(t->finish_reasons[i].count) {
        fprintf(stderr," %s/read%d=%llu",t->finish_reasons[i].caller,t->finish_reasons[i].readback,
            (unsigned long long)t->finish_reasons[i].count);
    }
    for(unsigned i=0;i<32;i++) if(t->finish_reasons[i].count) {
        fprintf(stderr,"\n[Vulkan boundary costs] %ux%u %s/read%d count=%llu wait_ms=%.3f gpu_ms=%.3f tail_ms=%.3f",
            t->width,t->height,t->finish_reasons[i].caller,t->finish_reasons[i].readback,
            (unsigned long long)t->finish_reasons[i].count,
            t->finish_reasons[i].wait_ns/1e6,t->finish_reasons[i].gpu_ns/1e6,t->finish_reasons[i].tail_ns/1e6);
        t->finish_reasons[i].count=t->finish_reasons[i].wait_ns=t->finish_reasons[i].gpu_ns=t->finish_reasons[i].tail_ns=0;
    }
    fputc('\n',stderr);
    fprintf(stderr,"[Vulkan depth uploads] %ux%u count=%llu inclusive %.2fms own_fence_wait %.2fms\n",
        t->width,t->height,(unsigned long long)t->depth_uploads,t->depth_upload_ns/1e6,t->depth_upload_wait_ns/1e6);
    t->depth_uploads=t->depth_upload_ns=t->depth_upload_wait_ns=0;
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
/* Readback allocations are read by the CPU every guest completion. Prefer
 * cached memory even when noncoherent; callers explicitly flush/invalidate.
 * The first host-visible type may be uncached on unified-memory devices. */
static int readback_memory_type(VkPhysicalDevice gpu,uint32_t bits,uint32_t *index) {
    return memory_type(gpu,bits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT,index) ||
        memory_type(gpu,bits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,index);
}
static int memory_coherent(VkPhysicalDevice gpu,uint32_t index) {
    /* Exercise explicit range maintenance on coherent development devices. */
    const char *force=getenv("CHIHIRO_VULKAN_FORCE_STAGING_MAINTENANCE");
    if(force && atoi(force)) return 0;
    VkPhysicalDeviceMemoryProperties p;vkGetPhysicalDeviceMemoryProperties(gpu,&p);
    return !!(p.memoryTypes[index].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}
/* CPU accesses only the requested packed plane. Keep allocation-wide mapping,
 * but maintain only intersecting cache atoms on noncoherent memory. Fences and
 * GPU barriers remain required even when host cache maintenance is a no-op. */
static VkResult maintain_staging(ChVkTarget *t,int paired,int upload,VkDeviceSize offset,VkDeviceSize bytes) {
    VkDeviceSize allocation=paired?t->paired_color_allocation:t->read_allocation;
    if(offset>allocation || bytes>allocation-offset) return VK_ERROR_INITIALIZATION_FAILED;
    if(paired?t->paired_color_coherent:t->read_coherent) {
        t->resource_stats.coherent_staging_skips++;return VK_SUCCESS;
    }
    VkDeviceSize atom=t->noncoherent_atom,start=offset-offset%atom,end=offset+bytes;
    VkDeviceSize padding=(atom-end%atom)%atom;
    end=padding>allocation-end?allocation:end+padding;
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory=paired?t->paired_color_mem:t->read_mem,.offset=start,.size=end-start};
    t->resource_stats.staging_maintenance_bytes+=range.size;
    return upload?vkFlushMappedMemoryRanges(t->device,1,&range):vkInvalidateMappedMemoryRanges(t->device,1,&range);
}
/* Upload command storage is independent of the draw command. Same-queue
 * barriers order GPU use; the host fence guards staging/command reuse. */
static VkResult wait_upload(ChVkTarget *t) {
    if(!t->upload_pending) return VK_SUCCESS;
    uint64_t start=t->timing?time_ns():0;
    VkResult r=vkWaitForFences(t->device,1,&t->upload_fence,VK_TRUE,5000000000ULL);
    if(start) {
        uint64_t elapsed=time_ns()-start;
        t->depth_upload_wait_ns+=elapsed;t->timing_stats.depth_upload_wait_ns+=elapsed;
    }
    if(r==VK_SUCCESS) t->upload_pending=0;else t->failed=1;
    return r;
}
/* Keep staging mapped until target destruction. Mapping does not synchronize
 * GPU access: existing fence waits and flush/invalidate operations still do. */
static VkResult map_readback(ChVkTarget *t,int paired,void **mapped) {
    VkResult ready=wait_upload(t);if(ready!=VK_SUCCESS) return ready;
    void **slot=paired?&t->paired_color_map:&t->read_map;
    if(!*slot) {
        void *address=NULL;
        VkResult r=vkMapMemory(t->device,paired?t->paired_color_mem:t->read_mem,
            0,VK_WHOLE_SIZE,0,&address);
        if(r!=VK_SUCCESS) return r;
        *slot=address;
    }
    *mapped=*slot;return VK_SUCCESS;
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
        .usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
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
static void retire_retired(Retired *,uint32_t *);
static void retire_samples(ChVkTarget *t) {retire_retired(t->retired,&t->retired_count);}
uint32_t ch_vk_target_pending(const ChVkTarget *t) {return t?t->pending:0;}
VkResult ch_vk_target_batch_limit(ChVkTarget *t,uint32_t draws) {
    if(!t || t->failed || t->resource_stats.pipeline_builds || draws>BATCH_DRAWS) return VK_ERROR_INITIALIZATION_FAILED;
    t->batch_limit=draws?draws:BATCH_DRAWS;return VK_SUCCESS;
}
VkResult ch_vk_target_batching(ChVkTarget *t,int enabled) {
    if(!t || t->failed || (enabled && !t->batch_enabled && t->resource_stats.pipeline_builds)) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=finish_batch_named(t,0,__func__);if(r==VK_SUCCESS) t->batch_enabled=!!enabled;return r;
}
VkResult ch_vk_target_retire_sample(ChVkTarget *t,ChVkTexture *image,
    void (*release)(void *,ChVkTexture *),void *context,int owned) {
    if(!image) return VK_SUCCESS;
    VkResult r=VK_SUCCESS;
    if(t && t->pending && t->retired_count==t->batch_limit*5) r=finish_batch_named(t,0,__func__);
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
    live_release(t);
    (void)wait_copy_source(t);(void)retire_copy_out(t);
    if(t->color_readback_active) (void)ch_vk_target_read_color_end(t);
    if(t->readback_active) (void)ch_vk_target_read_depth_end(t);
    /* Finish deferred work before destroying anything it references. */
    if(!t->failed) (void)finish_batch_named(t,0,__func__);
    if(t->failed) vkQueueWaitIdle(t->queue);
    if(t->ring) (void)drain_ring(t);
    retire_samples(t);
    for(unsigned i=0;i<t->live_count;i++) if(t->live[i].source->live_reader==t) t->live[i].source->live_reader=NULL;
    t->live_count=0;
    if(t->lent_count) {   /* readers' batches may still sample these views */
        vkQueueWaitIdle(t->queue);
        for(unsigned i=0;i<t->lent_count;i++) ch_vk_texture_destroy(t->lent[i].texture);
    }
    for(unsigned i=0;i<t->pipeline_extent;i++) destroy_pipeline(t,&t->pipelines[i]);
    if(t->ring) for(unsigned i=1;i<RING_SLOTS;i++) {
        unsigned o=(t->slot+i)%RING_SLOTS;
        if(t->slots[o].geometry) vkDestroyBuffer(t->device,t->slots[o].geometry,NULL);
        if(t->slots[o].geometry_memory) vkFreeMemory(t->device,t->slots[o].geometry_memory,NULL);
        if(t->slots[o].timing) vkDestroyQueryPool(t->device,t->slots[o].timing,NULL);
        if(t->slots[o].fence) vkDestroyFence(t->device,t->slots[o].fence,NULL);
    }
    for(unsigned k=0;k<RING_SLOTS;k++) free(t->slots[k].retired);
    if(t->pipeline_cache && !t->borrowed_pipeline_cache) vkDestroyPipelineCache(t->device,t->pipeline_cache,NULL);
    destroy_geometry(t);
    if(t->framebuffer) vkDestroyFramebuffer(t->device,t->framebuffer,NULL);
    if(t->view) vkDestroyImageView(t->device,t->view,NULL);
    if(t->pass) vkDestroyRenderPass(t->device,t->pass,NULL);
    for(int i=0;i<4;i++) {
        if(i && t->fov_fb[i]) vkDestroyFramebuffer(t->device,t->fov_fb[i],NULL);   /* [0] is t->framebuffer */
        if(t->rate_view[i]) vkDestroyImageView(t->device,t->rate_view[i],NULL);
        if(t->rate_image[i]) vkDestroyImage(t->device,t->rate_image[i],NULL);
        if(t->rate_memory[i]) vkFreeMemory(t->device,t->rate_memory[i],NULL);
    }
    ch_vk_texture_destroy(t->dummy);
    ch_vk_texture_destroy(t->dummy_cube);
    if(t->queries) vkDestroyQueryPool(t->device,t->queries,NULL);
    if(t->timing) vkDestroyQueryPool(t->device,t->timing,NULL);
    if(t->upload_fence) vkDestroyFence(t->device,t->upload_fence,NULL);
    if(t->fence) vkDestroyFence(t->device,t->fence,NULL);
    if(t->pool) vkDestroyCommandPool(t->device,t->pool,NULL);
    ch_vk_depth_quant_destroy(t->depth_quant_draw);
    ch_vk_depth_quant_destroy(t->depth_quant_input);
    ch_vk_depth_quant_destroy(t->depth_quant_inline);
    ch_vk_depth_quant_destroy(t->depth_quant);
    ch_vk_image_read_destroy(t->color_read);
    ch_vk_image_read_destroy(t->paired_color_read);
    ch_vk_color_pack_destroy(t->color_pack);
    ch_vk_color_pack_destroy(t->paired_color_pack);
    if(t->paired_color) vkDestroyBuffer(t->device,t->paired_color,NULL);
    if(t->paired_color_map) vkUnmapMemory(t->device,t->paired_color_mem);
    if(t->paired_color_mem) vkFreeMemory(t->device,t->paired_color_mem,NULL);
    ch_vk_image_read_destroy(t->depth_color_read);
    ch_vk_image_read_destroy(t->depth_read);
    ch_vk_depth_pack_destroy(t->depth_pack);
    if(t->read) vkDestroyBuffer(t->device,t->read,NULL);
    if(t->read_map) vkUnmapMemory(t->device,t->read_mem);
    if(t->read_mem) vkFreeMemory(t->device,t->read_mem,NULL);
    if(t->depth_view) vkDestroyImageView(t->device,t->depth_view,NULL);
    if(t->depth) vkDestroyImage(t->device,t->depth,NULL);
    if(t->depth_mem) vkFreeMemory(t->device,t->depth_mem,NULL);
    if(t->image) vkDestroyImage(t->device,t->image,NULL);
    if(t->image_mem) vkFreeMemory(t->device,t->image_mem,NULL);
    free(t);
}
static VkResult create_depth(VkPhysicalDevice,VkDevice,VkQueue,uint32_t,uint32_t,uint32_t,VkFormat,int,ChVkTarget **);
VkResult ch_vk_target_create_input_depth(VkPhysicalDevice physical,VkDevice device,VkQueue queue,
    uint32_t family,uint32_t width,uint32_t height,ChVkTarget **out) {
    return create_depth(physical,device,queue,family,width,height,VK_FORMAT_D32_SFLOAT_S8_UINT,1,out);
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
    return create_depth(physical,device,queue,family,width,height,depth_format,0,out);
}
static VkResult create_depth(VkPhysicalDevice physical,VkDevice device,VkQueue queue,
    uint32_t family,uint32_t width,uint32_t height,VkFormat depth_format,int input_depth,ChVkTarget **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;
    if(depth_format!=VK_FORMAT_D32_SFLOAT_S8_UINT && depth_format!=VK_FORMAT_D24_UNORM_S8_UINT)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if(!width || !height || (uint64_t)width*height>SIZE_MAX/12 || (uint64_t)width*height>UINT32_MAX/3)
        return VK_ERROR_INITIALIZATION_FAILED;
    ChVkTarget *t=calloc(1,sizeof(*t));
    if(!t) return VK_ERROR_OUT_OF_HOST_MEMORY;
    t->batch_limit=BATCH_DRAWS;
    const char *async_upload=getenv("CHIHIRO_VULKAN_ASYNC_DEPTH_UPLOADS");
    t->async_depth_uploads=!async_upload || atoi(async_upload);
    const char *async_copy=getenv("CHIHIRO_VULKAN_ASYNC_DEPTH_COPIES");
    t->async_depth_copies=!async_copy || atoi(async_copy);
    VkPhysicalDeviceProperties device_properties;
    vkGetPhysicalDeviceProperties(physical,&device_properties);
    t->noncoherent_atom=device_properties.limits.nonCoherentAtomSize;
    t->family=family; t->physical=physical; t->device=device; t->queue=queue; t->width=width; t->height=height;
    ch_vk_vertex_program_loop_init(physical);
    const char *reuse=getenv("CHIHIRO_VULKAN_REUSE_RESOURCES");
    t->reuse_resources=!reuse || strcmp(reuse,"0");
    const char *split=getenv("CHIHIRO_VULKAN_NOAT");
    if(!split) split=getenv("VC3_VULKAN_NOAT");
    /* Keep opt-in: the first Quest pair regressed and warm repeats show no
     * benefit. A discard-free module is not automatically faster. */
    t->split_alpha=split && atoi(split);
    const char *fetch=getenv("CHIHIRO_VULKAN_SPECIALIZE_FETCH");
    /* Attribute formats and fixed-function modes as specialization constants:
     * the generic fixed-function vertex shader was ~2500-3400 instructions with
     * its decode loops left in (ir3 stats on the Steam Frame); specialized,
     * E.S. at native eye size went from ~18-19.7 to 16.7 ms a frame. Default
     * on; CHIHIRO_VULKAN_SPECIALIZE_FETCH=0: the generic shaders. */
    t->specialize_fetch=!fetch || atoi(fetch);
    const char *specialize=getenv("CHIHIRO_VULKAN_SPECIALIZE_RC");
    t->specialize_rc=!specialize || atoi(specialize);
    t->depth_format=depth_format;t->input_depth=input_depth;
    t->bytes=(VkDeviceSize)width*height*4;
    VkResult result;
    VkImageCreateInfo image={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
        .extent={width,height,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    const char *direct_color=getenv("CHIHIRO_VULKAN_DIRECT_COLOR_READ");
    {   /* Sampled usage: compute color reads, and other targets sampling this
         * image in place (ch_vk_target_lend_texture). */
        VkImageFormatProperties support;
        result=vkGetPhysicalDeviceImageFormatProperties(physical,image.format,image.imageType,
            image.tiling,image.usage|VK_IMAGE_USAGE_SAMPLED_BIT,image.flags,&support);
        if(result!=VK_ERROR_FORMAT_NOT_SUPPORTED) {
            TRY(result);
            t->sampleable=width<=support.maxExtent.width && height<=support.maxExtent.height &&
                (support.sampleCounts&VK_SAMPLE_COUNT_1_BIT);
            if(t->sampleable) image.usage|=VK_IMAGE_USAGE_SAMPLED_BIT;
            t->sampled_color_read=t->sampleable && (!direct_color || atoi(direct_color)) &&
                ((uint64_t)width*height+63)/64<=device_properties.limits.maxComputeWorkGroupCount[0];
        }
    }
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
    const char *direct_read=getenv("CHIHIRO_VULKAN_DIRECT_DEPTH_READ");
    t->sampled_depth_read=(!direct_read || atoi(direct_read)) &&
        depth_format==VK_FORMAT_D32_SFLOAT_S8_UINT &&
        (depth_properties.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
        ((uint64_t)width*height+63)/64<=device_properties.limits.maxComputeWorkGroupCount[0];
    image.format=depth_format;
    image.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if(input_depth) image.usage|=VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    if(t->sampled_depth_read) {
        VkImageFormatProperties support;
        result=vkGetPhysicalDeviceImageFormatProperties(physical,depth_format,image.imageType,
            image.tiling,image.usage|VK_IMAGE_USAGE_SAMPLED_BIT,image.flags,&support);
        if(result==VK_ERROR_FORMAT_NOT_SUPPORTED) t->sampled_depth_read=0;
        else {
            TRY(result);
            if(width>support.maxExtent.width || height>support.maxExtent.height ||
               !(support.sampleCounts&VK_SAMPLE_COUNT_1_BIT)) t->sampled_depth_read=0;
        }
        if(t->sampled_depth_read) image.usage|=VK_IMAGE_USAGE_SAMPLED_BIT;
    }
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
        .size=2*t->bytes+(t->bytes/4+3)/4*4,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    TRY(vkCreateBuffer(device,&buffer,NULL,&t->read));
    vkGetBufferMemoryRequirements(device,t->read,&req);
    mem.allocationSize=req.size;
    if(!readback_memory_type(physical,req.memoryTypeBits,
                    &mem.memoryTypeIndex)) {
        result=VK_ERROR_FEATURE_NOT_PRESENT; goto fail;
    }
    t->read_allocation=mem.allocationSize;t->read_coherent=memory_coherent(physical,mem.memoryTypeIndex);
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
    {const char *ring=getenv("CHIHIRO_VULKAN_BATCH_RING");t->ring=!ring || atoi(ring)!=0;}
    t->slots[0].retired=t->retired=calloc(BATCH_DRAWS*5,sizeof *t->retired);
    if(!t->retired) {result=VK_ERROR_OUT_OF_HOST_MEMORY;goto fail;}
    if(t->ring) {
        for(unsigned k=1;k<RING_SLOTS;k++) {
            t->slots[k].retired=calloc(BATCH_DRAWS*5,sizeof *t->retired);
            if(!t->slots[k].retired) {result=VK_ERROR_OUT_OF_HOST_MEMORY;goto fail;}
            TRY(vkAllocateCommandBuffers(device,&cmd,&t->slots[k].command));
            TRY(vkCreateFence(device,&fence,NULL,&t->slots[k].fence));
        }
    }
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
                .queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=3};
            TRY(vkCreateQueryPool(device,&timing,NULL,&t->timing));
            if(t->ring) for(unsigned k=1;k<RING_SLOTS;k++) TRY(vkCreateQueryPool(device,&timing,NULL,&t->slots[k].timing));
            const char *draws=getenv("CHIHIRO_VULKAN_DRAW_TIMING");
            if(draws && atoi(draws)) {
                VkQueryPoolCreateInfo per_draw=timing;per_draw.queryCount=BATCH_DRAWS+2;
                for(unsigned k=0;k<(t->ring?RING_SLOTS:1u);k++) TRY(vkCreateQueryPool(device,&per_draw,NULL,&t->draw_pool[k]));
            }
        }
    }
    *out=t; return VK_SUCCESS;
fail:
    ch_vk_target_destroy(t); return result;
}
static void color_read_layout(ChVkTarget *t,VkCommandBuffer command,int begin) {
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=begin?VK_ACCESS_TRANSFER_READ_BIT:VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask=begin?VK_ACCESS_SHADER_READ_BIT:VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout=begin?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout=begin?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=t->image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(command,begin?VK_PIPELINE_STAGE_TRANSFER_BIT:VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        begin?VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&b);
}
/* Returns whether the requested paired color read was recorded as well. */
static int record_depth_transfer(ChVkTarget *t,VkCommandBuffer command,int upload,int paired) {
    if(!upload && t->packed_transfer && t->depth_read) {
        /* Read the two attachment aspects directly: no intermediate depth or
         * stencil planes are materialized in the host-visible staging buffer. */
        VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstAccessMask=VK_ACCESS_SHADER_READ_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=t->depth,.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1}};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,0,NULL,1,&b);
        paired=paired && t->color_bgra && t->depth_color_read;
        if(paired) {
            color_read_layout(t,command,1);
            ch_vk_image_read_record_pair(t->depth_color_read,command,t->width,t->height,t->depth_morton,t->color_bgra==2);
            color_read_layout(t,command,0);
            t->resource_stats.direct_color_readbacks++;
            t->resource_stats.direct_paired_readbacks++;
        } else ch_vk_image_read_record(t->depth_read,command,t->width,t->height,t->depth_morton);
        b.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b.oldLayout=b.newLayout;b.newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,0,0,NULL,0,NULL,1,&b);
        t->resource_stats.direct_depth_readbacks++;
        return paired;
    }
    // Staging may previously have been read/written by the pack compute pass.
    VkBufferMemoryBarrier reuse={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=t->read,.size=VK_WHOLE_SIZE};
    if(t->depth_pack) vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,1,&reuse,0,NULL);
    if(t->packed_transfer && upload) ch_vk_depth_pack_record(t->depth_pack,command,t->width,t->height,1,t->depth_morton,t->depth_format==VK_FORMAT_D24_UNORM_S8_UINT);
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask=upload?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .newLayout=upload?VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=t->depth,.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkBufferImageCopy regions[2]={
        {.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1},.imageExtent={t->width,t->height,1}},
        {.bufferOffset=t->bytes,.imageSubresource={VK_IMAGE_ASPECT_STENCIL_BIT,0,0,1},.imageExtent={t->width,t->height,1}}};
    if(upload) vkCmdCopyBufferToImage(command,t->read,t->depth,barrier.newLayout,2,regions);
    else vkCmdCopyImageToBuffer(command,t->depth,barrier.newLayout,t->read,2,regions);
    barrier.srcAccessMask=upload?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout=barrier.newLayout; barrier.newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,0,0,NULL,0,NULL,1,&barrier);
    if(t->packed_transfer && !upload) ch_vk_depth_pack_record(t->depth_pack,command,t->width,t->height,0,t->depth_morton,t->depth_format==VK_FORMAT_D24_UNORM_S8_UINT);
    if(!upload) {
        VkBufferMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .buffer=t->read,.size=VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&host,0,NULL);
    }
    return 0;
}
/* readback: 0 none, 1 depth/stencil, 2 color. Staging storage is shared. */
static VkResult ensure_paired_color(ChVkTarget *t) {
    if(t->paired_color) return VK_SUCCESS;
    VkResult result;VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;
    VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=2*t->bytes,
        .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    TRY(vkCreateBuffer(t->device,&info,NULL,&buffer));
    VkMemoryRequirements req;vkGetBufferMemoryRequirements(t->device,buffer,&req);
    VkMemoryAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size};
    if(!readback_memory_type(t->physical,req.memoryTypeBits,&alloc.memoryTypeIndex)) {
        result=VK_ERROR_FEATURE_NOT_PRESENT;goto fail;
    }
    TRY(vkAllocateMemory(t->device,&alloc,NULL,&memory));
    TRY(vkBindBufferMemory(t->device,buffer,memory,0));
    t->paired_color_allocation=alloc.allocationSize;t->paired_color_coherent=memory_coherent(t->physical,alloc.memoryTypeIndex);
    t->paired_color=buffer;t->paired_color_mem=memory;return VK_SUCCESS;
fail:
    if(buffer) vkDestroyBuffer(t->device,buffer,NULL);
    if(memory) vkFreeMemory(t->device,memory,NULL);
    return result;
}
static void record_color_read(ChVkTarget *t,int paired) {
    ChVkImageRead *reader=paired?t->paired_color_read:t->color_read;
    VkBuffer buffer=paired?t->paired_color:t->read;
    if(t->color_bgra && reader) {
        color_read_layout(t,t->command,1);
        ch_vk_image_read_record(reader,t->command,t->width,t->height,t->color_bgra==2);
        color_read_layout(t,t->command,0);
        t->resource_stats.direct_color_readbacks++;
        return;
    }
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageExtent={t->width,t->height,1}};
    vkCmdCopyImageToBuffer(t->command,t->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
    if(t->color_bgra) ch_vk_color_pack_record(paired?t->paired_color_pack:t->color_pack,
        t->command,t->width,t->height,0,t->color_bgra==2);
    VkBufferMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=buffer,.size=VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&host,0,NULL);
}
static void record_paired_color(ChVkTarget *t) {record_color_read(t,1);}

static void color_staging_reuse_on(ChVkTarget *t,VkCommandBuffer command) {
    if(!t->color_pack) return;
    VkMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&b,0,NULL,0,NULL);
}
static void color_staging_reuse(ChVkTarget *t) {color_staging_reuse_on(t,t->command);}
#if defined(__linux__) && !defined(__ANDROID__)
#include <execinfo.h>
#include <dlfcn.h>
#include <stdio.h>
/* CHIHIRO_VULKAN_WAIT_STACKS=1 (desktop Linux): where the host blocks on a
 * batch's fence -- each wait's time summed by its call stack, the top ones
 * printed now and then as module offsets (addr2line -f -e <binary> ...). For
 * finding which guest-visible readbacks cost the most round trips. */
#define WAIT_STACK_DEPTH 12
static struct { void *pc[WAIT_STACK_DEPTH]; int n; uint64_t ns,count; } wait_stacks[512];
static int wait_stacks_on=-1; static uint64_t wait_stacks_total;
static void wait_stack_note(uint64_t ns) {
    if(wait_stacks_on<0) {const char *e=getenv("CHIHIRO_VULKAN_WAIT_STACKS"); wait_stacks_on=e && *e=='1';}
    if(!wait_stacks_on) return;
    void *pc[WAIT_STACK_DEPTH];int n=backtrace(pc,WAIT_STACK_DEPTH);
    unsigned h=0;for(int i=0;i<n;i++) h=h*31u+(unsigned)((uintptr_t)pc[i]>>2);
    for(unsigned k=0;k<512;k++) {
        unsigned s=(h+k)%512u;
        if(!wait_stacks[s].n) {memcpy(wait_stacks[s].pc,pc,sizeof(void *)*(size_t)n);wait_stacks[s].n=n;}
        if(wait_stacks[s].n==n && !memcmp(wait_stacks[s].pc,pc,sizeof(void *)*(size_t)n)) {
            wait_stacks[s].ns+=ns;wait_stacks[s].count++;break;
        }
    }
    static uint64_t every;
    if(!every) {const char *e=getenv("CHIHIRO_VULKAN_WAIT_STACKS_EVERY");every=e && atoi(e)>0?(uint64_t)atoi(e):20000;}
    if(++wait_stacks_total%every) return;
    fprintf(stderr,"[Vulkan wait stacks] after %llu waits, top by time:\n",(unsigned long long)wait_stacks_total);
    for(int r=0;r<8;r++) {
        int best=-1;
        for(int s=0;s<512;s++) if(wait_stacks[s].n && wait_stacks[s].count && (best<0 || wait_stacks[s].ns>wait_stacks[best].ns)) best=s;
        if(best<0) break;
        fprintf(stderr,"  %.1fms %llu waits:",wait_stacks[best].ns/1e6,(unsigned long long)wait_stacks[best].count);
        for(int i=1;i<wait_stacks[best].n;i++) {
            Dl_info info;
            if(dladdr(wait_stacks[best].pc[i],&info) && info.dli_fbase)
                fprintf(stderr," %s+%#lx",strrchr(info.dli_fname,'/')?strrchr(info.dli_fname,'/')+1:info.dli_fname,
                    (unsigned long)((uintptr_t)wait_stacks[best].pc[i]-(uintptr_t)info.dli_fbase));
            else fprintf(stderr," %p",wait_stacks[best].pc[i]);
        }
        fputc('\n',stderr);
        wait_stacks[best].count=0;wait_stacks[best].ns=0;  /* printed: start again */
    }
}
#else
static void wait_stack_note(uint64_t ns) {(void)ns;}
#endif
/* Per-draw GPU time by bucket: vertex path (fixed/program/CPU) x fragment
 * shader (plain/textured/combiner/effect) x eye target or guest. */
static struct {uint64_t ns,draws,indices,instances;} draw_stats[3][4][3];
static uint64_t draw_stats_start;
static const char *const draw_vertex_name[3]={"fixed","program","cpu"};
static const char *const draw_fragment_name[4]={"plain","textured","combiner","effect"};
static void draw_timing_collect(ChVkTarget *t,unsigned k) {
    uint32_t n=t->draw_n[k];
    if(!t->draw_pool[k] || !n) return;
    uint64_t stamps[BATCH_DRAWS+2];
    if(vkGetQueryPoolResults(t->device,t->draw_pool[k],0,n+1,sizeof stamps,stamps,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)!=VK_SUCCESS) {t->draw_n[k]=0;return;}
    uint64_t mask=t->timestamp_bits==64?UINT64_MAX:((UINT64_C(1)<<t->timestamp_bits)-1);
    for(uint32_t i=1;i<=n;i++) {
        unsigned b=t->draw_meta[k][i].bucket,v=b>>4,f=(b>>2)&3,e=b&3;
        uint64_t ns=(uint64_t)(((stamps[i]-stamps[i-1])&mask)*t->timestamp_period);
        draw_stats[v][f][e].ns+=ns;draw_stats[v][f][e].draws++;draw_stats[v][f][e].indices+=t->draw_meta[k][i].indices;
    }
    t->draw_n[k]=0;
    uint64_t now=time_ns();
    if(!draw_stats_start) draw_stats_start=now;
    if(now-draw_stats_start<2000000000ull) return;
    fprintf(stderr,"[Vulkan draw GPU] %.1fs:",(now-draw_stats_start)/1e9);
    for(unsigned v=0;v<3;v++) for(unsigned f=0;f<4;f++) for(unsigned e=0;e<3;e++) if(draw_stats[v][f][e].draws)
        fprintf(stderr," %s/%s/%s=%llu draws %.1fms (%.1fus, %llu idx/draw)",draw_vertex_name[v],draw_fragment_name[f],e==1?"eyes3d":e==2?"eyesflat":"guest",
            (unsigned long long)draw_stats[v][f][e].draws,draw_stats[v][f][e].ns/1e6,
            draw_stats[v][f][e].ns/1e3/draw_stats[v][f][e].draws,
            (unsigned long long)(draw_stats[v][f][e].indices/draw_stats[v][f][e].draws));
    fputc('\n',stderr);
    memset(draw_stats,0,sizeof draw_stats);draw_stats_start=now;
}
static VkResult drain_ring(ChVkTarget *);
static VkResult retire_batch(ChVkTarget *t) {
    {VkResult drained=drain_ring(t);if(drained!=VK_SUCCESS) return drained;}
    if(!t->submitted) return VK_SUCCESS;
    VkResult result;uint64_t wait_start=time_ns();
    TRY(vkWaitForFences(t->device,1,&t->fence,VK_TRUE,5000000000ULL));
    wait_stack_note(time_ns()-wait_start);
    /* A destination submission follows its copy on the same queue. Its
     * completed fence proves the source copy is also safe to retire. */
    TRY(wait_copy_source(t));
    /* Detach before retirement callbacks: the copy is complete, and a callback
     * may release resources associated with the dependent target. */
    if(t->copy_destination) {t->copy_destination->copy_source=NULL;t->copy_destination=NULL;}
    draw_timing_collect(t,t->slot);
    if(t->timing) {
        uint64_t waited=time_ns()-wait_start;
        t->timing_stats.batch_wait_ns+=waited;
        uint64_t elapsed;TRY(timestamp_elapsed(t,&elapsed));t->timing_stats.batch_gpu_ns+=elapsed;
        if(t->submitted_reason>=0) {
            t->finish_reasons[t->submitted_reason].wait_ns+=waited;
            t->finish_reasons[t->submitted_reason].gpu_ns+=elapsed;
            if(t->tail_stamped) {
                uint64_t stamps[3];
                if(vkGetQueryPoolResults(t->device,t->timing,0,3,sizeof stamps,stamps,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS) {
                    uint64_t mask=t->timestamp_bits==64?UINT64_MAX:((UINT64_C(1)<<t->timestamp_bits)-1);
                    t->finish_reasons[t->submitted_reason].tail_ns+=(uint64_t)(((stamps[1]-stamps[2])&mask)*t->timestamp_period);
                }
            }
        }
    }
    /* This queue submission follows any depth upload, so its fence also
     * proves that upload command and staging reads have completed. */
    t->upload_pending=0;
    t->pending=t->pending_clears=0;t->geometry_used=0;
    for(unsigned i=0;i<t->pipeline_extent;i++) {t->pipelines[i].set_used=0;t->pipelines[i].pending&=~(1u<<t->slot);}
    uint64_t retire_start=t->timing?time_ns():0;
    retire_samples(t);
    if(t->timing) {
        uint64_t now=time_ns();t->timing_stats.batch_retire_ns+=now-retire_start;
        t->timing_stats.batch_finish_ns+=now-t->submitted_finish_start;report_batch_timing(t,now);
    }
    t->submitted=0;t->deferred_retire=0;return VK_SUCCESS;
fail:
    t->failed=1;vkQueueWaitIdle(t->queue);t->submitted=0;t->deferred_retire=0;t->pending=t->pending_clears=0;
    if(t->copy_destination) {t->copy_destination->failed=1;t->copy_destination->copy_source=NULL;t->copy_destination=NULL;}
    retire_samples(t);return result;
}
static VkResult wait_copy_source(ChVkTarget *t) {
    return t && t->copy_source?retire_batch(t->copy_source):VK_SUCCESS;
}
static VkResult retire_copy_out(ChVkTarget *t) {
    return t && (t->copy_destination || t->deferred_retire)?retire_batch(t):VK_SUCCESS;
}
static void retire_retired(Retired *list,uint32_t *count) {
    for(unsigned i=0;i<*count;i++) {
        if(list[i].owned) ch_vk_texture_destroy(list[i].image);
        else if(list[i].release) list[i].release(list[i].context,list[i].image);
    }
    *count=0;
}
static VkResult retire_slot(ChVkTarget *t,unsigned k) {
    VkResult result;
    if(!t->slots[k].inflight) return VK_SUCCESS;
    uint64_t wait_start=time_ns();
    TRY(vkWaitForFences(t->device,1,&t->slots[k].fence,VK_TRUE,5000000000ULL));
    wait_stack_note(time_ns()-wait_start);
    draw_timing_collect(t,k);
    if(t->slots[k].timing) {
        uint64_t waited=time_ns()-wait_start,stamps[3];
        t->timing_stats.batch_wait_ns+=waited;
        int n=t->slots[k].tail_stamped?3:2;
        if(vkGetQueryPoolResults(t->device,t->slots[k].timing,0,(uint32_t)n,sizeof stamps,stamps,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS) {
            uint64_t mask=t->timestamp_bits==64?UINT64_MAX:((UINT64_C(1)<<t->timestamp_bits)-1);
            uint64_t gpu=(uint64_t)(((stamps[1]-stamps[0])&mask)*t->timestamp_period);
            t->timing_stats.batch_gpu_ns+=gpu;
            if(t->slots[k].reason>=0) {
                t->finish_reasons[t->slots[k].reason].wait_ns+=waited;
                t->finish_reasons[t->slots[k].reason].gpu_ns+=gpu;
                if(n==3) t->finish_reasons[t->slots[k].reason].tail_ns+=(uint64_t)(((stamps[1]-stamps[2])&mask)*t->timestamp_period);
            }
        }
        uint64_t now=time_ns();t->timing_stats.batch_finish_ns+=now-t->slots[k].finish_start;report_batch_timing(t,now);
    }
    for(unsigned i=0;i<t->pipeline_extent;i++) t->pipelines[i].pending&=~(1u<<k);
    retire_retired(t->slots[k].retired,&t->slots[k].retired_count);
    t->slots[k].inflight=0;return VK_SUCCESS;
fail:
    t->failed=1;vkQueueWaitIdle(t->queue);t->slots[k].inflight=0;
    retire_retired(t->slots[k].retired,&t->slots[k].retired_count);return result;
}
static VkResult drain_ring(ChVkTarget *t) {
    /* Oldest first: the slot after the recording one was submitted first. */
    if(t->ring) for(unsigned i=1;i<RING_SLOTS;i++) {
        VkResult r=retire_slot(t,(t->slot+i)%RING_SLOTS);if(r!=VK_SUCCESS) return r;
    }
    return VK_SUCCESS;
}
/* The just-submitted batch moves to its slot; recording continues in the
 * other one once that slot's earlier batch has completed. */
static VkResult rotate(ChVkTarget *t) {
    unsigned k=t->slot,n=(k+1)%RING_SLOTS;
    t->slots[k].command=t->command;t->slots[k].fence=t->fence;t->slots[k].timing=t->timing;
    t->slots[k].geometry=t->geometry;t->slots[k].geometry_memory=t->geometry_memory;t->slots[k].geometry_map=t->geometry_map;
    t->slots[k].geometry_capacity=t->geometry_capacity;t->slots[k].geometry_coherent=t->geometry_coherent;
    t->slots[k].retired=t->retired;t->slots[k].retired_count=t->retired_count;
    t->slots[k].reason=t->submitted_reason;t->slots[k].finish_start=t->submitted_finish_start;
    t->slots[k].tail_stamped=t->tail_stamped;t->slots[k].inflight=1;
    VkResult r=retire_slot(t,n);
    t->slot=n;
    t->command=t->slots[n].command;t->fence=t->slots[n].fence;t->timing=t->slots[n].timing;
    t->geometry=t->slots[n].geometry;t->geometry_memory=t->slots[n].geometry_memory;t->geometry_map=t->slots[n].geometry_map;
    t->geometry_capacity=t->slots[n].geometry_capacity;t->geometry_coherent=t->slots[n].geometry_coherent;
    t->retired=t->slots[n].retired;t->retired_count=t->slots[n].retired_count;
    t->upload_pending=0;t->pending=t->pending_clears=0;t->geometry_used=0;t->tail_stamped=0;
    for(unsigned i=0;i<t->pipeline_extent;i++) t->pipelines[i].set_used=0;
    return r;
}
static void live_barrier(VkCommandBuffer command,const ChVkTarget *source,int acquire) {
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=acquire?VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT:0,
        .dstAccessMask=acquire?VK_ACCESS_SHADER_READ_BIT:VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout=acquire?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout=acquire?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=source->image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(command,
        acquire?VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT:VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        acquire?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,0,NULL,0,NULL,1,&b);
}
static void live_forget(ChVkTarget *reader,const ChVkTarget *source) {
    for(unsigned i=0;i<reader->live_count;i++) if(reader->live[i].source==source) {
        reader->live[i]=reader->live[--reader->live_count];return;
    }
}
/* Anything about to touch a lent image first sends the reader's batch to the
 * queue: its samples precede the touch, and its finish hands the image back. */
static void live_release(ChVkTarget *source) {
    ChVkTarget *reader=source->live_reader;
    if(!reader) return;
    for(unsigned i=0;i<reader->live_count;i++) if(reader->live[i].source==source && !reader->live[i].recorded) {
        reader->live[i]=reader->live[--reader->live_count];source->live_reader=NULL;return;
    }
    (void)ch_vk_target_submit(reader);
    /* No batch was recorded after all: the barrier never reached the queue. */
    if(source->live_reader==reader) {live_forget(reader,source);source->live_reader=NULL;}
}
static VkResult finish_batch_with_copy(ChVkTarget *t,int readback,const char *caller,ChVkTarget *depth_destination,ChVkTexture *texture_destination,int defer_wait) {
    if(!t || t->failed) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result;
    live_release(t);
    if(t->submitted) return retire_batch(t);
    if(!t->pending) {TRY(drain_ring(t));TRY(wait_copy_source(t));return wait_upload(t);}
    uint64_t finish_start=t->timing?time_ns():0;
    vkCmdEndRenderPass(t->command);
    {   /* Hand lent images back once this batch's samples are recorded. */
        unsigned kept=0;
        for(unsigned i=0;i<t->live_count;i++) {
            if(!t->live[i].recorded) {t->live[kept++]=t->live[i];continue;}
            live_barrier(t->command,t->live[i].source,0);
            if(t->live[i].source->live_reader==t) t->live[i].source->live_reader=NULL;
        }
        t->live_count=kept;
    }
    if(t->timing) {vkCmdWriteTimestamp(t->command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,t->timing,2);t->tail_stamped=1;}
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=t->image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    int paired_read=0;
    if(readback==1 || readback==3) paired_read=record_depth_transfer(t,t->command,0,readback==3);
    if(readback==3 && !paired_read) record_paired_color(t);
    if(readback==2) record_color_read(t,0);
    if(depth_destination) record_depth_copy(t->command,t,depth_destination);
    if(texture_destination) TRY(ch_vk_texture_record_image_copy(texture_destination,t->device,t->queue,t->command,t->image,t->width,t->height));
    if(t->timing) vkCmdWriteTimestamp(t->command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,t->timing,1);
    TRY(vkEndCommandBuffer(t->command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&t->command};
    uint64_t submit_start=t->timing?time_ns():0;
    TRY(vkQueueSubmit(t->queue,1,&submit,t->fence));
    uint64_t wait_start=t->timing?time_ns():0;
    if(t->timing) t->timing_stats.batch_submit_ns+=wait_start-submit_start;
    t->submitted=1;t->submitted_finish_start=finish_start;t->submitted_reason=-1;
    if(t->timing) {
        t->timing_stats.batch_draws+=t->pending-t->pending_clears;t->timing_stats.batch_submissions++;
        for(unsigned i=0;i<32;i++) {
            if(!t->finish_reasons[i].caller ||
               (t->finish_reasons[i].readback==readback && !strcmp(t->finish_reasons[i].caller,caller))) {
                t->finish_reasons[i].caller=caller;t->finish_reasons[i].readback=readback;
                t->finish_reasons[i].count++;t->submitted_reason=(int)i;break;
            }
        }
    }
    t->layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;t->resource_stats.batch_submissions++;
    t->resource_stats.combined_depth_copies+=depth_destination!=NULL;
    t->resource_stats.combined_texture_copies+=texture_destination!=NULL;
    if(readback==1 || readback==3) t->resource_stats.combined_depth_readbacks++;
    if(readback==2 || readback==3) t->resource_stats.combined_color_readbacks++;
    /* Nothing for the host to read and no copy edge: hand the batch to the
     * GPU and keep recording into the other slot. */
    if(defer_wait && t->ring && !readback && !depth_destination) {t->submitted=0;t->resource_stats.ring_submissions++;return rotate(t);}
    return defer_wait?VK_SUCCESS:retire_batch(t);
fail:
    t->failed=1;vkQueueWaitIdle(t->queue);t->pending=t->pending_clears=0;
    retire_samples(t);return result;
}
static VkResult finish_batch_named(ChVkTarget *t,int readback,const char *caller) {
    return finish_batch_with_copy(t,readback,caller,NULL,NULL,0);
}
VkResult ch_vk_target_finish(ChVkTarget *t) {return finish_batch_named(t,0,"external_completion");}
VkResult ch_vk_target_submit(ChVkTarget *t) {
    if(!t || t->failed) return VK_ERROR_INITIALIZATION_FAILED;
    if(!t->pending || t->submitted) return VK_SUCCESS;
    VkResult r=finish_batch_with_copy(t,0,"lazy_completion",NULL,NULL,1);
    if(r==VK_SUCCESS) t->deferred_retire=t->submitted;
    return r;
}
static VkResult quantize_depth(ChVkTarget *,int);
VkResult ch_vk_target_depth_quantization(ChVkTarget *t,int mode) {
    if(!t || mode<0 || mode>5)return VK_ERROR_INITIALIZATION_FAILED;
    /* Reasserting disabled quantization is common for non-Z24 surfaces.
     * It must not turn every draw into a submission and CPU fence wait. */
    if(!mode && !t->quantized_depth && !t->failed) return VK_SUCCESS;
    if(!mode) {VkResult r=finish_batch_named(t,0,__func__);if(r==VK_SUCCESS)t->quantized_depth=0;return r;}
    if((mode==3 || mode==4) && !t->input_depth)return VK_ERROR_FEATURE_NOT_PRESENT;
    if(t->quantized_depth==mode)return VK_SUCCESS;
    if(mode==4 && t->pass && !t->input_pass)return VK_ERROR_INITIALIZATION_FAILED;
    if(t->failed || !t->initialized || t->depth_format!=VK_FORMAT_D32_SFLOAT_S8_UINT)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(t->physical,&props);
    if(mode<3 && t->bytes>props.limits.maxStorageBufferRange) return VK_ERROR_FEATURE_NOT_PRESENT;
    if(mode==1) {
        if(t->bytes/4>UINT32_MAX-63 || (t->bytes/4+63)/64>props.limits.maxComputeWorkGroupCount[0] ||
           props.limits.maxComputeWorkGroupInvocations<64 || props.limits.maxComputeWorkGroupSize[0]<64)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(t->physical,&n,NULL);
        VkQueueFamilyProperties *families=calloc(n,sizeof *families);
        if(!families)return VK_ERROR_OUT_OF_HOST_MEMORY;
        vkGetPhysicalDeviceQueueFamilyProperties(t->physical,&n,families);
        int compute=t->family<n && (families[t->family].queueFlags&VK_QUEUE_COMPUTE_BIT);free(families);
        if(!compute)return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    VkResult result;
    TRY(finish_batch_named(t,0,__func__));
    if(mode==5) {
        /* Establish canonical storage once before fragment-side depth tests.
         * Later uploads and clears canonicalize at their existing boundary. */
        TRY(quantize_depth(t,1));t->quantized_depth=5;return VK_SUCCESS;
    }
    if(mode>=3) {
        if(!t->depth_quant_input) TRY(ch_vk_depth_quant_create_input(t->device,t->depth,t->depth_view,t->width,t->height,&t->depth_quant_input));
    } else if(mode==2) {
        if(!t->depth_quant_draw) TRY(ch_vk_depth_quant_create_draw(t->device,t->read,t->bytes,t->depth_view,t->width,t->height,&t->depth_quant_draw));
    } else if(!t->depth_quant) TRY(ch_vk_depth_quant_create(t->device,t->read,t->bytes,&t->depth_quant));
    if(mode==4)t->input_pass=1;
    t->quantized_depth=mode;return VK_SUCCESS;
fail:return result;
}
static VkResult quantize_depth(ChVkTarget *t,int mode) {
    if(!t)return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result;int enabled=t?t->quantized_depth:0;
    TRY(ch_vk_target_depth_quantization(t,mode));
    t->quantized_depth=enabled;
    TRY(finish_batch_named(t,0,__func__));
    TRY(vkResetCommandBuffer(t->command,0));TRY(vkResetFences(t->device,1,&t->fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vkBeginCommandBuffer(t->command,&begin));
    color_staging_reuse(t);
    ch_vk_depth_quant_record(mode==3?t->depth_quant_input:mode==2?t->depth_quant_draw:t->depth_quant,t->command,t->depth,t->width,t->height);
    TRY(vkEndCommandBuffer(t->command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&t->command};
    TRY(vkQueueSubmit(t->queue,1,&submit,t->fence));
    TRY(vkWaitForFences(t->device,1,&t->fence,VK_TRUE,5000000000ULL));
    return VK_SUCCESS;
fail:t->failed=1;return result;
}
VkResult ch_vk_target_quantize_depth(ChVkTarget *t) {return quantize_depth(t,1);}
VkResult ch_vk_target_quantize_depth_draw(ChVkTarget *t) {return quantize_depth(t,2);}
VkResult ch_vk_target_quantize_depth_input(ChVkTarget *t) {return quantize_depth(t,3);}
static VkResult begin_batch(ChVkTarget *t) {
    VkResult result;
    TRY(retire_copy_out(t));
    if(!t->pending) {
        live_release(t);   /* new writes: a reader's samples go first */
        for(unsigned i=0;i<t->live_count;) {   /* recorded into a batch that never reached the queue */
            if(!t->live[i].recorded) {i++;continue;}
            if(t->live[i].source->live_reader==t) t->live[i].source->live_reader=NULL;
            t->live[i]=t->live[--t->live_count];
        }
        t->recorded_pipeline=VK_NULL_HANDLE;t->recorded_push_layout=VK_NULL_HANDLE;t->recorded_blend_valid=0;t->recorded_stencil_valid=0;
        TRY(vkResetCommandBuffer(t->command,0));TRY(vkResetFences(t->device,1,&t->fence));
        VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        TRY(vkBeginCommandBuffer(t->command,&begin));
        color_staging_reuse(t);
        for(unsigned i=0;i<t->live_count;i++) if(!t->live[i].recorded) {
            live_barrier(t->command,t->live[i].source,1);t->live[i].recorded=1;
        }
        if(t->timing) {
            vkCmdResetQueryPool(t->command,t->timing,0,3);t->tail_stamped=0;
            vkCmdWriteTimestamp(t->command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,t->timing,0);
        }
        if(t->draw_pool[t->slot]) {
            vkCmdResetQueryPool(t->command,t->draw_pool[t->slot],0,BATCH_DRAWS+2);t->draw_n[t->slot]=0;
            vkCmdWriteTimestamp(t->command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,t->draw_pool[t->slot],0);
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
            .framebuffer=target_framebuffer(t),.renderArea={{0,0},{t->width,t->height}}};
        vkCmdBeginRenderPass(t->command,&pass,VK_SUBPASS_CONTENTS_INLINE);
    }
    return VK_SUCCESS;
fail:t->failed=1;return result;
}
static VkResult record_batch(ChVkTarget *t,VkBuffer geometry,VkDeviceSize index_offset,uint32_t index_count) {
    VkResult result;
    uint64_t record_start=t->timing?time_ns():0;
    TRY(begin_batch(t));
    if(t->recorded_pipeline!=t->active_pipeline->pipeline) {
        vkCmdBindPipeline(t->command,VK_PIPELINE_BIND_POINT_GRAPHICS,t->active_pipeline->pipeline);
        t->recorded_pipeline=t->active_pipeline->pipeline;t->recorded_stencil_valid=0;t->resource_stats.batch_pipeline_binds++;
    }
    if(t->active_pipeline->key.depth.stencilTestEnable &&
       (!t->recorded_stencil_valid || t->recorded_stencil_reference!=t->stencil_reference)) {
        vkCmdSetStencilReference(t->command,VK_STENCIL_FACE_FRONT_AND_BACK,t->stencil_reference);
        t->recorded_stencil_reference=t->stencil_reference;t->recorded_stencil_valid=1;
    }
    if(!t->recorded_blend_valid || memcmp(t->recorded_blend,t->blend_constants,sizeof t->recorded_blend)) {
        vkCmdSetBlendConstants(t->command,t->blend_constants);
        memcpy(t->recorded_blend,t->blend_constants,sizeof t->recorded_blend);
        t->recorded_blend_valid=1;t->resource_stats.batch_blend_updates++;
    }
    if(t->active_pipeline->has_descriptors) vkCmdBindDescriptorSets(t->command,VK_PIPELINE_BIND_POINT_GRAPHICS,
        t->active_pipeline->pipeline_layout,0,1,&t->active_pipeline->descriptor_set,0,NULL);
    if(t->recorded_push_layout!=t->active_pipeline->pipeline_layout || memcmp(&t->recorded_push,&t->push,sizeof t->push)) {
        vkCmdPushConstants(t->command,t->active_pipeline->pipeline_layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(t->push),t->push.alpha);
        memcpy(&t->recorded_push,&t->push,sizeof t->push);
        t->recorded_push_layout=t->active_pipeline->pipeline_layout;t->resource_stats.batch_push_updates++;
    }
    VkDeviceSize vertex_offset=t->vertex_offset;
    vkCmdBindVertexBuffers(t->command,0,1,&geometry,&vertex_offset);
    vkCmdBindIndexBuffer(t->command,geometry,index_offset,VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(t->command,index_count,t->fixed && t->fixed->imm_mode?2:1,0,0,0);
    if(t->draw_pool[t->slot] && t->draw_n[t->slot]<BATCH_DRAWS) {
        uint32_t i=++t->draw_n[t->slot];
        const PipelineKey *key=&t->active_pipeline->key;
        unsigned v=!key->fixed?2:key->vertex_count?1:0,f=key->shader&3,e=t->fixed?t->fixed->imm_mode&3:0;
        t->draw_meta[t->slot][i].bucket=(uint8_t)(v<<4|f<<2|e);t->draw_meta[t->slot][i].indices=index_count;
        vkCmdWriteTimestamp(t->command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,t->draw_pool[t->slot],i);
    }
    if(t->quantized_depth && t->quantized_depth!=5 && t->active_pipeline->key.depth.depthTestEnable &&
       t->active_pipeline->key.depth.depthWriteEnable) {
        /* Quantization helpers bind their own graphics state/push layout. */
        t->recorded_pipeline=VK_NULL_HANDLE;t->recorded_push_layout=VK_NULL_HANDLE;t->recorded_blend_valid=0;t->recorded_stencil_valid=0;
        t->resource_stats.depth_roundtrips_by_compare[t->active_pipeline->key.depth.depthCompareOp]++;
        if(t->quantized_depth==4) {
            ch_vk_depth_quant_record_inline(t->depth_quant_inline,t->command);
        } else {
            vkCmdEndRenderPass(t->command);
            ch_vk_depth_quant_record(t->quantized_depth>=3?t->depth_quant_input:t->quantized_depth==2?t->depth_quant_draw:t->depth_quant,t->command,t->depth,t->width,t->height);
            VkMemoryBarrier color={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
            vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,1,&color,0,NULL,0,NULL);
            VkRenderPassBeginInfo pass={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=t->pass,
                .framebuffer=target_framebuffer(t),.renderArea={{0,0},{t->width,t->height}}};
            vkCmdBeginRenderPass(t->command,&pass,VK_SUBPASS_CONTENTS_INLINE);
        }
    }
    t->active_pipeline->pending|=1u<<t->slot;
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
       (t->quantized_depth==5 && (clear_flags&2) && (!isfinite(clear_depth) || clear_depth<0 || clear_depth>1)) ||
       (clear_flags & ~7u) || ((clear_flags & 1) && !rgba) ||
       (!t->initialized && clear_flags!=7))
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result;
    TRY(finish_batch_named(t,0,__func__));
    uint64_t cpu_start=t->timing?time_ns():0;
    TRY(vkResetCommandBuffer(t->command,0));
    TRY(vkResetFences(t->device,1,&t->fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vkBeginCommandBuffer(t->command,&begin));
    color_staging_reuse(t);
    if(t->timing) {
        vkCmdResetQueryPool(t->command,t->timing,0,3);t->tail_stamped=0;
        vkCmdWriteTimestamp(t->command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,t->timing,0);
    }
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=t->layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=t->layout,
        .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=t->image,.subresourceRange=range};
    /* A read-only/depth-only operation leaves color in TRANSFER_SRC.
     * Avoid a gratuitous SRC->DST->SRC round trip when color is untouched. */
    int color_write=geometry || (clear_flags&1);
    if(color_write) {
        vkCmdPipelineBarrier(t->command,t->layout==VK_IMAGE_LAYOUT_UNDEFINED?
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    }
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
        if(t->quantized_depth==5 && (clear_flags&2)) value.depth=ch_vk_depth_unpack(ch_vk_depth_pack_attachment(clear_depth));
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
            .renderPass=t->pass,.framebuffer=target_framebuffer(t),
            .renderArea={{0,0},{t->width,t->height}}};
        if(t->count_requested) vkCmdResetQueryPool(t->command,t->queries,0,1);
        vkCmdBeginRenderPass(t->command,&rp,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(t->command,VK_PIPELINE_BIND_POINT_GRAPHICS,t->active_pipeline->pipeline);
        vkCmdSetBlendConstants(t->command,t->blend_constants);
        if(t->active_pipeline->key.depth.stencilTestEnable)
            vkCmdSetStencilReference(t->command,VK_STENCIL_FACE_FRONT_AND_BACK,t->stencil_reference);
        if(t->active_pipeline->has_descriptors) vkCmdBindDescriptorSets(t->command,VK_PIPELINE_BIND_POINT_GRAPHICS,
            t->active_pipeline->pipeline_layout,0,1,&t->active_pipeline->descriptor_set,0,NULL);
        vkCmdPushConstants(t->command,t->active_pipeline->pipeline_layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(t->push),t->push.alpha);
        VkDeviceSize offset=t->vertex_offset;
        vkCmdBindVertexBuffers(t->command,0,1,&geometry,&offset);
        vkCmdBindIndexBuffer(t->command,geometry,index_offset,VK_INDEX_TYPE_UINT16);
        if(t->count_requested) vkCmdBeginQuery(t->command,t->queries,0,VK_QUERY_CONTROL_PRECISE_BIT);
        vkCmdDrawIndexed(t->command,index_count,t->fixed && t->fixed->imm_mode?2:1,0,0,0);
        if(t->count_requested) vkCmdEndQuery(t->command,t->queries,0);
        if(t->quantized_depth==4 && ((clear_flags&2) ||
           (t->active_pipeline->key.depth.depthTestEnable && t->active_pipeline->key.depth.depthWriteEnable)))
            ch_vk_depth_quant_record_inline(t->depth_quant_inline,t->command);
        vkCmdEndRenderPass(t->command);
    }
    if(t->quantized_depth && t->quantized_depth!=5 && !(t->quantized_depth==4 && geometry) && ((clear_flags&2) || (geometry &&
       t->active_pipeline->key.depth.depthTestEnable && t->active_pipeline->key.depth.depthWriteEnable)))
        ch_vk_depth_quant_record(t->quantized_depth>=3?t->depth_quant_input:t->quantized_depth==2?t->depth_quant_draw:t->depth_quant,t->command,t->depth,t->width,t->height);
    barrier.srcAccessMask=geometry?VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT:VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout=geometry?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if(color_write) {
        vkCmdPipelineBarrier(t->command,geometry?VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    } else t->resource_stats.read_only_color_barriers_avoided+=2;
    if(!t->defer_color_readback) record_color_read(t,0);
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
    for(unsigned i=0;i<t->pipeline_extent;i++) t->pipelines[i].set_used=0;
    if(t->defer_color_readback) return VK_SUCCESS;
    void *mapped;
    TRY(map_readback(t,0,&mapped));
    result=maintain_staging(t,0,0,t->color_bgra==2?t->bytes:0,t->bytes);
    if(result==VK_SUCCESS) memcpy(pixels,(char *)mapped+(t->color_bgra==2?t->bytes:0),(size_t)t->bytes);
    if(result!=VK_SUCCESS) goto fail;
    return VK_SUCCESS;
fail:
    t->failed=1; return result;
}

void ch_vk_target_defer_color_readback(ChVkTarget *t,int enabled) {
    if(t) {if(!enabled && t->pending) (void)finish_batch_named(t,0,__func__);t->defer_color_readback=!!enabled;}
}
/* The caller keeps the output alive and leaves this target untouched until
 * end. Independent targets can be submitted before the first CPU wait. */
VkResult ch_vk_target_read_color_begin(ChVkTarget *t,void *pixels,VkDeviceSize bytes) {
    if(!t || t->failed || !pixels || bytes<t->bytes || t->color_readback_active || t->readback_active)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult copied=retire_copy_out(t);if(copied!=VK_SUCCESS) return copied;
    int batched=!!t->pending;
    VkResult r=batched?finish_batch_with_copy(t,2,__func__,NULL,NULL,1):ch_vk_target_read_color(t,pixels,bytes);
    if(r!=VK_SUCCESS) return r;
    t->color_readback_active=1;t->color_readback_batched=batched;t->color_readback_pixels=pixels;
    if(batched) t->resource_stats.queued_color_readbacks++;
    return VK_SUCCESS;
}
VkResult ch_vk_target_read_color_end(ChVkTarget *t) {
    if(!t || !t->color_readback_active) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=VK_SUCCESS;
    if(t->color_readback_batched) {
        r=retire_batch(t);
        void *mapped=NULL;
        if(r==VK_SUCCESS) r=map_readback(t,0,&mapped);
        if(r==VK_SUCCESS) {
            r=maintain_staging(t,0,0,t->color_bgra==2?t->bytes:0,t->bytes);
            if(r==VK_SUCCESS) memcpy(t->color_readback_pixels,
                (char *)mapped+(t->color_bgra==2?t->bytes:0),(size_t)t->bytes);
        }
    }
    t->color_readback_active=0;t->color_readback_pixels=NULL;
    if(r!=VK_SUCCESS) t->failed=1;
    return r;
}
VkResult ch_vk_target_read_color(ChVkTarget *t,void *pixels,VkDeviceSize bytes) {
    if(!t || t->color_readback_active || t->readback_active) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult copied=retire_copy_out(t);if(copied!=VK_SUCCESS) return copied;
    if(t->pending) {
        if(!pixels || bytes<t->bytes) return VK_ERROR_INITIALIZATION_FAILED;
        VkResult r=finish_batch_named(t,2,__func__);
        if(r!=VK_SUCCESS) return r;
        void *mapped=NULL;
        r=map_readback(t,0,&mapped);
        if(r==VK_SUCCESS) {
            r=maintain_staging(t,0,0,t->color_bgra==2?t->bytes:0,t->bytes);
            if(r==VK_SUCCESS) memcpy(pixels,(char *)mapped+(t->color_bgra==2?t->bytes:0),(size_t)t->bytes);
        }
        if(r!=VK_SUCCESS) t->failed=1;
        return r;
    }
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
    VkResult copied=retire_copy_out(t);if(copied!=VK_SUCCESS) return copied;
    /* Allocate before retiring an open batch so the first snapshot, like a
     * refresh, can copy and generate mips in the source submission. The copy
     * remains private until the source fence proves both operations complete. */
    if(t->pending && !t->submitted) {
        ChVkTexture *copy=NULL;
        VkResult result=ch_vk_texture_create_copy_target(t->physical,t->device,t->queue,t->family,
            t->width,t->height,levels,&copy);
        if(result==VK_SUCCESS) result=finish_batch_with_copy(t,0,__func__,NULL,copy,1);
        if(result!=VK_SUCCESS) {ch_vk_texture_destroy(copy);return result;}
        t->deferred_retire=t->submitted;
        t->resource_stats.combined_new_texture_copies++;
        *out=copy;return VK_SUCCESS;
    }
    VkResult result=finish_batch_named(t,0,__func__);
    if(result!=VK_SUCCESS) return result;
    if(t->layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) return VK_ERROR_INITIALIZATION_FAILED;
    return ch_vk_texture_copy_image(t->physical,t->device,t->queue,t->family,t->width,t->height,t->image,levels,out);
}
VkResult ch_vk_target_lend_texture(ChVkTarget *t,ChVkTarget *reader,int alpha_one,
    const VkSamplerCreateInfo *s,ChVkTexture **out) {
    if(!t || !reader || !s || !out || t==reader || t->failed || reader->failed || !t->initialized ||
       !t->sampleable || t->device!=reader->device || t->queue!=reader->queue) return VK_NOT_READY;
    uint32_t key[12]={(uint32_t)alpha_one,s->magFilter,s->minFilter,s->mipmapMode,s->addressModeU,s->addressModeV,
        s->addressModeW,s->borderColor,s->unnormalizedCoordinates,s->compareEnable,s->anisotropyEnable,0};
    memcpy(&key[11],&s->maxLod,sizeof key[11]);
    ChVkTexture *texture=NULL;
    for(unsigned i=0;i<t->lent_count;i++) if(!memcmp(t->lent[i].key,key,sizeof key)) {texture=t->lent[i].texture;break;}
    if(!texture) {
        if(t->lent_count==sizeof t->lent/sizeof t->lent[0]) return VK_NOT_READY;
        VkResult r=ch_vk_texture_borrow(t->device,t->queue,t->image,t->width,t->height,alpha_one,s,&texture);
        if(r!=VK_SUCCESS) return r;
        memcpy(t->lent[t->lent_count].key,key,sizeof key);t->lent[t->lent_count++].texture=texture;
    }
    if(t->live_reader!=reader) {
        if(reader->live_count==sizeof reader->live/sizeof reader->live[0]) return VK_NOT_READY;
        live_release(t);
        VkResult r=retire_copy_out(t);if(r!=VK_SUCCESS) return r;
        r=ch_vk_target_submit(t);if(r!=VK_SUCCESS) return r;   /* the image's writes reach the queue first */
        if(t->layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) return VK_NOT_READY;
        /* The acquire barrier goes before the reader's next render pass. */
        if(reader->pending) {r=ch_vk_target_submit(reader);if(r!=VK_SUCCESS) return r;}
        reader->live[reader->live_count].source=t;reader->live[reader->live_count++].recorded=0;
        t->live_reader=reader;
    }
    *out=texture;return VK_SUCCESS;
}
VkResult ch_vk_target_refresh_texture(ChVkTarget *t,ChVkTexture *texture) {
    if(!t || t->failed || !t->initialized) return VK_ERROR_INITIALIZATION_FAILED;
    if(!ch_vk_texture_image_copy_valid(texture,t->device,t->queue,t->image,t->width,t->height)) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult copied=retire_copy_out(t);if(copied!=VK_SUCCESS) return copied;
    if(t->pending && !t->submitted) {
        VkResult r=finish_batch_with_copy(t,0,__func__,NULL,texture,1);
        if(r==VK_SUCCESS) t->deferred_retire=t->submitted;
        return r;
    }
    VkResult r=finish_batch_named(t,0,__func__);
    if(r!=VK_SUCCESS) return r;
    if(t->layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) return VK_ERROR_INITIALIZATION_FAILED;
    return ch_vk_texture_copy_from_image(texture,t->device,t->image,t->width,t->height);
}
VkResult ch_vk_target_clear_read(ChVkTarget *t, const float rgba[4],
    void *pixels, VkDeviceSize bytes) {
    return render_read(t,rgba,7,1.0f,0,VK_NULL_HANDLE,0,0,pixels,bytes);
}
/* Diagnostic identity excludes Vulkan structure headers/padding and pointers.
 * It lets cache training compare semantic pipeline variants across processes. */
static uint64_t diagnostic_hash(uint64_t hash,const void *data,size_t size) {
    const unsigned char *p=data;
    for(size_t i=0;i<size;i++) hash=(hash^p[i])*UINT64_C(1099511628211);
    return hash;
}
static uint64_t key_identity(uint32_t width,uint32_t height,uint32_t depth_format,uint32_t input_pass,const PipelineKey *key) {
    uint64_t h=UINT64_C(14695981039346656037);
    const uint32_t target[]={width,height,depth_format,input_pass};
    h=diagnostic_hash(h,target,sizeof target);
    h=diagnostic_hash(h,&key->blend,sizeof key->blend);
    const size_t depth_start=offsetof(VkPipelineDepthStencilStateCreateInfo,depthTestEnable);
    h=diagnostic_hash(h,(const unsigned char *)&key->depth+depth_start,sizeof key->depth-depth_start);
    h=diagnostic_hash(h,&key->cull,sizeof key->cull);
    h=diagnostic_hash(h,&key->front,sizeof key->front);
    const uint32_t shaders[]={key->shader,key->fixed,key->alpha_shader,key->specialized,key->fragment_depth};
    h=diagnostic_hash(h,shaders,sizeof shaders);
    h=diagnostic_hash(h,&key->program,sizeof key->program);
    const uint32_t samplers[]={key->sample_static,key->sample_types};
    h=diagnostic_hash(h,samplers,sizeof samplers);
    h=diagnostic_hash(h,&key->vertex_count,sizeof key->vertex_count);
    h=diagnostic_hash(h,key->vertex_program,sizeof key->vertex_program);
    h=diagnostic_hash(h,&key->vertex_fetch_specialized,sizeof key->vertex_fetch_specialized);
    h=diagnostic_hash(h,key->vertex_formats,sizeof key->vertex_formats);
    return diagnostic_hash(h,key->fixed_fetch,sizeof key->fixed_fetch);
}
static uint64_t pipeline_identity(const ChVkTarget *t,const PipelineKey *key) {
    return key_identity(t->width,t->height,(uint32_t)t->depth_format,(uint32_t)t->input_pass,key);
}
/* The render pass a target's pipelines are built against: R8G8B8A8 color
 * and depth; with input_pass the depth an input attachment too. */
/* Foveated shading on the eye images (VK_KHR_fragment_shading_rate). Every
 * target of the registered device at least 1024 wide -- the immersive eye
 * images, side by side, and their half-size copies; game surfaces are at
 * most 640 -- renders with a shading-rate attachment: full rate within
 * `inner` degrees of each eye's view axis, 2x1 out to `outer`, 2x2 out to
 * `far`, 4x4 beyond (mostly the corners, at the lens's edge).
 * Render pass, pipelines and boot-time recipes all ask target_foveated. */
static struct {
    VkDevice device; PFN_vkCreateRenderPass2KHR create_pass2;
    uint32_t texel_w,texel_h; float inner,outer,far;
    int have_view; float tan[2][4];
    int level;   /* dynamic resolution (vk_immersive.c): eyes drawn at scales[level] */
} s_fov;
static const float s_fov_scales[4]={1.f,.9f,.8f,.7f};
void ch_vk_target_set_foveation_level(int level) {
    if(level>=0 && level<4) __atomic_store_n(&s_fov.level,level,__ATOMIC_RELEASE);
}
static VkFramebuffer target_framebuffer(const ChVkTarget *t) {
    return t->fov_fb[0]?t->fov_fb[__atomic_load_n(&s_fov.level,__ATOMIC_ACQUIRE)]:t->framebuffer;
}
void ch_vk_target_set_foveation(VkDevice device,PFN_vkCreateRenderPass2KHR create_pass2,
    uint32_t texel_w,uint32_t texel_h,float inner_deg,float outer_deg,float far_deg) {
    s_fov.create_pass2=create_pass2;s_fov.texel_w=texel_w;s_fov.texel_h=texel_h;
    s_fov.inner=inner_deg;s_fov.outer=outer_deg;s_fov.far=far_deg;s_fov.device=device;
}
void ch_vk_target_set_foveation_view(const float tan_fov[2][4]) {
    if(__atomic_load_n(&s_fov.have_view,__ATOMIC_ACQUIRE)) return;
    memcpy(s_fov.tan,tan_fov,sizeof s_fov.tan);
    __atomic_store_n(&s_fov.have_view,1,__ATOMIC_RELEASE);
}
static int target_foveated(VkDevice device,uint32_t width) {
    return s_fov.device && device==s_fov.device && width>=1024;
}
/* The rate for a point of eye e at (u,v) in [0,1]: the angle from the view
 * axis, from the eye's field of view (a symmetric ~100 degrees until the
 * headset's is known). Encoding: log2(width)<<2 | log2(height). */
static uint8_t fov_rate(int e,float u,float v) {
    float l=-1.2f,r=1.2f,up=1.2f,dn=-1.2f;
    if(__atomic_load_n(&s_fov.have_view,__ATOMIC_ACQUIRE)) {l=s_fov.tan[e][0];r=s_fov.tan[e][1];up=s_fov.tan[e][2];dn=s_fov.tan[e][3];}
    float tx=l+u*(r-l),ty=up+v*(dn-up);
    float a=atanf(sqrtf(tx*tx+ty*ty))*57.29578f;
    return a<s_fov.inner?0:a<s_fov.outer?4:a<s_fov.far?5:10;
}
/* The target's shading-rate image: filled once, then kept in the
 * attachment layout. */
static VkResult rate_image_create(ChVkTarget *t,int lv) {
    VkResult result=VK_SUCCESS;
    uint32_t w=(t->width+s_fov.texel_w-1)/s_fov.texel_w,h=(t->height+s_fov.texel_h-1)/s_fov.texel_h;
    VkBuffer staging=VK_NULL_HANDLE;VkDeviceMemory staging_memory=VK_NULL_HANDLE;
    VkCommandPool pool=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;
    VkImageCreateInfo ic={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8_UINT,
        .extent={w,h,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR|VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    TRY(vkCreateImage(t->device,&ic,NULL,&t->rate_image[lv]));
    VkMemoryRequirements req;vkGetImageMemoryRequirements(t->device,t->rate_image[lv],&req);
    VkMemoryAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size};
    if(!memory_type(t->physical,req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,&alloc.memoryTypeIndex)) {result=VK_ERROR_OUT_OF_DEVICE_MEMORY;goto fail;}
    TRY(vkAllocateMemory(t->device,&alloc,NULL,&t->rate_memory[lv]));
    TRY(vkBindImageMemory(t->device,t->rate_image[lv],t->rate_memory[lv],0));
    VkImageViewCreateInfo vc={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=t->rate_image[lv],
        .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R8_UINT,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    TRY(vkCreateImageView(t->device,&vc,NULL,&t->rate_view[lv]));
    VkBufferCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=(VkDeviceSize)w*h,.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    TRY(vkCreateBuffer(t->device,&bc,NULL,&staging));
    vkGetBufferMemoryRequirements(t->device,staging,&req);
    alloc.allocationSize=req.size;
    if(!memory_type(t->physical,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,&alloc.memoryTypeIndex)) {result=VK_ERROR_OUT_OF_DEVICE_MEMORY;goto fail;}
    TRY(vkAllocateMemory(t->device,&alloc,NULL,&staging_memory));
    TRY(vkBindBufferMemory(t->device,staging,staging_memory,0));
    uint8_t *rates;TRY(vkMapMemory(t->device,staging_memory,0,VK_WHOLE_SIZE,0,(void **)&rates));
    unsigned counts[11]={0};
    /* each eye's picture fills k of its half: the left eye's from the
     * top-left corner, the right eye's from the top-right (immersive.glsl) */
    float k=s_fov_scales[lv];
    for(uint32_t y=0;y<h;y++) for(uint32_t x=0;x<w;x++) {
        float px=(x+.5f)*s_fov.texel_w/(float)t->width*2,py=(y+.5f)*s_fov.texel_h/(float)t->height/k;
        int e=px>=1;float u=e?(px-(2-k))/k:px/k;
        uint8_t r=u<0 || u>1 || py>1?10:fov_rate(e,u,py);rates[y*w+x]=r;counts[r]++;
    }
    vkUnmapMemory(t->device,staging_memory);
    if(!lv) fprintf(stderr,"[Vulkan] foveated %ux%u: %u%% full rate, %u%% 2x1, %u%% 2x2, %u%% 4x4 (%ux%u texels of %ux%u)\n",t->width,t->height,
        counts[0]*100/(w*h),counts[4]*100/(w*h),counts[5]*100/(w*h),counts[10]*100/(w*h),w,h,s_fov.texel_w,s_fov.texel_h);
    VkCommandPoolCreateInfo pc={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=t->family};
    TRY(vkCreateCommandPool(t->device,&pc,NULL,&pool));
    VkCommandBuffer cmd;VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,
        .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    TRY(vkAllocateCommandBuffers(t->device,&ca,&cmd));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vkBeginCommandBuffer(cmd,&begin));
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=t->rate_image[lv],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&b);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={w,h,1}};
    vkCmdCopyBufferToImage(cmd,staging,t->rate_image[lv],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
    b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,0,0,NULL,0,NULL,1,&b);
    TRY(vkEndCommandBuffer(cmd));
    VkFenceCreateInfo fc={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    TRY(vkCreateFence(t->device,&fc,NULL,&fence));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
    TRY(vkQueueSubmit(t->queue,1,&submit,fence));
    TRY(vkWaitForFences(t->device,1,&fence,VK_TRUE,UINT64_MAX));
fail:
    if(fence) vkDestroyFence(t->device,fence,NULL);
    if(pool) vkDestroyCommandPool(t->device,pool,NULL);
    if(staging) vkDestroyBuffer(t->device,staging,NULL);
    if(staging_memory) vkFreeMemory(t->device,staging_memory,NULL);
    return result;
}
/* target_pass with a shading-rate attachment (2): render pass 2. */
static VkResult target_pass_foveated(VkDevice device,VkFormat depth_format,int input_pass,VkRenderPass *out) {
    VkAttachmentDescription2 a[3]={
        {.sType=VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,.format=VK_FORMAT_R8G8B8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
         .loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {.sType=VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,.format=depth_format,.samples=VK_SAMPLE_COUNT_1_BIT,
         .loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE,
         .initialLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,.finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL},
        {.sType=VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,.format=VK_FORMAT_R8_UINT,.samples=VK_SAMPLE_COUNT_1_BIT,
         .loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_NONE,
         .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout=VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR,
         .finalLayout=VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR}};
    a[2].storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkAttachmentReference2 color={.sType=VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,.attachment=0,
        .layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT};
    VkAttachmentReference2 depth={.sType=VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,.attachment=1,
        .layout=input_pass?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT};
    VkAttachmentReference2 depth_input=depth;depth_input.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
    VkAttachmentReference2 rate={.sType=VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,.attachment=2,
        .layout=VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR};
    VkFragmentShadingRateAttachmentInfoKHR fsr={.sType=VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR,
        .pFragmentShadingRateAttachment=&rate,.shadingRateAttachmentTexelSize={s_fov.texel_w,s_fov.texel_h}};
    VkSubpassDescription2 sub={.sType=VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,.pNext=&fsr,
        .pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&color,
        .pDepthStencilAttachment=&depth,.inputAttachmentCount=input_pass?1:0,.pInputAttachments=input_pass?&depth_input:NULL};
    const VkPipelineStageFlags stages=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags access=VK_ACCESS_INPUT_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkSubpassDependency2 deps[3]={
        {.sType=VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2,.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,.srcStageMask=stages,.dstStageMask=stages,
         .srcAccessMask=access,.dstAccessMask=access,.dependencyFlags=VK_DEPENDENCY_BY_REGION_BIT},
        {.sType=VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2,.srcSubpass=0,.dstSubpass=0,.srcStageMask=stages,.dstStageMask=stages,
         .srcAccessMask=access,.dstAccessMask=access,.dependencyFlags=VK_DEPENDENCY_BY_REGION_BIT},
        {.sType=VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2,.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,.srcStageMask=stages,.dstStageMask=stages,
         .srcAccessMask=access,.dstAccessMask=access,.dependencyFlags=VK_DEPENDENCY_BY_REGION_BIT}};
    VkRenderPassCreateInfo2 rp={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,.attachmentCount=3,.pAttachments=a,
        .subpassCount=1,.pSubpasses=&sub,.dependencyCount=input_pass?3:0,.pDependencies=input_pass?deps:NULL};
    return s_fov.create_pass2(device,&rp,NULL,out);
}
static VkResult target_pass(VkDevice device,VkFormat depth_format,int input_pass,uint32_t width,VkRenderPass *out) {
    VkResult result=VK_SUCCESS;
    if(target_foveated(device,width)) return target_pass_foveated(device,depth_format,input_pass,out);
    VkAttachmentDescription attachment={.format=VK_FORMAT_R8G8B8A8_UNORM,
        .samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentDescription attachments[2]={attachment,{
        .format=depth_format,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,.finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference depth_ref={1,input_pass?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ref={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=1,.pColorAttachments=&ref,.pDepthStencilAttachment=&depth_ref,
        .inputAttachmentCount=input_pass?1:0,.pInputAttachments=input_pass?&depth_ref:NULL};
    const VkPipelineStageFlags stages=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags access=VK_ACCESS_INPUT_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkSubpassDependency deps[]={
        {VK_SUBPASS_EXTERNAL,0,stages,stages,access,access,VK_DEPENDENCY_BY_REGION_BIT},
        {0,0,stages,stages,access,access,VK_DEPENDENCY_BY_REGION_BIT},
        {0,VK_SUBPASS_EXTERNAL,stages,stages,access,access,VK_DEPENDENCY_BY_REGION_BIT}};
    VkRenderPassCreateInfo rp={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=2,.pAttachments=attachments,.subpassCount=1,.pSubpasses=&sub,
        .dependencyCount=input_pass?3:0,.pDependencies=input_pass?deps:NULL};
    TRY(vkCreateRenderPass(device,&rp,NULL,out));
fail:
    return result;
}
/* A pipeline from its key alone (and the target's size: the viewport is
 * static) -- what a target builds, and what the recipes precompile. */
static VkResult pipeline_build(VkDevice device,VkPipelineCache cache,VkRenderPass pass,VkPipelineLayout layout,
    const PipelineKey *key,uint32_t width,uint32_t height,VkPipeline *out) {
    VkResult result=VK_SUCCESS;
    VkShaderModule vert=VK_NULL_HANDLE, frag=VK_NULL_HANDLE;
    VkShaderModuleCreateInfo shader={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=key->fixed?sizeof(color_fixed_spv):sizeof(color_vert_spv),.pCode=key->fixed?color_fixed_spv:color_vert_spv};
    if(key->vertex_count) {
        shader.codeSize=sizeof(color_program_spv);shader.pCode=color_program_spv;
        if(ch_vk_vertex_program_loop) {
#define VP_MODULE(name) do {shader.codeSize=sizeof(color_##name##_spv);shader.pCode=color_##name##_spv;} while(0)
            if(key->vertex_count<=16) VP_MODULE(program_s16);
            else if(key->vertex_count<=32) VP_MODULE(program_s32);
            else if(key->vertex_count<=64) VP_MODULE(program_s64);
            else VP_MODULE(program_loop);
#undef VP_MODULE
        }
    }
    TRY(vkCreateShaderModule(device,&shader,NULL,&vert));
#define FRAGMENT(name) do { \
    shader.codeSize=key->fragment_depth?sizeof(color_##name##_depth_spv):sizeof(color_##name##_spv); \
    shader.pCode=key->fragment_depth?color_##name##_depth_spv:color_##name##_spv; \
} while(0)
    if(key->specialized) {
        if(key->alpha_shader) FRAGMENT(combiner_specialized);else FRAGMENT(combiner_specialized_noat);
    } else if(key->shader==3) {
        if(key->alpha_shader) FRAGMENT(effect);else FRAGMENT(effect_noat);
    } else if(key->shader==2) {
        if(key->alpha_shader) FRAGMENT(combiner);else FRAGMENT(combiner_noat);
    } else if(key->shader==1) {
        if(key->alpha_shader) FRAGMENT(textured);else FRAGMENT(textured_noat);
    } else {
        if(key->alpha_shader) FRAGMENT(frag);else FRAGMENT(frag_noat);
    }
#undef FRAGMENT
    TRY(vkCreateShaderModule(device,&shader,NULL,&frag));
    VkPipelineShaderStageCreateInfo stages[]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vert,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=frag,.pName="main"}};
    VkSpecializationMapEntry entries[sizeof(key->program)/sizeof(uint32_t)+2];
    for(unsigned i=0;i<sizeof entries/sizeof entries[0];i++)
        entries[i]=(VkSpecializationMapEntry){.constantID=i,.offset=i*sizeof(uint32_t),.size=sizeof(uint32_t)};
    entries[sizeof entries/sizeof entries[0]-2].constantID=100;
    entries[sizeof entries/sizeof entries[0]-1].constantID=101;
    VkSpecializationInfo specialization={.mapEntryCount=sizeof entries/sizeof entries[0],.pMapEntries=entries,
        .dataSize=sizeof key->program+8,.pData=&key->program};
    if(key->specialized) stages[1].pSpecializationInfo=&specialization;
    VkSpecializationMapEntry vp_entries[563];
    for(unsigned i=0;i<563;i++) vp_entries[i]=(VkSpecializationMapEntry){i,i*4,4};
    uint32_t vp_data[563];memcpy(vp_data,&key->vertex_count,562*4);vp_data[562]=0;
    /* Instruction input reads are immutable program metadata. Scan only when
     * creating a pipeline, conservatively including all encoded source muxes. */
    for(unsigned i=0;i<key->vertex_count;i++) {
        uint32_t y=key->vertex_program[i][1],z=key->vertex_program[i][2],w=key->vertex_program[i][3];
        if(((z>>26)&3)==2 || ((z>>11)&3)==2 || ((w>>28)&3)==2) vp_data[562]|=1u<<((y>>9)&15);
    }
    VkSpecializationInfo vp_specialization={563,vp_entries,sizeof vp_data,vp_data};
    if(key->vertex_count) stages[0].pSpecializationInfo=&vp_specialization;
    VkSpecializationMapEntry fixed_entries[25];
    for(unsigned i=0;i<25;i++) fixed_entries[i]=(VkSpecializationMapEntry){i,i*4,4};
    VkSpecializationInfo fixed_specialization={25,fixed_entries,sizeof key->fixed_fetch,key->fixed_fetch};
    if(key->fixed && !key->vertex_count) stages[0].pSpecializationInfo=&fixed_specialization;
    VkVertexInputBindingDescription binding={0,sizeof(ChVkColorVertex),VK_VERTEX_INPUT_RATE_VERTEX};   /* fixed: no vertex input */

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
        .vertexBindingDescriptionCount=key->fixed?0:1,.pVertexBindingDescriptions=key->fixed?NULL:&binding,
        .vertexAttributeDescriptionCount=key->fixed?0:9,.pVertexAttributeDescriptions=key->fixed?NULL:attrs};
    VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport={0,0,(float)width,(float)height,0,1};
    VkRect2D scissor={{0,0},{width,height}};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=key->cull,.frontFace=key->front,.lineWidth=1,.depthClampEnable=key->fixed!=0};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&key->blend};   /* blend constants: dynamic */
    VkDynamicState dynamic_states[]={VK_DYNAMIC_STATE_BLEND_CONSTANTS,VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dynamic={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount=key->depth.stencilTestEnable?2:1,.pDynamicStates=dynamic_states};
    VkGraphicsPipelineCreateInfo pipeline={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount=2,.pStages=stages,.pVertexInputState=&input,.pInputAssemblyState=&assembly,
        .pViewportState=&vp,.pRasterizationState=&raster,.pMultisampleState=&ms,
        .pDynamicState=&dynamic,.pDepthStencilState=&key->depth,.pColorBlendState=&cb,.layout=layout,.renderPass=pass};
    /* foveated targets: the rate from the attachment */
    VkPipelineFragmentShadingRateStateCreateInfoKHR fsr={.sType=VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR,
        .fragmentSize={1,1},.combinerOps={VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR}};
    if(target_foveated(device,width)) pipeline.pNext=&fsr;
    TRY(vkCreateGraphicsPipelines(device,cache,1,&pipeline,NULL,out));
fail:
    if(vert) vkDestroyShaderModule(device,vert,NULL);
    if(frag) vkDestroyShaderModule(device,frag,NULL);
    return result;
}
/* A pipeline's descriptor set and pipeline layouts, from its key: images
 * (the combiner's 8, or one texture), the combiner's and the fixed/program
 * constants (uniform buffers), the vertex streams (storage buffer). */
typedef struct {uint32_t images,uniforms,buffers;} PipelineDescriptors;
static PipelineDescriptors pipeline_descriptors(const PipelineKey *key) {
    int rc=key->shader==2,texture=key->shader==1,fixed=key->fixed!=0;
    return (PipelineDescriptors){rc?8u:texture?1u:0u,(uint32_t)rc+(uint32_t)fixed,(uint32_t)fixed};
}
static VkResult pipeline_layout_create(VkDevice device,const PipelineKey *key,VkDescriptorSetLayout *set_layout,VkPipelineLayout *out) {
    VkResult result=VK_SUCCESS;
    int rc=key->shader==2,fixed=key->fixed!=0;
    PipelineDescriptors d=pipeline_descriptors(key);
    VkPushConstantRange alpha_range={VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(((ChVkTarget *)0)->push)};
    VkPipelineLayoutCreateInfo layout={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount=1,.pPushConstantRanges=&alpha_range};
    *set_layout=VK_NULL_HANDLE;
    if(d.images || d.uniforms || d.buffers) {
        VkDescriptorSetLayoutBinding bindings[5];unsigned n=0;
        if(d.images) bindings[n++]=(VkDescriptorSetLayoutBinding){.binding=0,
            .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=rc?4:1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
        if(rc) {
            bindings[n++]=(VkDescriptorSetLayoutBinding){.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
            bindings[n++]=(VkDescriptorSetLayoutBinding){.binding=2,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=4,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
        }
        if(fixed) bindings[n++]=(VkDescriptorSetLayoutBinding){.binding=3,
            .descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_VERTEX_BIT};
        if(fixed) bindings[n++]=(VkDescriptorSetLayoutBinding){.binding=4,
            .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_VERTEX_BIT};
        VkDescriptorSetLayoutCreateInfo dsl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=n,.pBindings=bindings};
        TRY(vkCreateDescriptorSetLayout(device,&dsl,NULL,set_layout));
        layout.setLayoutCount=1;layout.pSetLayouts=set_layout;
    }
    TRY(vkCreatePipelineLayout(device,&layout,NULL,out));
fail:
    return result;
}
static VkResult create_pipeline(ChVkTarget *t,const PipelineKey *key) {
    VkResult result;
    if(!t->pass) {
        int foveated=target_foveated(t->device,t->width);
        if(foveated) for(int lv=0;lv<4;lv++) TRY(rate_image_create(t,lv));
        TRY(target_pass(t->device,t->depth_format,t->input_pass,t->width,&t->pass));
        VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image=t->image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        TRY(vkCreateImageView(t->device,&view,NULL,&t->view));
        VkImageView views[]={t->view,t->depth_view,t->rate_view[0]};
        VkFramebufferCreateInfo fb={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass=t->pass,.attachmentCount=foveated?3:2,.pAttachments=views,
            .width=t->width,.height=t->height,.layers=1};
        TRY(vkCreateFramebuffer(t->device,&fb,NULL,&t->framebuffer));
        if(foveated) {
            t->fov_fb[0]=t->framebuffer;
            for(int lv=1;lv<4;lv++) {views[2]=t->rate_view[lv];TRY(vkCreateFramebuffer(t->device,&fb,NULL,&t->fov_fb[lv]));}
        }
    }
    if(t->input_pass && !t->depth_quant_inline)
        TRY(ch_vk_depth_quant_create_inline(t->device,t->depth,t->depth_view,t->width,t->height,t->pass,&t->depth_quant_inline));
    PipelineDescriptors d=pipeline_descriptors(key);
    t->active_pipeline->has_descriptors=d.images || d.uniforms || d.buffers;
    TRY(pipeline_layout_create(t->device,key,&t->active_pipeline->descriptor_layout,&t->active_pipeline->pipeline_layout));
    if(t->active_pipeline->has_descriptors) {
        VkDescriptorPoolSize sizes[3];unsigned n=0;
        if(d.images) sizes[n++]=(VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,d.images};
        if(d.uniforms) sizes[n++]=(VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,d.uniforms};
        if(d.buffers) sizes[n++]=(VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,d.buffers};
        uint32_t banks=t->ring?RING_SLOTS:1;
        for(unsigned i=0;i<n;i++) sizes[i].descriptorCount*=(t->batch_enabled?t->batch_limit:1)*banks;
        VkDescriptorPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=(t->batch_enabled?t->batch_limit:1)*banks,.poolSizeCount=n,.pPoolSizes=sizes};
        TRY(vkCreateDescriptorPool(t->device,&pool,NULL,&t->active_pipeline->descriptor_pool));
        VkDescriptorSetAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool=t->active_pipeline->descriptor_pool,.descriptorSetCount=1,.pSetLayouts=&t->active_pipeline->descriptor_layout};
        TRY(vkAllocateDescriptorSets(t->device,&alloc,&t->active_pipeline->descriptor_set));
        t->active_pipeline->sets[t->slot][0]=t->active_pipeline->descriptor_set;
        for(unsigned k=0;k<RING_SLOTS;k++) t->active_pipeline->set_count[k]=0;
        t->active_pipeline->set_count[t->slot]=1;
    }
    /* Retain driver compilation results across synchronous pipeline changes.
     * This does not reuse a VkPipeline with a different render state. */
    if(!t->pipeline_cache) {
        VkPipelineCacheCreateInfo cache={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        TRY(vkCreatePipelineCache(t->device,&cache,NULL,&t->pipeline_cache));
    }
    TRY(pipeline_build(t->device,t->pipeline_cache,t->pass,t->active_pipeline->pipeline_layout,key,t->width,t->height,&t->active_pipeline->pipeline));
fail:
    if(result!=VK_SUCCESS) t->failed=1;
    return result;
}
/* Pipeline recipes. A first build compiles (Turnip: up to ~100 ms each,
 * ~220 on the combat route): mid-game, a new scene's dozens stalled play
 * for seconds. Every pipeline a target builds is appended to a file -- its
 * key and the target properties it depends on -- and at start-up worker
 * threads build each recorded one into the shared pipeline cache (and the
 * driver's), so play finds them compiled (~1 ms). */
typedef struct {
    uint32_t magic,version,width,height,depth_format,input_pass;
    PipelineKey key;
} Recipe;
#define RECIPE_MAGIC 0x50524b56u   /* "VKRP" */
#define RECIPE_VERSION ((uint32_t)sizeof(Recipe)*16u+1u)
#define RECIPE_THREADS 8
static struct {
    pthread_mutex_t lock;
    FILE *file;
    uint64_t *seen;size_t seen_n,seen_cap;
    Recipe *list;size_t count;
    atomic_size_t next;atomic_int stop;atomic_uint built,failed,done;
    pthread_t threads[RECIPE_THREADS];unsigned threads_n;
    VkDevice device;VkPipelineCache cache;uint64_t start_ns;
} s_recipes={.lock=PTHREAD_MUTEX_INITIALIZER};
/* lock held; 1: new */
static int recipe_seen(uint64_t h) {
    if(!h) h=1;
    if(2*(s_recipes.seen_n+1)>s_recipes.seen_cap) {
        size_t cap=s_recipes.seen_cap?s_recipes.seen_cap*2:1024;
        uint64_t *n=calloc(cap,sizeof *n);if(!n) return 0;
        for(size_t i=0;i<s_recipes.seen_cap;i++) if(s_recipes.seen[i]) {
            size_t k=s_recipes.seen[i]&(cap-1);while(n[k]) k=(k+1)&(cap-1);n[k]=s_recipes.seen[i];
        }
        free(s_recipes.seen);s_recipes.seen=n;s_recipes.seen_cap=cap;
    }
    size_t k=h&(s_recipes.seen_cap-1);
    while(s_recipes.seen[k]) {if(s_recipes.seen[k]==h) return 0;k=(k+1)&(s_recipes.seen_cap-1);}
    s_recipes.seen[k]=h;s_recipes.seen_n++;return 1;
}
static void recipe_note(const ChVkTarget *t,const PipelineKey *key) {
    Recipe r;memset(&r,0,sizeof r);
    r.magic=RECIPE_MAGIC;r.version=RECIPE_VERSION;r.width=t->width;r.height=t->height;
    r.depth_format=(uint32_t)t->depth_format;r.input_pass=(uint32_t)t->input_pass;r.key=*key;
    uint64_t h=pipeline_identity(t,key);
    pthread_mutex_lock(&s_recipes.lock);
    if(s_recipes.file && recipe_seen(h)) {fwrite(&r,sizeof r,1,s_recipes.file);fflush(s_recipes.file);}
    pthread_mutex_unlock(&s_recipes.lock);
}
static int recipe_valid(const Recipe *r) {
    return r->magic==RECIPE_MAGIC && r->version==RECIPE_VERSION && r->width && r->height &&
        r->width<=16384 && r->height<=16384 && r->depth_format && r->input_pass<=1 &&
        r->key.shader<=3 && r->key.vertex_count<=136 && r->key.depth.sType==VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
}
static VkResult recipe_build(const Recipe *r) {
    VkDevice device=s_recipes.device;VkResult result=VK_SUCCESS;
    VkRenderPass pass=VK_NULL_HANDLE;VkDescriptorSetLayout set_layout=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
    PipelineKey key=r->key;key.depth.pNext=NULL;
    TRY(target_pass(device,(VkFormat)r->depth_format,(int)r->input_pass,r->width,&pass));
    TRY(pipeline_layout_create(device,&key,&set_layout,&layout));
    TRY(pipeline_build(device,s_recipes.cache,pass,layout,&key,r->width,r->height,&pipeline));
fail:
    if(pipeline) vkDestroyPipeline(device,pipeline,NULL);
    if(layout) vkDestroyPipelineLayout(device,layout,NULL);
    if(set_layout) vkDestroyDescriptorSetLayout(device,set_layout,NULL);
    if(pass) vkDestroyRenderPass(device,pass,NULL);
    return result;
}
static void *recipe_worker(void *arg) {
    (void)arg;
    for(;;) {
        if(atomic_load(&s_recipes.stop)) break;
        size_t i=atomic_fetch_add(&s_recipes.next,1);
        if(i>=s_recipes.count) break;
        if(recipe_build(&s_recipes.list[i])==VK_SUCCESS) atomic_fetch_add(&s_recipes.built,1);
        else atomic_fetch_add(&s_recipes.failed,1);
    }
    if(atomic_fetch_add(&s_recipes.done,1)+1==s_recipes.threads_n)
        fprintf(stderr,"[Vulkan pipelines] precompiled %u of %zu recipes in %.0f ms on %u threads%s\n",
            atomic_load(&s_recipes.built),s_recipes.count,(time_ns()-s_recipes.start_ns)/1e6,s_recipes.threads_n,
            atomic_load(&s_recipes.failed)?" (some failed)":"");
    return NULL;
}
VkResult ch_vk_target_recipes_start(VkDevice device,VkPipelineCache cache,const char *path,unsigned threads) {
    if(!device || !cache || !path || !*path) return VK_ERROR_INITIALIZATION_FAILED;
    ch_vk_target_recipes_stop();
    pthread_mutex_lock(&s_recipes.lock);
    FILE *in=fopen(path,"rb");size_t bad=0;
    if(in) {
        Recipe r;
        while(fread(&r,sizeof r,1,in)==1) {
            if(!recipe_valid(&r)) {bad++;continue;}
            if(!recipe_seen(key_identity(r.width,r.height,r.depth_format,r.input_pass,&r.key))) continue;
            Recipe *grown=realloc(s_recipes.list,(s_recipes.count+1)*sizeof *grown);
            if(!grown) break;
            s_recipes.list=grown;s_recipes.list[s_recipes.count++]=r;
        }
        fclose(in);
    }
    /* A file of another layout (an older build) starts over. */
    s_recipes.file=fopen(path,bad && !s_recipes.count?"wb":"ab");
    if(bad && s_recipes.count) {   /* keep the good ones only */
        fclose(s_recipes.file);s_recipes.file=fopen(path,"wb");
        if(s_recipes.file) {fwrite(s_recipes.list,sizeof *s_recipes.list,s_recipes.count,s_recipes.file);fflush(s_recipes.file);}
    }
    s_recipes.device=device;s_recipes.cache=cache;s_recipes.start_ns=time_ns();
    atomic_store(&s_recipes.next,0);atomic_store(&s_recipes.stop,0);
    atomic_store(&s_recipes.built,0);atomic_store(&s_recipes.failed,0);atomic_store(&s_recipes.done,0);
    if(threads>RECIPE_THREADS) threads=RECIPE_THREADS;
    if(!s_recipes.count) threads=0;
    s_recipes.threads_n=0;
    for(unsigned i=0;i<threads;i++) {
        if(pthread_create(&s_recipes.threads[i],NULL,recipe_worker,NULL)) break;
        s_recipes.threads_n++;
    }
    fprintf(stderr,"[Vulkan pipelines] %zu recipes from %s%s, precompiling on %u threads\n",s_recipes.count,path,
        bad?" (some of another layout dropped)":"",s_recipes.threads_n);
    pthread_mutex_unlock(&s_recipes.lock);
    return s_recipes.file?VK_SUCCESS:VK_ERROR_INITIALIZATION_FAILED;
}
void ch_vk_target_recipes_stop(void) {
    atomic_store(&s_recipes.stop,1);
    for(unsigned i=0;i<s_recipes.threads_n;i++) pthread_join(s_recipes.threads[i],NULL);
    pthread_mutex_lock(&s_recipes.lock);
    s_recipes.threads_n=0;
    if(s_recipes.file) fclose(s_recipes.file);
    s_recipes.file=NULL;free(s_recipes.list);s_recipes.list=NULL;s_recipes.count=0;
    free(s_recipes.seen);s_recipes.seen=NULL;s_recipes.seen_n=s_recipes.seen_cap=0;
    pthread_mutex_unlock(&s_recipes.lock);
}
/* One driver update per draw; all backing info stays live until the call. */
static void update_descriptors(ChVkTarget *t,const ChVkTexture *texture,
    VkBuffer buffer,VkDeviceSize base,VkDeviceSize vertex_bytes)
{
    VkWriteDescriptorSet writes[5];
    VkDescriptorImageInfo images[4],cubes[4];
    VkDescriptorBufferInfo buffers[3];
    unsigned n=0,b=0;
    if(texture || t->rc) {
        unsigned count=t->rc?4:1;
        for(unsigned i=0;i<count;i++) {
            const ChVkTexture *stage=t->rc?(t->stage_images?t->stage_images[i]:NULL):texture;
            images[i]=ch_vk_texture_descriptor(stage && !ch_vk_texture_is_cube(stage)?stage:t->dummy);
            if(t->rc) cubes[i]=ch_vk_texture_descriptor(ch_vk_texture_is_cube(stage)?stage:t->dummy_cube);
        }
        writes[n++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet=t->active_pipeline->descriptor_set,.dstBinding=0,.descriptorCount=count,
            .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=images};
        if(t->rc) {writes[n]=writes[0];writes[n].dstBinding=2;writes[n++].pImageInfo=cubes;}
    }
    if(t->rc) {
        buffers[b]=(VkDescriptorBufferInfo){buffer,base,256};
        writes[n++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet=t->active_pipeline->descriptor_set,.dstBinding=1,.descriptorCount=1,
            .descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&buffers[b++]};
    }
    if(t->fixed) {
        buffers[b]=(VkDescriptorBufferInfo){buffer,base+256,ch_vk_raw_params_size(t->fixed)};
        writes[n++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet=t->active_pipeline->descriptor_set,.dstBinding=3,.descriptorCount=1,
            .descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&buffers[b++]};
        buffers[b]=(VkDescriptorBufferInfo){buffer,t->vertex_offset,vertex_bytes};
        writes[n]=writes[n-1];writes[n].dstBinding=4;writes[n].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[n++].pBufferInfo=&buffers[b];
    }
    if(n) vkUpdateDescriptorSets(t->device,n,writes,0,NULL);
}
static VkResult draw_full(ChVkTarget *t, const float rgba[4],
    const void *vertices, uint32_t vertex_count,
    const uint16_t *indices, uint32_t index_count, const Nv2aRenderState *state,
    const ChVkTexture *texture, uint32_t *samples, const Nv2aCombiner *rc, uint32_t fog_color, const ChVkTexture *const stage_images[4], const ChVkEffect *effect, int coverage_alpha, void *pixels, VkDeviceSize bytes, const ChVkFixedParams *fixed) {
    if(!t || t->failed || !vertices || !indices || !vertex_count || !index_count ||
       index_count%3 || !pixels || bytes<t->bytes)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult copied=retire_copy_out(t);if(copied!=VK_SUCCESS) return copied;
    for(uint32_t i=0;i<index_count;i++) if(indices[i]>=vertex_count)
        return VK_ERROR_INITIALIZATION_FAILED;
    if(samples && !t->queries) return VK_ERROR_FEATURE_NOT_PRESENT;
    if(rc && texture) return VK_ERROR_FEATURE_NOT_PRESENT;
    if(effect && (effect->kind<1 || effect->kind>2 || rc || texture)) return VK_ERROR_FEATURE_NOT_PRESENT;
    if(texture && ch_vk_texture_is_cube(texture)) return VK_ERROR_FEATURE_NOT_PRESENT;
    t->effect=effect!=NULL;
    if(effect) t->push.effect=*effect; else memset(&t->push.effect,0,sizeof(t->push.effect));
    t->rc=rc;t->fixed=fixed;t->stage_images=stage_images;
    t->vertex_offset=fixed?(256+ch_vk_raw_params_size(fixed)+255)/256*256:rc?256:0;
    size_t vertex_stride=fixed?ch_vk_raw_vertex_stride(fixed):sizeof(ChVkColorVertex);
    VkDeviceSize vertex_bytes=fixed && fixed->stream_bytes?fixed->stream_bytes:(VkDeviceSize)vertex_count*vertex_stride;
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
    if(batched && t->ring && t->pending==t->batch_limit) TRY(finish_batch_with_copy(t,0,"batch_limit",NULL,NULL,1));
    else if((!batched || t->pending==t->batch_limit) && t->pending) TRY(finish_batch_named(t,0,__func__));
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
    /* Reference changes need a command, not another graphics pipeline. */
    t->stencil_reference=depth_state.front.reference;
    depth_state.front.reference=depth_state.back.reference=0;
    key.depth=depth_state; key.cull=cull; key.front=front;
    key.shader=effect?3:rc?2:texture?1:0;key.fixed=fixed!=NULL;
    if(fixed && fixed->program_count) {key.vertex_count=fixed->program_count;memcpy(key.vertex_program,fixed->program,sizeof key.vertex_program);}
    if(key.vertex_count && t->specialize_fetch) {
        key.vertex_fetch_specialized=1;
        memcpy(key.vertex_formats,fixed->attribute_format,sizeof key.vertex_formats);
    }
    if(fixed && !key.vertex_count && t->specialize_fetch) {
        key.fixed_fetch[0]=1;
        memcpy(key.fixed_fetch+1,fixed->attribute_format,8*sizeof(uint32_t));
        const Nv2aFixedState *ff=&fixed->state;
        key.fixed_fetch[9]=ff->skin_mode;key.fixed_fetch[10]=ff->use_composite;
        key.fixed_fetch[11]=ff->lighting;key.fixed_fetch[12]=ff->flip_normals;
        key.fixed_fetch[13]=ff->has_normal;key.fixed_fetch[14]=ff->has_color;
        key.fixed_fetch[15]=ff->colmat_amb;key.fixed_fetch[16]=ff->colmat_dif;
        memcpy(key.fixed_fetch+17,ff->texgen_on,4*sizeof(uint32_t));
        memcpy(key.fixed_fetch+21,ff->texmat_on,4*sizeof(uint32_t));
    }
    key.fragment_depth=t->quantized_depth==5 && depth_state.depthTestEnable;
    key.alpha_shader=!!state->alpha_test_enable || !t->split_alpha;
    key.specialized=key.shader==2 && t->specialize_rc;
    if(key.specialized) {
        ch_vk_combiner_program(rc,&key.program);
        if(t->specialize_fetch) {
            key.sample_static=1;
            for(unsigned i=0;i<4;i++) if(stage_images && stage_images[i])
                key.sample_types|=(ch_vk_texture_is_cube(stage_images[i])?3u:1u)<<(i*2);
        }
    }
    PipelineResources *entry=NULL,*oldest=NULL;
    unsigned capacity=t->reuse_resources?(t->specialize_rc?PIPELINE_SLOTS:BASE_PIPELINE_SLOTS):1;
    /* Consecutive draws commonly reuse the selected pipeline. Validate the
     * entire key before bypassing the bounded cache scan; eviction and state
     * changes still follow the normal path. */
    PipelineResources *previous=t->active_pipeline;
    if(previous && previous->pipeline && !memcmp(&key,&previous->key,sizeof key)) {
        entry=previous;t->resource_stats.pipeline_consecutive_hits++;
    }
    /* One never-used slot suffices: all remaining slots have used==0. */
    unsigned search_count=t->pipeline_extent<capacity?t->pipeline_extent+1:capacity;
    for(unsigned i=0;!entry && i<search_count;i++) {
        PipelineResources *p=&t->pipelines[i];
        t->resource_stats.pipeline_scan_entries++;
        if(p!=previous && p->pipeline && !memcmp(&key,&p->key,sizeof key)) {entry=p;break;}
        if(!oldest || p->used<oldest->used) oldest=p;
    }
    if(entry) {t->resource_stats.pipeline_hits++;if(!key.alpha_shader) t->resource_stats.no_alpha_pipeline_hits++;
        if(key.specialized) t->resource_stats.specialized_pipeline_hits++;}
    else {
        entry=oldest;
        if(entry->pipeline) {
            if(entry->pending) TRY(finish_batch_named(t,0,__func__));
            t->resource_stats.pipeline_evictions++;
        }
        unsigned slot=(unsigned)(entry-t->pipelines);
        if(slot>=t->pipeline_extent) t->pipeline_extent=slot+1;
        destroy_pipeline(t,entry);t->active_pipeline=entry;
        const char *pipeline_log=getenv("CHIHIRO_VULKAN_PIPELINE_LOG");
        int log_pipeline=pipeline_log && atoi(pipeline_log);
        if(log_pipeline) {
            fprintf(stderr,"[Vulkan pipeline begin] target=%ux%u shader=%u fixed=%u vertex_program=%u specialized=%u alpha=%u control=%08x stage_program=%08x cull=%u front=%u key=%016llx\n",
                t->width,t->height,key.shader,key.fixed,key.vertex_count,key.specialized,key.alpha_shader,key.program.control,
                rc?rc->stage_program:0,(unsigned)cull,(unsigned)front,(unsigned long long)pipeline_identity(t,&key));
            fflush(stderr);
        }
        uint64_t create_start=time_ns();
        result=create_pipeline(t,&key);
        if(result!=VK_SUCCESS) return result;
        uint64_t create_ns=time_ns()-create_start;t->resource_stats.pipeline_create_ns+=create_ns;
        if(log_pipeline) fprintf(stderr,"[Vulkan pipeline end] %.3fms\n",create_ns/1e6);
        if(create_ns>t->resource_stats.pipeline_create_max_ns) t->resource_stats.pipeline_create_max_ns=create_ns;
        entry->key=key;t->resource_stats.pipeline_builds++;
        recipe_note(t,&key);
        if(!key.alpha_shader) t->resource_stats.no_alpha_pipeline_builds++;
        if(key.specialized) t->resource_stats.specialized_pipeline_builds++;
    }
    memcpy(t->blend_constants,constants,sizeof constants);
    t->active_pipeline=entry;entry->used=++t->pipeline_clock;
    VkDeviceSize header=t->fixed?(256+ch_vk_raw_params_size(t->fixed)+255)/256*256:t->rc?256:0;
    VkDeviceSize payload=header+vertex_bytes+(VkDeviceSize)index_count*sizeof(*indices);
    VkDeviceSize base=t->pending?(t->geometry_used+255)/256*256:0;
    if(base>UINT64_MAX-payload) {result=VK_ERROR_OUT_OF_HOST_MEMORY;goto fail;}
    if(t->pending && base+payload>t->geometry_capacity) {
        if(t->ring && t->batch_enabled) TRY(finish_batch_with_copy(t,0,"geometry_full",NULL,NULL,1));
        else TRY(finish_batch_named(t,0,__func__));
        base=0;
    }
    if(entry->has_descriptors) {
        if(entry->set_used==entry->set_count[t->slot]) {
            VkDescriptorSetAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool=entry->descriptor_pool,.descriptorSetCount=1,.pSetLayouts=&entry->descriptor_layout};
            TRY(vkAllocateDescriptorSets(t->device,&alloc,&entry->sets[t->slot][entry->set_count[t->slot]]));entry->set_count[t->slot]++;
        }
        entry->descriptor_set=entry->sets[t->slot][entry->set_used++];
    }
    t->vertex_offset=base+header;
    VkDeviceSize offset=t->vertex_offset+vertex_bytes;
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
    }
    if(fixed) memcpy((char *)mapped+base+256,fixed,ch_vk_raw_params_size(fixed));
    update_descriptors(t,texture,buffer,base,vertex_bytes);
    memcpy((char *)mapped+t->vertex_offset,vertices,(size_t)vertex_bytes);
    memcpy((char *)mapped+offset,indices,(size_t)(size-offset));
    if(!t->geometry_coherent) {
        VkDeviceSize atom=t->noncoherent_atom;
        VkDeviceSize first=base/atom*atom;
        VkDeviceSize flushed=(size+atom-1)/atom*atom;
        /* Only this draw was written. Earlier batch snapshots were already
         * flushed; align both ends to the device's cache-maintenance atom. */
        VkMappedMemoryRange flush={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory=t->geometry_memory,.offset=first,
            .size=flushed<=t->geometry_capacity?flushed-first:VK_WHOLE_SIZE};
        TRY(vkFlushMappedMemoryRanges(t->device,1,&flush));
    }
    if(batched) {
        result=record_batch(t,buffer,offset,index_count);t->geometry_used=size;
    } else result=render_read(t,rgba,rgba?7:0,1.0f,0,buffer,offset,index_count,pixels,bytes);
    if(result==VK_SUCCESS && samples) *samples=(uint32_t)t->samples;
fail:
    if(t->failed) vkQueueWaitIdle(t->queue);
    if(!t->pending) for(unsigned i=0;i<t->pipeline_extent;i++) t->pipelines[i].set_used=0;
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
    /* Resident clears can share the open render pass with draws. Keep the
     * synchronous path for first use and modes needing a quantization pass. */
    if(!t || t->failed || !pixels || bytes<t->bytes || (flags&~7u) ||
       ((flags&1) && !rgba) || (t->quantized_depth==5 && (flags&2) &&
       (!isfinite(depth) || depth<0 || depth>1))) return VK_ERROR_INITIALIZATION_FAILED;
    if(!flags || !t->initialized || !t->pass || !t->batch_enabled || !t->defer_color_readback ||
       (t->quantized_depth && t->quantized_depth!=5))
        return render_read(t,rgba,flags,depth,stencil,VK_NULL_HANDLE,0,0,pixels,bytes);
    VkResult result;
    if(t->pending==t->batch_limit) TRY(finish_batch_named(t,0,__func__));
    TRY(begin_batch(t));
    VkClearAttachment attachments[2]={0};uint32_t count=0;
    if(flags&1) {
        attachments[count].aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
        memcpy(attachments[count++].clearValue.color.float32,rgba,4*sizeof(float));
    }
    if(flags&6) {
        attachments[count].aspectMask=((flags&2)?VK_IMAGE_ASPECT_DEPTH_BIT:0)|((flags&4)?VK_IMAGE_ASPECT_STENCIL_BIT:0);
        attachments[count++].clearValue.depthStencil=(VkClearDepthStencilValue){
            t->quantized_depth==5 && (flags&2)?ch_vk_depth_unpack(ch_vk_depth_pack_attachment(depth)):depth,stencil};
    }
    VkClearRect rect={.rect={{0,0},{t->width,t->height}},.layerCount=1};
    vkCmdClearAttachments(t->command,count,attachments,1,&rect);
    t->pending++;t->pending_clears++;t->resource_stats.batch_clears++;
    return VK_SUCCESS;
fail:t->failed=1;return result;
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
VkQueue ch_vk_target_queue(const ChVkTarget *t) { return t?t->queue:VK_NULL_HANDLE; }
uint32_t ch_vk_target_family(const ChVkTarget *t) { return t?t->family:0; }
VkResult ch_vk_target_record_copy_to_buffer(ChVkTarget *t,VkCommandBuffer command,VkBuffer destination,
    VkDeviceSize offset,VkDeviceSize capacity) {
    if(!t || t->failed || !t->initialized || !command || !destination || (offset&3) || capacity<offset || capacity-offset<t->bytes)
        return VK_ERROR_INITIALIZATION_FAILED;
    /* This target's recorded work goes to the queue first, without a wait;
     * the copy is ordered after it on the same queue by the barrier. */
    live_release(t);
    VkResult r=ch_vk_target_submit(t);
    if(r!=VK_SUCCESS) return r;
    if(t->layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryBarrier ready={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&ready,0,NULL,0,NULL);
    VkBufferImageCopy copy={.bufferOffset=offset,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={t->width,t->height,1}};
    vkCmdCopyImageToBuffer(command,t->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,destination,1,&copy);
    VkBufferMemoryBarrier done={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=destination,.offset=offset,.size=t->bytes};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,1,&done,0,NULL);
    return VK_SUCCESS;
}
static int ring_inflight(const ChVkTarget *t) {
    if(t->ring) for(unsigned k=0;k<RING_SLOTS;k++) if(t->slots[k].inflight) return 1;
    return 0;
}
VkFence ch_vk_target_completion_fence(const ChVkTarget *t) { return t && !t->pending && !t->copy_source && !ring_inflight(t)?(t->upload_pending?t->upload_fence:t->fence):VK_NULL_HANDLE; }
VkResult ch_vk_target_record_export(ChVkTarget *t,VkCommandBuffer command,VkImage destination) {
    if(!t || t->failed || !t->initialized || !command || !destination || destination==t->image) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=ch_vk_target_submit(t);   /* pending draws, not waited for */
    if(r!=VK_SUCCESS) return r;
    if(t->pending || t->layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) return VK_NOT_READY;
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=destination,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkImageCopy copy={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.extent={t->width,t->height,1}};
    vkCmdCopyImage(command,t->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,destination,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,0,0,NULL,0,NULL,1,&barrier);
    /* The target's next batch waits for transfer reads (its first barrier). */
    return VK_SUCCESS;
}
VkResult ch_vk_target_export_image(ChVkTarget *t,VkImage destination,void *verify,VkDeviceSize bytes) {
    if(!t || t->failed || !t->initialized || !destination || destination==t->image ||
        (verify && bytes<t->bytes)) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result;
    TRY(finish_batch_named(t,0,__func__));
    TRY(vkResetCommandBuffer(t->command,0));
    TRY(vkResetFences(t->device,1,&t->fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vkBeginCommandBuffer(t->command,&begin));
    color_staging_reuse(t);
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
        TRY(map_readback(t,0,&mapped));
        result=maintain_staging(t,0,0,0,t->bytes);
        if(result==VK_SUCCESS) memcpy(verify,mapped,(size_t)t->bytes);
        if(result!=VK_SUCCESS) goto fail;
    }
    return VK_SUCCESS;
fail:
    t->failed=1; return result;
}

/* The color image, tightly packed RGBA8, into a buffer of this target's
 * device (a shared bridge buffer another device reads), complete on return. */
VkResult ch_vk_target_copy_to_buffer(ChVkTarget *t,VkBuffer destination,VkDeviceSize capacity) {
    return ch_vk_target_copy_to_buffer_at(t,destination,0,capacity);
}
VkResult ch_vk_target_copy_to_buffer_at(ChVkTarget *t,VkBuffer destination,VkDeviceSize offset,VkDeviceSize capacity) {
    if(!t || t->failed || !t->initialized || !destination || (offset&3) || capacity<offset || capacity-offset<t->bytes) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result;
    TRY(finish_batch_named(t,0,__func__));
    TRY(vkResetCommandBuffer(t->command,0));
    TRY(vkResetFences(t->device,1,&t->fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vkBeginCommandBuffer(t->command,&begin));
    color_staging_reuse(t);
    VkBufferImageCopy copy={.bufferOffset=offset,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={t->width,t->height,1}};
    vkCmdCopyImageToBuffer(t->command,t->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,destination,1,&copy);
    VkBufferMemoryBarrier done={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=destination,.size=VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,1,&done,0,NULL);
    TRY(vkEndCommandBuffer(t->command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&t->command};
    TRY(vkQueueSubmit(t->queue,1,&submit,t->fence));
    TRY(vkWaitForFences(t->device,1,&t->fence,VK_TRUE,5000000000ULL));
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

VkResult ch_vk_target_color_bgra(ChVkTarget *t,int enabled) {
    return ch_vk_target_color_layout(t,enabled?1:0);
}
VkResult ch_vk_target_color_layout(ChVkTarget *t,int mode) {
    if(!t || t->failed) return VK_ERROR_INITIALIZATION_FAILED;
    if(t->color_bgra==mode) return VK_SUCCESS;
    VkResult result;TRY(finish_batch_named(t,0,__func__));
    if(mode<0 || mode>2 || (mode==2 && ((t->width&(t->width-1)) || (t->height&(t->height-1))))) return VK_ERROR_FEATURE_NOT_PRESENT;
    if(mode && !t->color_pack) {
        VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(t->physical,&properties);
        if(2*t->bytes>properties.limits.maxStorageBufferRange || t->bytes/4>UINT32_MAX-255u ||
           (t->bytes/4+255u)/256u>properties.limits.maxComputeWorkGroupCount[0]) return VK_ERROR_FEATURE_NOT_PRESENT;
        TRY(ensure_paired_color(t));
        TRY(ch_vk_color_pack_create(t->device,t->pipeline_cache,t->read,2*t->bytes,&t->color_pack));
        TRY(ch_vk_color_pack_create(t->device,t->pipeline_cache,t->paired_color,2*t->bytes,&t->paired_color_pack));
        if(t->sampled_color_read) {
            TRY(ch_vk_image_read_create(t->device,t->pipeline_cache,t->read,2*t->bytes,
                t->image,VK_FORMAT_R8G8B8A8_UNORM,&t->color_read));
            TRY(ch_vk_image_read_create(t->device,t->pipeline_cache,t->paired_color,2*t->bytes,
                t->image,VK_FORMAT_R8G8B8A8_UNORM,&t->paired_color_read));
        }
    }
    t->color_bgra=mode;return VK_SUCCESS;
fail:
    t->failed=1;return result;
}

VkResult ch_vk_target_import_color(ChVkTarget *t,const void *rgba,VkDeviceSize bytes) {
    if(!t || t->failed || !t->initialized || !rgba || bytes<t->bytes) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result; void *mapped;
    TRY(finish_batch_named(t,0,__func__));
    TRY(map_readback(t,0,&mapped));
    memcpy((char *)mapped+(t->color_bgra==2?t->bytes:0),rgba,(size_t)t->bytes);
    result=maintain_staging(t,0,1,t->color_bgra==2?t->bytes:0,t->bytes);
    if(result!=VK_SUCCESS) goto fail;
    TRY(vkResetCommandBuffer(t->command,0)); TRY(vkResetFences(t->device,1,&t->fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vkBeginCommandBuffer(t->command,&begin));
    color_staging_reuse(t);
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=t->image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={t->width,t->height,1}};
    if(t->color_bgra) ch_vk_color_pack_record(t->color_pack,t->command,t->width,t->height,1,t->color_bgra==2);
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

VkResult ch_vk_target_import_buffer(ChVkTarget *t,VkBuffer source,VkDeviceSize capacity) {
    return ch_vk_target_import_buffer_at(t,source,0,capacity);
}
VkResult ch_vk_target_import_buffer_at(ChVkTarget *t,VkBuffer source,VkDeviceSize offset,VkDeviceSize capacity) {
    if(!t || t->failed || !t->initialized || !source || (offset&3) || capacity<offset || capacity-offset<t->bytes) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result;
    TRY(finish_batch_named(t,0,__func__));
    if(t->layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) return VK_ERROR_INITIALIZATION_FAILED;
    TRY(vkResetCommandBuffer(t->command,0)); TRY(vkResetFences(t->device,1,&t->fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vkBeginCommandBuffer(t->command,&begin));
    VkBufferMemoryBarrier ready={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=source,.size=VK_WHOLE_SIZE};
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=t->image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(t->command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,1,&ready,1,&barrier);
    VkBufferImageCopy copy={.bufferOffset=offset,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={t->width,t->height,1}};
    vkCmdCopyBufferToImage(t->command,source,t->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
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

VkResult ch_vk_target_depth_morton(ChVkTarget *t,int enabled) {
    if(!t || t->failed) return VK_ERROR_INITIALIZATION_FAILED;
    if(t->depth_morton==!!enabled) return VK_SUCCESS;
    if(enabled && ((t->width&(t->width-1)) || (t->height&(t->height-1)))) return VK_ERROR_FEATURE_NOT_PRESENT;
    if(enabled) {
        VkPhysicalDeviceProperties p;vkGetPhysicalDeviceProperties(t->physical,&p);
        if(2*t->bytes+(t->bytes/4+3)/4*4>p.limits.maxStorageBufferRange ||
           t->bytes/4>UINT32_MAX-255u || (t->bytes/4+255u)/256u>p.limits.maxComputeWorkGroupCount[0])
            return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    VkResult result=finish_batch_named(t,0,__func__);
    if(result==VK_SUCCESS) t->depth_morton=!!enabled;
    return result;
}

static VkResult transfer_depth_stencil(ChVkTarget *t,void *depth,void *stencil,size_t pixels,int upload,int packed,void *rgba,size_t color_bytes,int phase) {
    if(!t || t->failed || !t->initialized || !depth || (!packed && !stencil) || pixels<t->bytes/4)
        return VK_ERROR_INITIALIZATION_FAILED;
    if(rgba && (upload || color_bytes<t->bytes)) return VK_ERROR_INITIALIZATION_FAILED;
    if(t->color_readback_active || (phase==2)!=!!t->readback_active) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult copied=retire_copy_out(t);if(copied!=VK_SUCCESS) return copied;
    int gpu_packed=packed;
    if(gpu_packed && !t->depth_pack) {
        uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(t->physical,&count,NULL);
        VkQueueFamilyProperties *families=calloc(count,sizeof *families);
        if(!families) return VK_ERROR_OUT_OF_HOST_MEMORY;
        vkGetPhysicalDeviceQueueFamilyProperties(t->physical,&count,families);
        int compute=t->family<count && (families[t->family].queueFlags&VK_QUEUE_COMPUTE_BIT);free(families);
        if(!compute) return VK_ERROR_FEATURE_NOT_PRESENT;
        VkResult r=ch_vk_depth_pack_create(t->device,t->pipeline_cache,t->read,2*t->bytes+(t->bytes/4+3)/4*4,&t->depth_pack);
        if(r!=VK_SUCCESS) return r;
    }
    if(gpu_packed && !upload && t->sampled_depth_read && !t->depth_read) {
        VkResult r=ch_vk_image_read_create(t->device,t->pipeline_cache,t->read,
            2*t->bytes+(t->bytes/4+3)/4*4,t->depth,t->depth_format,&t->depth_read);
        if(r!=VK_SUCCESS) return r;
    }
    if(upload && !packed) for(size_t i=0;i<t->bytes/4;i++) {
        float d=((const float *)depth)[i];
        if(!isfinite(d) || d<0 || d>1) return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint64_t upload_start=t->timing && upload?time_ns():0;
    VkResult result; void *mapped;
    if(rgba) {result=ensure_paired_color(t);if(result!=VK_SUCCESS) return result;}
    if(gpu_packed && rgba && t->depth_read && t->paired_color_read && t->color_bgra && !t->depth_color_read) {
        result=ch_vk_image_read_create_pair(t->device,t->pipeline_cache,t->read,
            2*t->bytes+(t->bytes/4+3)/4*4,t->depth,t->image,t->paired_color,2*t->bytes,&t->depth_color_read);
        if(result!=VK_SUCCESS) return result;
    }
    t->packed_transfer=gpu_packed;
    VkDeviceSize packed_offset=t->bytes+(t->bytes/4+3)/4*4;
    if(phase==2) {
        if(t->readback_batched) TRY(retire_batch(t));
        else TRY(vkWaitForFences(t->device,1,&t->fence,VK_TRUE,5000000000ULL));
        goto readback;
    }
    if(phase==1) {
        t->readback_request.depth=depth;t->readback_request.stencil=stencil;
        t->readback_request.rgba=rgba;t->readback_request.pixels=pixels;
        t->readback_request.color_bytes=color_bytes;t->readback_request.packed=packed;
        t->readback_batched=!!t->pending;
    }
    if(!upload && t->pending) {
        TRY(finish_batch_with_copy(t,rgba?3:1,__func__,NULL,NULL,phase==1));
        if(phase==1) {t->readback_active=1;return VK_SUCCESS;}
        goto readback;
    }

    if(upload) {
        TRY(finish_batch_named(t,0,__func__));
        TRY(map_readback(t,0,&mapped));
        if(gpu_packed) memcpy((char *)mapped+packed_offset,depth,(size_t)t->bytes);
        else if(t->depth_format==VK_FORMAT_D24_UNORM_S8_UINT) {
            uint32_t *z=mapped;
            for(size_t i=0;i<t->bytes/4;i++)
                z[i]=(uint32_t)((double)((const float *)depth)[i]*16777215.0+0.5);
        } else if(t->quantized_depth==5) {
            float *values=mapped;
            for(size_t i=0;i<t->bytes/4;i++) values[i]=ch_vk_depth_unpack(ch_vk_depth_pack(((const float *)depth)[i]));
        } else memcpy(mapped,depth,(size_t)t->bytes);
        if(!packed) memcpy((char *)mapped+t->bytes,stencil,(size_t)t->bytes/4);
        result=maintain_staging(t,0,1,gpu_packed?packed_offset:0,gpu_packed?t->bytes:t->bytes+t->bytes/4);
        if(result!=VK_SUCCESS) goto fail;
    }
    TRY(finish_batch_named(t,0,__func__));
    int queued=upload && packed && t->batch_enabled && t->async_depth_uploads;
    if(queued && !t->upload_command) {
        VkCommandBufferAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool=t->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
        TRY(vkAllocateCommandBuffers(t->device,&alloc,&t->upload_command));
        VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        TRY(vkCreateFence(t->device,&fence,NULL,&t->upload_fence));
    }
    VkCommandBuffer command=queued?t->upload_command:t->command;
    VkFence fence=queued?t->upload_fence:t->fence;
    TRY(vkResetCommandBuffer(command,0)); TRY(vkResetFences(t->device,1,&fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vkBeginCommandBuffer(command,&begin));
    color_staging_reuse_on(t,command);
    int paired_read=record_depth_transfer(t,command,upload,rgba!=NULL);
    if(rgba && !paired_read) record_paired_color(t);
    TRY(vkEndCommandBuffer(command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    TRY(vkQueueSubmit(t->queue,1,&submit,fence));
    if(queued) {t->upload_pending=1;t->resource_stats.queued_depth_uploads++;return VK_SUCCESS;}
    if(phase==1) {t->readback_active=1;return VK_SUCCESS;}
    uint64_t upload_wait=upload_start?time_ns():0;
    TRY(vkWaitForFences(t->device,1,&t->fence,VK_TRUE,5000000000ULL));
    if(upload_start) {uint64_t elapsed=time_ns()-upload_wait;
        t->depth_upload_wait_ns+=elapsed;t->timing_stats.depth_upload_wait_ns+=elapsed;}
readback:
    if(!upload) {
        TRY(map_readback(t,0,&mapped));
        result=maintain_staging(t,0,0,gpu_packed?packed_offset:0,gpu_packed?t->bytes:t->bytes+t->bytes/4);
        if(result==VK_SUCCESS) {
            if(gpu_packed) memcpy(depth,(char *)mapped+packed_offset,(size_t)t->bytes);
            else if(t->depth_format==VK_FORMAT_D24_UNORM_S8_UINT) {
                const uint32_t *z=mapped;
                for(size_t i=0;i<t->bytes/4;i++)
                    ((float *)depth)[i]=(float)((double)(z[i]&0xffffffu)/16777215.0);
            } else memcpy(depth,mapped,(size_t)t->bytes);
            if(!packed) memcpy(stencil,(char *)mapped+t->bytes,(size_t)t->bytes/4);
        }
        if(result!=VK_SUCCESS) goto fail;
    }
    if(rgba) {
        TRY(map_readback(t,1,&mapped));
        result=maintain_staging(t,1,0,t->color_bgra==2?t->bytes:0,t->bytes);
        if(result==VK_SUCCESS) memcpy(rgba,(char *)mapped+(t->color_bgra==2?t->bytes:0),(size_t)t->bytes);
        if(result!=VK_SUCCESS) goto fail;
    }
    if(upload_start) {t->depth_uploads++;t->depth_upload_ns+=time_ns()-upload_start;}
    t->readback_active=0;t->packed_transfer=0;return VK_SUCCESS;
fail:
    t->readback_active=0;t->packed_transfer=0;t->failed=1; return result;
}
VkResult ch_vk_target_read_depth_begin(ChVkTarget *t,void *depth,uint8_t *stencil,size_t pixels,
    int packed,void *rgba,size_t bytes) {
    return transfer_depth_stencil(t,depth,stencil,pixels,0,!!packed,rgba,bytes,1);
}
VkResult ch_vk_target_read_depth_end(ChVkTarget *t) {
    if(!t || !t->readback_active) return VK_ERROR_INITIALIZATION_FAILED;
    return transfer_depth_stencil(t,t->readback_request.depth,t->readback_request.stencil,
        t->readback_request.pixels,0,t->readback_request.packed,t->readback_request.rgba,
        t->readback_request.color_bytes,2);
}
VkResult ch_vk_target_read_depth_color(ChVkTarget *t,void *depth,uint8_t *stencil,size_t pixels,
    int packed,void *rgba,size_t color_bytes) {
    if(!rgba) return VK_ERROR_INITIALIZATION_FAILED;
    return transfer_depth_stencil(t,depth,stencil,pixels,0,!!packed,rgba,color_bytes,0);
}
VkResult ch_vk_target_read_depth_stencil(ChVkTarget *t,float *depth,uint8_t *stencil,size_t pixels) {
    return transfer_depth_stencil(t,depth,stencil,pixels,0,0,NULL,0,0);
}
VkResult ch_vk_target_write_depth_stencil(ChVkTarget *t,const float *depth,const uint8_t *stencil,size_t pixels) {
    return transfer_depth_stencil(t,(void *)depth,(void *)stencil,pixels,1,0,NULL,0,0);
}
VkResult ch_vk_target_read_z24s8(ChVkTarget *t,uint32_t *words,size_t pixels) {
    return transfer_depth_stencil(t,words,NULL,pixels,0,1,NULL,0,0);
}
VkResult ch_vk_target_write_z24s8(ChVkTarget *t,const uint32_t *words,size_t pixels) {
    return transfer_depth_stencil(t,(void *)words,NULL,pixels,1,1,NULL,0,0);
}

static void record_depth_copy(VkCommandBuffer command,ChVkTarget *s,ChVkTarget *t) {
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
    vkCmdPipelineBarrier(command,depth_stages,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,2,barriers);
    VkImageCopy regions[2];
    for(unsigned i=0;i<2;i++) regions[i]=(VkImageCopy){
        .srcSubresource={i?VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1},
        .dstSubresource={i?VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1},
        .extent={t->width,t->height,1}};
    vkCmdCopyImage(command,s->depth,barriers[0].newLayout,t->depth,barriers[1].newLayout,2,regions);
    for(unsigned i=0;i<2;i++) {
        barriers[i].srcAccessMask=barriers[i].dstAccessMask;
        barriers[i].dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barriers[i].oldLayout=barriers[i].newLayout;
        barriers[i].newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,depth_stages,0,0,NULL,0,NULL,2,barriers);
}

static VkResult copy_depth_stencil(ChVkTarget *source,ChVkTarget *destination,int queued) {
    ChVkTarget *s=source,*t=destination;
    if(!s || !t || s->failed || t->failed || !s->initialized || !t->initialized ||
       s->device!=t->device || s->queue!=t->queue || s->family!=t->family ||
       s->width!=t->width || s->height!=t->height || s->depth_format!=t->depth_format)
        return VK_ERROR_INITIALIZATION_FAILED;
    if(s->readback_active || s->color_readback_active || t->readback_active || t->color_readback_active)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result;
    TRY(retire_copy_out(s));TRY(retire_copy_out(t));
    TRY(wait_copy_source(s));TRY(wait_copy_source(t));
    if(queued && s->async_depth_copies && s!=t && s->pending) {
        /* Pending targets use distinct attachments and immutable sampled
         * snapshots. Submit the older destination accesses before overwriting
         * its depth, then append the copy to the source batch. Retire only the
         * destination's old fence before associating the later copy: its old
         * fence does not cover that incoming copy. */
        int destination_pending=!!t->pending;
        if(destination_pending) TRY(finish_batch_with_copy(t,0,__func__,NULL,NULL,1));
        result=finish_batch_with_copy(s,0,__func__,t,NULL,1);
        if(result==VK_SUCCESS && destination_pending) result=retire_batch(t);
        if(result!=VK_SUCCESS) {
            if(t->submitted) (void)retire_batch(t);
            if(s->submitted) (void)retire_batch(s);
            goto fail;
        }
        s->copy_destination=t;t->copy_source=s;s->resource_stats.queued_depth_copies++;
        return VK_SUCCESS;
    }
    /* Synchronous callers can also combine an idle-destination copy with
     * source rendering, but retain the public completion guarantee. */
    if(s!=t && s->pending && !t->pending) {
        result=finish_batch_with_copy(s,0,__func__,t,NULL,0);
        if(result!=VK_SUCCESS) goto fail;
        return VK_SUCCESS;
    }
    TRY(finish_batch_named(s,0,__func__));
    if(s==t) return VK_SUCCESS;
    TRY(finish_batch_named(t,0,__func__));
    TRY(vkResetCommandBuffer(t->command,0));TRY(vkResetFences(t->device,1,&t->fence));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    TRY(vkBeginCommandBuffer(t->command,&begin));
    color_staging_reuse(t);
    record_depth_copy(t->command,s,t);
    TRY(vkEndCommandBuffer(t->command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&t->command};
    TRY(vkQueueSubmit(t->queue,1,&submit,t->fence));
    TRY(vkWaitForFences(t->device,1,&t->fence,VK_TRUE,5000000000ULL));
    return VK_SUCCESS;
fail:
    s->failed=t->failed=1;return result;
}

VkResult ch_vk_target_copy_depth_stencil(ChVkTarget *source,ChVkTarget *destination) {
    uint64_t start=source && source->timing?time_ns():0;
    VkResult result=copy_depth_stencil(source,destination,0);
    if(start) {
        source->timing_stats.depth_copy_ns+=time_ns()-start;
        source->timing_stats.depth_copy_calls++;
    }
    return result;
}

VkResult ch_vk_target_copy_depth_stencil_queued(ChVkTarget *source,ChVkTarget *destination) {
    uint64_t start=source && source->timing?time_ns():0;
    VkResult result=copy_depth_stencil(source,destination,1);
    if(start) {
        source->timing_stats.depth_copy_ns+=time_ns()-start;
        source->timing_stats.depth_copy_calls++;
    }
    return result;
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

static VkResult draw_fixed(ChVkTarget *t,const ChVkFixedParams *fixed,
    const ChVkFixedVertex *v,uint32_t n,const uint16_t *indices,uint32_t count,
    const Nv2aRenderState *state,const ChVkTexture *texture,uint32_t *samples,
    const Nv2aCombiner *rc,uint32_t fog,const ChVkTexture *const stages[4],int coverage_alpha,void *pixels,VkDeviceSize bytes) {
    if(!fixed || fixed->program_count>136 || !fixed->surface[0] || !fixed->surface[1] || !(fixed->depth[1]>fixed->depth[0]))
        return VK_ERROR_INITIALIZATION_FAILED;
    if(fixed->stream_bytes) {
        if(!n || fixed->stream_bytes<4 || (fixed->stream_bytes&3)) return VK_ERROR_INITIALIZATION_FAILED;
        for(unsigned a=0;a<(fixed->program_count?16u:8u);a++) {
            uint32_t f=fixed->attribute_format[a],kind=f&255,size=f>>8;
            unsigned element=kind==0?16:kind==1 || kind==7?4:kind==2 || kind==6?size*2:kind==5?size:kind==3?size*4:0;
            if(!element || (kind && (size<1 || size>4)) ||
               (uint64_t)fixed->attribute_offset[a]+(uint64_t)(n-1)*fixed->attribute_stride[a]+element>fixed->stream_bytes-4)
                return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    return draw_full(t,NULL,v,n,indices,count,state,texture,samples,rc,fog,stages,NULL,coverage_alpha,pixels,bytes,fixed);
}
VkResult ch_vk_target_draw_fixed_read(ChVkTarget *t,const ChVkFixedParams *fixed,
    const ChVkFixedVertex *v,uint32_t n,const uint16_t *indices,uint32_t count,
    const Nv2aRenderState *state,const ChVkTexture *texture,uint32_t *samples,
    const Nv2aCombiner *rc,uint32_t fog,const ChVkTexture *const stages[4],void *pixels,VkDeviceSize bytes) {
    return draw_fixed(t,fixed,v,n,indices,count,state,texture,samples,rc,fog,stages,0,pixels,bytes);
}
VkResult ch_vk_target_draw_fixed_coverage_read(ChVkTarget *t,const ChVkFixedParams *fixed,
    const ChVkFixedVertex *v,uint32_t n,const uint16_t *indices,uint32_t count,
    const Nv2aRenderState *state,const ChVkTexture *texture,
    const Nv2aCombiner *rc,uint32_t fog,const ChVkTexture *const stages[4],void *pixels,VkDeviceSize bytes) {
    return draw_fixed(t,fixed,v,n,indices,count,state,texture,NULL,rc,fog,stages,1,pixels,bytes);
}
