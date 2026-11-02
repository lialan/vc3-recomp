#include "vk_texture_cache.h"
#include "../nv2a_texture_cube.h"
#include "../d3d/d3d8_swizzle.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
struct Entry {
    Nv2aTexture key;
    ChVkTexture *image;
    unsigned char *raw;
    size_t span,palette,cost;
    uint64_t used,frame;
    unsigned pins;
    uint32_t lookup_next; /* bucket chain: entry index + 1 */
    ChVkTarget *source;
    uint64_t serial;
    ChGuestWriteRange *raw_watch,*palette_watch;
    int observed_valid,observed_same;
};
#define LOOKUP_HINTS 1024u
struct ChVkTextureCache {
    ChVkTextureCacheConfig config;
    struct Entry *entries;
    uint64_t clock,frame;
    uint32_t hints[LOOKUP_HINTS]; /* entry index + 1, always validated */
    uint32_t buckets[LOOKUP_HINTS];
    uint32_t extent; /* Skip the never-populated tail of the bounded table. */
    ChVkTextureCacheStats stats;
    int trace;
};
static uint64_t cache_us(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000u+t.tv_nsec/1000;
}
static void trace_miss(ChVkTextureCache *c,const Nv2aTexture *t,uint64_t start,uint64_t room,uint64_t snapshot,uint64_t upload,int target,int reuse) {
    if(!c->trace) return;
    uint64_t end=cache_us();if(end-start<1000) return;
    fprintf(stderr,"[Vulkan texture miss] %ux%u format=%02x target=%d reuse=%d room_ms=%.3f snapshot_ms=%.3f upload_ms=%.3f watch_ms=%.3f\n",
        t->width,t->height,t->color,target,reuse,(room-start)/1000.,(snapshot-room)/1000.,(upload-snapshot)/1000.,(end-upload)/1000.);
}
VkResult ch_vk_texture_storage_span(const Nv2aTexture *t,size_t *span,size_t *palette) {
    if(!span || !palette || !t || !t->width || !t->height || (uint64_t)t->width*t->height>SIZE_MAX/4)
        return VK_ERROR_INITIALIZATION_FAILED;
    if(t->cube) {
        size_t face=ch_nv2a_cube_face_bytes(t);
        if(!face || (!d3d8_format_is_swizzled(t->color) && !d3d8_format_dxt_block_bytes(t->color)))
            return VK_ERROR_FEATURE_NOT_PRESENT;
        *span=face*6;*palette=t->color==0x0b?1024:0;
        return *palette && !t->palette?VK_ERROR_FORMAT_NOT_SUPPORTED:VK_SUCCESS;
    }
    uint64_t n;
    uint32_t block=d3d8_format_dxt_block_bytes(t->color),bpp;
    if(block) n=(((uint64_t)t->width+3)/4)*(((uint64_t)t->height+3)/4)*block;
    else {
        switch(t->color) {
        case 0x00:case 0x01:case 0x0b:case 0x13:case 0x19:case 0x1f:bpp=1;break;
        case 0x02:case 0x03:case 0x04:case 0x05:case 0x10:case 0x11:
        case 0x1a:case 0x1c:case 0x1d:case 0x24:case 0x25:bpp=2;break;
        case 0x06:case 0x07:case 0x12:case 0x1e:case 0x3a:case 0x3b:case 0x3c:
        case 0x3f:case 0x40:case 0x41:bpp=4;break;
        default:return VK_NOT_READY;
        }
        if(d3d8_format_is_swizzled(t->color)) {
            if((t->width&(t->width-1)) || (t->height&(t->height-1))) return VK_NOT_READY;
            n=(uint64_t)t->width*t->height*bpp;
        } else {
            /* Packed 4:2:2 reads shared chroma from the complete four-byte
             * pair, including the partner of an odd final texel. */
            uint64_t row=(t->color==0x24 || t->color==0x25)?
                (((uint64_t)t->width+1)&~1ull)*2:(uint64_t)t->width*bpp;
            n=(uint64_t)(t->height-1)*t->pitch+row;
        }
    }
    if(n>SIZE_MAX) return VK_ERROR_INITIALIZATION_FAILED;
    *span=(size_t)n;*palette=t->color==0x0b?1024:0;
    if(*palette && !t->palette) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    return VK_SUCCESS;
}
static const unsigned char *guest(const ChVkTextureCache *c,uint32_t va) {
    return (const unsigned char *)c->config.guest_base+va;
}
static int unchanged(ChVkTextureCache *c,struct Entry *e,const Nv2aTexture *t) {
    if(e->raw_watch) {
        int changed=ch_guest_write_range_changed(e->raw_watch);
        if(e->palette) changed|=ch_guest_write_range_changed(e->palette_watch);
        /* A consumed generation must not turn a previously mismatched pinned
         * snapshot back into a hit on the next acquire. Remember both results. */
        if(e->observed_valid && !changed) {c->stats.tracked_checks++;return e->observed_same;}
    }
    /* Logical comparison span, not measured memory traffic: memcmp may stop
     * at the first mismatch or reuse bytes already in the CPU cache. */
    c->stats.cpu_checks++;c->stats.cpu_check_bytes+=e->span+e->palette;
    int same=!memcmp(e->raw,guest(c,t->offset),e->span) &&
        (!e->palette || !memcmp(e->raw+e->span,guest(c,t->palette),e->palette));
    c->stats.cpu_changes+=!same;e->observed_valid=1;e->observed_same=same;return same;
}
static uint32_t hint_bucket(const Nv2aTexture *,const ChVkTarget *);
static void index_entry(ChVkTextureCache *c,struct Entry *e) {
    uint32_t index=(uint32_t)(e-c->entries)+1;
    uint32_t *link=&c->buckets[hint_bucket(&e->key,e->source)];
    /* Preserve the original ascending-slot lookup/reuse order. */
    while(*link && *link<index) link=&c->entries[*link-1].lookup_next;
    e->lookup_next=*link;*link=index;
}
static void drop(ChVkTextureCache *c,struct Entry *e) {
    if(!e->image) return;
    if(e->pins) abort();
    uint32_t index=(uint32_t)(e-c->entries)+1;
    uint32_t *link=&c->buckets[hint_bucket(&e->key,e->source)];
    while(*link && *link!=index) link=&c->entries[*link-1].lookup_next;
    if(!*link) abort();
    *link=e->lookup_next;
    ch_vk_texture_destroy(e->image);free(e->raw);
    ch_guest_write_range_destroy(e->raw_watch);ch_guest_write_range_destroy(e->palette_watch);
    c->stats.bytes-=e->cost;c->stats.evictions++;c->stats.entries--;
    memset(e,0,sizeof *e);
}
static struct Entry *oldest(ChVkTextureCache *c,size_t needed) {
    struct Entry *found=NULL,*fit=NULL,*largest=NULL;
    for(uint32_t i=0;i<c->extent;i++) {
        struct Entry *e=&c->entries[i];
        if(!e->image || e->pins) continue;
        if(!found || e->used<found->used) found=e;
        /* Avoid destroying dozens of small stale images for one large image.
         * Protect the current/previous frame's working set; entry-count
         * pressure and clients without frame boundaries retain exact LRU. */
        if(!needed || !c->config.size_aware_eviction || c->frame-e->frame<2) continue;
        if(e->cost>=needed && (!fit || e->cost<fit->cost || (e->cost==fit->cost && e->used<fit->used))) fit=e;
        if(!largest || e->cost>largest->cost || (e->cost==largest->cost && e->used<largest->used)) largest=e;
    }
    return fit?fit:largest?largest:found;
}
static struct Entry *make_room(ChVkTextureCache *c,size_t cost) {
    if(cost>c->config.max_bytes) return NULL;
    struct Entry *slot=NULL;
    for(uint32_t i=0;i<c->extent;i++) if(!c->entries[i].image) {slot=&c->entries[i];break;}
    if(!slot && c->extent<c->config.max_entries) slot=&c->entries[c->extent];
    while(c->stats.bytes>c->config.max_bytes-cost || !slot) {
        size_t needed=c->stats.bytes>c->config.max_bytes-cost?c->stats.bytes-(c->config.max_bytes-cost):0;
        struct Entry *e=oldest(c,needed);
        if(!e) return NULL;
        drop(c,e);if(!slot) slot=e;
    }
    return slot;
}
static VkResult image_cost(const ChVkTextureCache *c,const Nv2aTexture *t,size_t *cost) {
    size_t pixels=0;uint32_t width=t->width,height=t->height,levels=ch_nv2a_texture_mip_levels(t,1);
    for(uint32_t i=0;i<levels;i++) {
        uint64_t bytes=(uint64_t)width*height*4*(t->cube?6:1);
        if(bytes>c->config.max_bytes || pixels>c->config.max_bytes-bytes) return VK_NOT_READY;
        pixels+=(size_t)bytes;width=width>1?width/2:1;height=height>1?height/2:1;
    }
    /* Reserve the base-level diagnostic/readback staging cost even for GPU
     * snapshots, which only allocate that buffer if it is actually read. */
    uint64_t staging=(uint64_t)t->width*t->height*4*(t->cube?6:1);
    if(staging>c->config.max_bytes-pixels) return VK_NOT_READY;
    *cost=pixels+(size_t)staging;return VK_SUCCESS;
}
/* Hints deliberately hash only cheap identity fields. Collisions, slot reuse
 * and different sampler states are resolved by the complete key check. */
