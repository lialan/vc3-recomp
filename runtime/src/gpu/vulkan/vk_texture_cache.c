#include "vk_texture_cache.h"
#include "../d3d/d3d8_swizzle.h"
#include <stdlib.h>
#include <string.h>
struct Entry {
    Nv2aTexture key;
    ChVkTexture *image;
    unsigned char *raw;
    size_t span,palette,cost;
    uint64_t used;
    unsigned pins;
};
struct ChVkTextureCache {
    ChVkTextureCacheConfig config;
    struct Entry *entries;
    uint64_t clock;
    ChVkTextureCacheStats stats;
};
static VkResult storage(const Nv2aTexture *t,size_t *span,size_t *palette) {
    if(!t || !t->width || !t->height || (uint64_t)t->width*t->height>SIZE_MAX/4)
        return VK_ERROR_INITIALIZATION_FAILED;
    if(t->cube) return VK_NOT_READY;
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
        } else n=(uint64_t)(t->height-1)*t->pitch+(uint64_t)t->width*bpp;
    }
    if(n>SIZE_MAX) return VK_ERROR_INITIALIZATION_FAILED;
    *span=(size_t)n;*palette=t->color==0x0b?1024:0;
    if(*palette && !t->palette) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    return VK_SUCCESS;
}
static const unsigned char *guest(const ChVkTextureCache *c,uint32_t va) {
    return (const unsigned char *)c->config.guest_base+va;
}
static int unchanged(const ChVkTextureCache *c,const struct Entry *e,const Nv2aTexture *t) {
    return !memcmp(e->raw,guest(c,t->offset),e->span) &&
        (!e->palette || !memcmp(e->raw+e->span,guest(c,t->palette),e->palette));
}
static void drop(ChVkTextureCache *c,struct Entry *e) {
    if(!e->image) return;
    if(e->pins) abort();
    ch_vk_texture_destroy(e->image);free(e->raw);
    c->stats.bytes-=e->cost;c->stats.evictions++;
    memset(e,0,sizeof *e);
}
static struct Entry *oldest(ChVkTextureCache *c) {
    struct Entry *found=NULL;
    for(uint32_t i=0;i<c->config.max_entries;i++) {
        struct Entry *e=&c->entries[i];
        if(e->image && !e->pins && (!found || e->used<found->used)) found=e;
    }
    return found;
}
static void release(void *context,ChVkTexture *image) {
    ChVkTextureCache *c=context;
    for(uint32_t i=0;i<c->config.max_entries;i++) if(c->entries[i].image==image) {
        if(!c->entries[i].pins) abort();
        c->entries[i].pins--;return;
    }
    abort();
}
static void pin(ChVkTextureCache *c,struct Entry *e,ChVkSampleImage *out) {
    e->pins++;e->used=++c->clock;
    *out=(ChVkSampleImage){.width=e->key.width,.height=e->key.height,.gpu=e->image,
        .release=release,.release_context=c};
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
    if(!c->config.max_bytes) c->config.max_bytes=64u<<20;
    if(!c->config.max_entries) c->config.max_entries=64;
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
    VkResult r=storage(t,&span,&palette);
    if(r!=VK_SUCCESS) return r;
    if(t->offset>c->config.guest_bytes || span>c->config.guest_bytes-t->offset ||
       (palette && (t->palette>c->config.guest_bytes || palette>c->config.guest_bytes-t->palette)))
        return VK_ERROR_INITIALIZATION_FAILED;
    VkSamplerCreateInfo sampler;
    r=ch_vk_sampler_state(t,&sampler);if(r!=VK_SUCCESS) return r;
    struct Entry *slot=NULL;
    for(uint32_t i=0;i<c->config.max_entries;i++) {
        struct Entry *e=&c->entries[i];
        if(!e->image) {if(!slot) slot=e;continue;}
        if(!memcmp(&e->key,t,sizeof *t) && unchanged(c,e,t)) {
            c->stats.hits++;pin(c,e,out);return VK_SUCCESS;
        }
    }
    c->stats.misses++;
    size_t pixels=(size_t)t->width*t->height*4;
    if(span>c->config.max_bytes || palette>c->config.max_bytes-span ||
       pixels>c->config.max_bytes-span-palette) return VK_NOT_READY;
    size_t cost=span+palette+pixels;
    while(c->stats.bytes>c->config.max_bytes-cost || !slot) {
        struct Entry *e=oldest(c);
        if(!e) return VK_NOT_READY;
        drop(c,e);if(!slot) slot=e;
    }
    unsigned char *raw=malloc(span+palette);
    uint32_t *argb=malloc((size_t)t->width*t->height*4);
    if(!raw || !argb) {free(raw);free(argb);return VK_ERROR_OUT_OF_HOST_MEMORY;}
    memcpy(raw,guest(c,t->offset),span);
    if(palette) memcpy(raw+span,guest(c,t->palette),palette);
    if(!c->config.decode(t,argb)) {free(raw);free(argb);return VK_ERROR_FORMAT_NOT_SUPPORTED;}
    struct Entry snapshot={.raw=raw,.span=span,.palette=palette};
    if(!unchanged(c,&snapshot,t)) {free(raw);free(argb);return VK_NOT_READY;}
    ChVkTexture *image=NULL;
    r=ch_vk_texture_upload(c->config.physical,c->config.device,c->config.queue,c->config.family,
        t->width,t->height,argb,&image);
    free(argb);
    if(r==VK_SUCCESS) r=ch_vk_texture_set_sampler(image,&sampler);
    if(r!=VK_SUCCESS) {ch_vk_texture_destroy(image);free(raw);return r;}
    *slot=(struct Entry){.key=*t,.image=image,.raw=raw,.span=span,.palette=palette,.cost=(size_t)cost};
    c->stats.bytes+=(size_t)cost;c->stats.uploads++;pin(c,slot,out);return VK_SUCCESS;
}
void ch_vk_texture_cache_stats(const ChVkTextureCache *c,ChVkTextureCacheStats *out) {
    if(out) *out=c?c->stats:(ChVkTextureCacheStats){0};
}
void ch_vk_texture_cache_destroy(ChVkTextureCache *c) {
    if(!c) return;
    for(uint32_t i=0;i<c->config.max_entries;i++) drop(c,&c->entries[i]);
    free(c->entries);free(c);
}