static uint32_t hint_bucket(const Nv2aTexture *t,const ChVkTarget *source) {
    uint64_t h=(uint64_t)t->offset*0x9e3779b1u;
    h^=(uint64_t)t->palette*0x85ebca6bu;
    h^=(uint64_t)t->width<<17;h^=(uint64_t)t->height<<5;
    h^=(uintptr_t)source>>4;h^=h>>32;h^=h>>16;
    return (uint32_t)h&(LOOKUP_HINTS-1);
}
static struct Entry *lookup_hint(ChVkTextureCache *c,const Nv2aTexture *t,ChVkTarget *source) {
    uint32_t index=c->hints[hint_bucket(t,source)];
    if(!index || index>c->extent) return NULL;
    struct Entry *e=&c->entries[index-1];
    if(!e->image || e->source!=source || memcmp(&e->key,t,sizeof *t)) return NULL;
    return e;
}
static void release(void *context,ChVkTexture *image) {
    /* Entries live in a fixed allocation and cannot be evicted while pinned. */
    struct Entry *e=context;
    if(e->image!=image || !e->pins) abort();
    e->pins--;
}
static void pin(ChVkTextureCache *c,struct Entry *e,ChVkSampleImage *out) {
    e->pins++;e->used=++c->clock;e->frame=c->frame;
    c->hints[hint_bucket(&e->key,e->source)]=(uint32_t)(e-c->entries)+1;
    *out=(ChVkSampleImage){.width=e->key.width,.height=e->key.height,.gpu=e->image,
        .release=release,.release_context=e};
}
VkResult ch_vk_texture_cache_create(const ChVkTextureCacheConfig *config,ChVkTextureCache **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;
    if(!config || !config->physical || !config->device || !config->queue ||
       !config->guest_base || !config->guest_bytes || !config->decode || config->max_entries>4096)
        return VK_ERROR_INITIALIZATION_FAILED;
    ChVkTextureCache *c=calloc(1,sizeof *c);
    if(!c) return VK_ERROR_OUT_OF_HOST_MEMORY;
    c->config=*config;
    const char *trace=getenv("CHIHIRO_VULKAN_FRAME_COSTS");c->trace=trace && atoi(trace)!=0;
    if(!c->config.max_bytes) c->config.max_bytes=64u<<20;
    if(!c->config.max_entries) c->config.max_entries=CH_VK_TEXTURE_CACHE_DEFAULT_ENTRIES;
    c->entries=calloc(c->config.max_entries,sizeof *c->entries);
    if(!c->entries) {free(c);return VK_ERROR_OUT_OF_HOST_MEMORY;}
    *out=c;return VK_SUCCESS;
}
VkResult ch_vk_texture_cache_resolve(void *context,const Nv2aTexture *t,ChVkSampleImage *out) {
    ChVkTextureCache *c=context;
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    memset(out,0,sizeof *out);
    if(!c) return VK_ERROR_INITIALIZATION_FAILED;
    size_t span,palette;
    VkResult r=ch_vk_texture_storage_span(t,&span,&palette);
    if(r!=VK_SUCCESS) return r;
    if(t->offset>c->config.guest_bytes || span>c->config.guest_bytes-t->offset ||
       (palette && (t->palette>c->config.guest_bytes || palette>c->config.guest_bytes-t->palette)))
        return VK_ERROR_INITIALIZATION_FAILED;
    VkSamplerCreateInfo sampler;
    r=ch_vk_sampler_state(t,&sampler);if(r!=VK_SUCCESS) return r;
    struct Entry *reusable=NULL;
    struct Entry *hint=lookup_hint(c,t,NULL);
    if(hint && unchanged(c,hint,t)) {
        c->stats.hits++;c->stats.lookup_hint_hits++;pin(c,hint,out);return VK_SUCCESS;
    }
    if(hint && c->config.reuse_uploads && !hint->pins) reusable=hint;
    for(uint32_t index=c->buckets[hint_bucket(t,NULL)];index;) {
        struct Entry *e=&c->entries[index-1];
        index=e->lookup_next;
        if(e==hint) continue;
        c->stats.lookup_scan_entries++;
        if(e->image && !e->source && !memcmp(&e->key,t,sizeof *t)) {
            if(unchanged(c,e,t)) {c->stats.hits++;pin(c,e,out);return VK_SUCCESS;}
            if(c->config.reuse_uploads && !e->pins) reusable=e;
        }
    }
    c->stats.misses++;
    size_t pixels=0;
    uint32_t levels=ch_nv2a_texture_mip_levels(t,1);
    r=image_cost(c,t,&pixels);if(r!=VK_SUCCESS) return r;
    size_t staging=(size_t)t->width*t->height*4*(t->cube?6:1);
    if(span>c->config.max_bytes || palette>c->config.max_bytes-span ||
       pixels>c->config.max_bytes-span-palette) return VK_NOT_READY;
    int gpu_decode=c->config.gpu_decode && ch_vk_texture_can_decode(t);
    size_t cost=span+palette+pixels;
    // Encoded GPU staging is retained in addition to the existing CPU snapshot.
    if(gpu_decode) {if(span>c->config.max_bytes-cost) return VK_NOT_READY;cost+=span;}
    uint64_t start=c->trace?cache_us():0;
    struct Entry *slot=reusable?reusable:make_room(c,cost);if(!slot) return VK_NOT_READY;
    uint64_t room=c->trace?cache_us():0;
    unsigned char *raw=malloc(span+palette);
    uint32_t *argb=gpu_decode?NULL:malloc(staging);
    if(!raw || (!gpu_decode && !argb)) {free(raw);free(argb);return VK_ERROR_OUT_OF_HOST_MEMORY;}
    memcpy(raw,guest(c,t->offset),span);
    if(palette) memcpy(raw+span,guest(c,t->palette),palette);
    if(!gpu_decode && !ch_vk_decode_texture_faces(t,c->config.decode,argb)) {free(raw);free(argb);return VK_ERROR_FORMAT_NOT_SUPPORTED;}
    if(!gpu_decode) c->stats.cpu_decode_pixels+=(uint64_t)t->width*t->height*(t->cube?6:1);
    struct Entry snapshot={.raw=raw,.span=span,.palette=palette};
    if(!unchanged(c,&snapshot,t)) {free(raw);free(argb);return VK_NOT_READY;}
    uint64_t copied=c->trace?cache_us():0;
    if(reusable) {
        r=gpu_decode?ch_vk_texture_replace_encoded(reusable->image,c->config.pipeline_cache,t,raw,span,c->config.queued_uploads):
            ch_vk_texture_replace_argb(reusable->image,argb,staging/4,c->config.queued_uploads);
        free(argb);
        if(r!=VK_SUCCESS) {free(raw);drop(c,reusable);return r;}
        free(reusable->raw);reusable->raw=raw;reusable->observed_valid=0;
        c->stats.uploads++;c->stats.cpu_reuses++;c->stats.gpu_decodes+=gpu_decode;pin(c,reusable,out);
        trace_miss(c,t,start,room,copied,c->trace?cache_us():0,0,1);return VK_SUCCESS;
    }
    ChVkTexture *image=NULL;
    if(gpu_decode) r=ch_vk_texture_upload_encoded(c->config.physical,c->config.device,c->config.queue,c->config.family,
        c->config.pipeline_cache,t,raw,span,levels,c->config.queued_uploads,&image);
    else if(c->config.queued_uploads) r=ch_vk_texture_upload_queued(c->config.physical,c->config.device,c->config.queue,c->config.family,
        t->width,t->height,argb,levels,t->cube,&image);
    else if(t->cube) r=ch_vk_texture_upload_cube(c->config.physical,c->config.device,c->config.queue,c->config.family,t->width,argb,&image);
    else r=ch_vk_texture_upload_mips(c->config.physical,c->config.device,c->config.queue,c->config.family,
        t->width,t->height,argb,levels,&image);
    free(argb);
    if(r==VK_SUCCESS) r=ch_vk_texture_set_sampler(image,&sampler);
    if(r!=VK_SUCCESS) {ch_vk_texture_destroy(image);free(raw);return r;}
    uint64_t uploaded=c->trace?cache_us():0;
    *slot=(struct Entry){.key=*t,.image=image,.raw=raw,.span=span,.palette=palette,.cost=(size_t)cost};
    slot->raw_watch=ch_guest_write_range_create(c->config.guest_write_watch,guest(c,t->offset),span);
    if(palette) slot->palette_watch=ch_guest_write_range_create(c->config.guest_write_watch,guest(c,t->palette),palette);
    if(slot==c->entries+c->extent) c->extent++;
    index_entry(c,slot);
    c->stats.entries++;c->stats.bytes+=(size_t)cost;c->stats.uploads++;c->stats.gpu_decodes+=gpu_decode;pin(c,slot,out);
    trace_miss(c,t,start,room,copied,uploaded,0,0);return VK_SUCCESS;
}
VkResult ch_vk_texture_cache_resolve_target(ChVkTextureCache *c,const Nv2aTexture *t,
    ChVkTarget *source,uint64_t serial,ChVkSampleImage *out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    memset(out,0,sizeof *out);
    if(!c || !t || !source || !serial || t->cube || ch_vk_target_device(source)!=c->config.device)
        return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t width,height;ch_vk_target_size(source,&width,&height);
    if(width!=t->width || height!=t->height) return VK_ERROR_INITIALIZATION_FAILED;
    VkSamplerCreateInfo sampler;VkResult r=ch_vk_sampler_state(t,&sampler);
    if(r!=VK_SUCCESS) return r;
    struct Entry *reusable=NULL;
    struct Entry *hint=lookup_hint(c,t,source);
    if(hint && hint->serial==serial) {
        c->stats.hits++;c->stats.gpu_hits++;c->stats.lookup_hint_hits++;pin(c,hint,out);return VK_SUCCESS;
    }
    if(hint && !hint->pins) reusable=hint;
    for(uint32_t index=c->buckets[hint_bucket(t,source)];index;) {
        struct Entry *e=&c->entries[index-1];
        index=e->lookup_next;
        if(e==hint) continue;
        c->stats.lookup_scan_entries++;
        if(e->image && e->source==source && !memcmp(&e->key,t,sizeof *t)) {
            if(e->serial==serial) {c->stats.hits++;c->stats.gpu_hits++;pin(c,e,out);return VK_SUCCESS;}
            if(!e->pins) reusable=e;
        }
    }
    if(reusable) {
        r=ch_vk_target_refresh_texture(source,reusable->image);if(r!=VK_SUCCESS) return r;
        reusable->serial=serial;c->stats.misses++;c->stats.gpu_copies++;c->stats.gpu_reuses++;
        pin(c,reusable,out);return VK_SUCCESS;
    }
    c->stats.misses++;size_t cost;
    r=image_cost(c,t,&cost);if(r!=VK_SUCCESS) return r;
    uint64_t start=c->trace?cache_us():0;
    struct Entry *slot=make_room(c,cost);if(!slot) return VK_NOT_READY;
    ChVkTexture *image=NULL;
    uint64_t room=c->trace?cache_us():0;
    r=ch_vk_target_snapshot_texture(source,ch_nv2a_texture_mip_levels(t,1),&image);
    if(r==VK_SUCCESS) r=ch_vk_texture_set_sampler(image,&sampler);
    if(r!=VK_SUCCESS) {ch_vk_texture_destroy(image);return r;}
    *slot=(struct Entry){.key=*t,.image=image,.cost=cost,.source=source,.serial=serial};
    if(slot==c->entries+c->extent) c->extent++;
    index_entry(c,slot);
    c->stats.entries++;c->stats.bytes+=cost;c->stats.gpu_copies++;pin(c,slot,out);
    trace_miss(c,t,start,room,room,c->trace?cache_us():0,1,0);return VK_SUCCESS;
}
void ch_vk_texture_cache_advance_frame(ChVkTextureCache *c) {
    if(!c) return;
    c->frame++;
    // Amortize cold destruction before a large first-use allocation. Keep a
    // quarter of the existing byte budget available when cold images allow
    // it; never trim a pinned or current/previous-frame image, and retire at
    // most one image per completed frame. The hard allocation cap is unchanged.
    size_t retained=c->config.max_bytes-c->config.max_bytes/4;
    if(c->config.size_aware_eviction && c->stats.bytes>retained) {
        struct Entry *e=oldest(c,c->stats.bytes-retained);
        if(e && c->frame-e->frame>=2) drop(c,e);
    }
}
void ch_vk_texture_cache_stats(const ChVkTextureCache *c,ChVkTextureCacheStats *out) {
    if(out) *out=c?c->stats:(ChVkTextureCacheStats){0};
    if(out && c) out->capacity=c->config.max_entries;
}
void ch_vk_texture_cache_destroy(ChVkTextureCache *c) {
    if(!c) return;
    for(uint32_t i=0;i<c->extent;i++) drop(c,&c->entries[i]);
    free(c->entries);free(c);
}
