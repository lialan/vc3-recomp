#include "vk_depth_resident.h"
#include "vk_depth_guest.h"
#include "vk_depth_quant_math.h"
#include "vk_guest_layout.h"
#include "vk_texture_cache.h"
#include <stdlib.h>
#include <string.h>
struct ChVkDepthResident {
    void *guest;size_t bytes;
    ChVkTarget *target;
    ChGuestWriteWatch *watch;ChGuestWriteRange *write_range;
    Nv2aSurface surface;Nv2aRenderState state;ChVkGuestLayout layout;
    float *depth;uint8_t *stencil,*raw,*edits;uint32_t *words;
    int dirty,gpu_copies,packed;
    ChVkDepthResidentStats stats;
};
static uint8_t *storage(ChVkDepthResident *d) {return (uint8_t *)d->guest+d->state.zeta_va;}
static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static VkResult read_target(ChVkDepthResident *d) {
    size_t count=(size_t)d->surface.width*d->surface.height;
    return d->packed?ch_vk_target_read_z24s8(d->target,d->words,count):
        ch_vk_target_read_depth_stencil(d->target,d->depth,d->stencil,count);
}
static VkResult write_target(ChVkDepthResident *d) {
    size_t count=(size_t)d->surface.width*d->surface.height;
    return d->packed?ch_vk_target_write_z24s8(d->target,d->words,count):
        ch_vk_target_write_depth_stencil(d->target,d->depth,d->stencil,count);
}
static VkResult observe(ChVkDepthResident *d) {
    if(d->target && d->write_range && !ch_guest_write_range_changed(d->write_range)) return VK_SUCCESS;
    if(!d->target || !memcmp(storage(d),d->raw,(size_t)d->layout.span)) return VK_SUCCESS;
    memcpy(d->edits,storage(d),(size_t)d->layout.span);
    int changed=0;
    for(uint32_t y=0;y<d->surface.height && !changed;y++) for(uint32_t x=0;x<d->surface.width;x++) {
        size_t at=(size_t)ch_vk_guest_offset(&d->layout,x,y);
        if(word(d->raw+at)!=word(d->edits+at)) {changed=1;break;}
    }
    if(changed) {
        VkResult r=read_target(d);
        if(r!=VK_SUCCESS) return r;
        for(uint32_t y=0;y<d->surface.height;y++) for(uint32_t x=0;x<d->surface.width;x++) {
            size_t at=(size_t)ch_vk_guest_offset(&d->layout,x,y),i=(size_t)y*d->surface.width+x;
            uint32_t before=word(d->raw+at),after=word(d->edits+at);
            /* Updating only stencil must not resurrect stale CPU depth, and
             * updating only depth must retain stencil written by the GPU. */
            if(d->packed) {
                uint32_t mask=(((before^after)&0xffffff00u)?0xffffff00u:0)|(((before^after)&255)?255u:0);
                d->words[i]=(d->words[i]&~mask)|(after&mask);
            } else {
                if((before^after)&0xffffff00u) d->depth[i]=ch_vk_depth_unpack(after>>8);
                if((before^after)&255) d->stencil[i]=(uint8_t)after;
            }
        }
        r=write_target(d);
        if(r!=VK_SUCCESS) return r;
        d->stats.cpu_merges++;
    }
    memcpy(d->raw,d->edits,(size_t)d->layout.span);
    return VK_SUCCESS;
}
VkResult ch_vk_depth_resident_create(void *guest,size_t bytes,ChVkDepthResident **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;
    if(!guest || !bytes) return VK_ERROR_INITIALIZATION_FAILED;
    ChVkDepthResident *d=calloc(1,sizeof *d);
    if(!d) return VK_ERROR_OUT_OF_HOST_MEMORY;
    d->guest=guest;d->bytes=bytes;d->gpu_copies=d->packed=1;*out=d;return VK_SUCCESS;
}
void ch_vk_depth_resident_gpu_copies(ChVkDepthResident *d,int enabled) {if(d) d->gpu_copies=!!enabled;}
void ch_vk_depth_resident_write_watch(ChVkDepthResident *d,ChGuestWriteWatch *watch) {
    if(d && !d->target) d->watch=watch;
}
VkResult ch_vk_depth_resident_packed(ChVkDepthResident *d,int enabled) {
    if(!d || d->target || d->words || d->depth || d->stencil) return VK_ERROR_INITIALIZATION_FAILED;
    d->packed=!!enabled;return VK_SUCCESS;
}
ChVkTarget *ch_vk_depth_resident_dirty_target(const ChVkDepthResident *d) {
    return d && d->dirty?d->target:NULL;
}
static VkResult flush_depth(ChVkDepthResident *d,void *rgba,size_t bytes) {
    if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    /* With no GPU writes, guest RAM already is authoritative. The next bind
     * observes CPU edits before GPU use; release may discard the idle image. */
    if(!d->target || !d->dirty) return VK_SUCCESS;
    VkResult r=observe(d);
    if(r!=VK_SUCCESS) return r;
    r=rgba?ch_vk_target_read_depth_color(d->target,d->packed?(void *)d->words:(void *)d->depth,
        d->stencil,(size_t)d->surface.width*d->surface.height,d->packed,rgba,bytes):read_target(d);
    ch_guest_write_watch_begin(d->watch,storage(d),(size_t)d->layout.span);
    if(r==VK_SUCCESS) r=d->packed?ch_vk_guest_z24s8_store(&d->surface,&d->state,d->guest,d->bytes,d->words):
        ch_vk_guest_depth_store(&d->surface,&d->state,d->guest,d->bytes,d->depth,d->stencil);
    if(r==VK_SUCCESS) memcpy(d->raw,storage(d),(size_t)d->layout.span);
    ch_guest_write_watch_end(d->watch,storage(d),(size_t)d->layout.span);
    if(r!=VK_SUCCESS) return r;
    d->dirty=0;d->stats.writebacks++;return VK_SUCCESS;
}
VkResult ch_vk_depth_resident_flush(ChVkDepthResident *d) {return flush_depth(d,NULL,0);}
VkResult ch_vk_depth_resident_flush_color(ChVkDepthResident *d,void *rgba,size_t bytes) {
    if(!d || !d->target || !d->dirty || !rgba) return VK_ERROR_INITIALIZATION_FAILED;
    return flush_depth(d,rgba,bytes);
}
VkResult ch_vk_depth_resident_release(ChVkDepthResident *d) {
    VkResult r=ch_vk_depth_resident_flush(d);
    if(r!=VK_SUCCESS) return r;
    d->target=NULL;free(d->depth);free(d->stencil);free(d->words);free(d->raw);free(d->edits);
    ch_guest_write_range_destroy(d->write_range);d->write_range=NULL;
    d->depth=NULL;d->words=NULL;d->stencil=d->raw=d->edits=NULL;return VK_SUCCESS;
}
static VkResult bind_depth(ChVkDepthResident *d,ChVkTarget *target,
    const Nv2aSurface *surface,const Nv2aRenderState *state,int overwrite) {
    if(!d || !target || !surface || !state || !state->zeta_va || ((state->surface_format>>4)&15)!=2)
        return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t width,height;ch_vk_target_size(target,&width,&height);
    if(width!=surface->width || height!=surface->height) return VK_ERROR_INITIALIZATION_FAILED;
    ChVkGuestLayout layout;
    VkResult r=ch_vk_guest_layout(state->surface_format,surface->width,surface->height,state->zeta_pitch,4,&layout);
    if(r!=VK_SUCCESS) return r;
    if(state->zeta_va>d->bytes || layout.span>d->bytes-state->zeta_va ||
       (uint64_t)surface->width*surface->height>SIZE_MAX/4) return VK_ERROR_INITIALIZATION_FAILED;
    d->stats.binds++;
    int same=d->target && d->state.zeta_va==state->zeta_va &&
       d->surface.width==surface->width && d->surface.height==surface->height &&
       d->state.zeta_pitch==state->zeta_pitch &&
        (d->state.surface_format&0xffffff00u)==(state->surface_format&0xffffff00u);
    if(same && overwrite) {
        /* The following full clear supersedes every logical pixel, including
         * intervening CPU edits. Snapshot RAM so those old edits cannot be
         * misidentified as new writes after the clear. Padding is untouched. */
        memcpy(d->raw,(uint8_t *)d->guest+state->zeta_va,(size_t)layout.span);
        d->target=target;d->surface=*surface;d->state=*state;d->layout=layout;
        d->dirty=0;d->stats.clear_skips++;return VK_SUCCESS;
    }
    if(same && d->target==target) return observe(d);
    if(same && d->gpu_copies) {
        /* RAM remains the last published snapshot. Merge any intervening CPU
         * component edits before transferring the exact GPU depth/stencil.
         * Completion/sample/layout boundaries still publish through flush. */
        r=observe(d);
        if(r==VK_SUCCESS) r=ch_vk_target_copy_depth_stencil(d->target,target);
        if(r!=VK_SUCCESS) return r;
        d->target=target;d->surface=*surface;d->state=*state;d->layout=layout;
        d->stats.gpu_copies++;return VK_SUCCESS;
    }
    r=ch_vk_depth_resident_release(d);
    if(r!=VK_SUCCESS) return r;
    size_t count=(size_t)surface->width*surface->height;
    if(d->packed) d->words=malloc(count*4);
    else {d->depth=malloc(count*4);d->stencil=malloc(count);}
    d->raw=malloc((size_t)layout.span);d->edits=malloc((size_t)layout.span);
    if((d->packed?!d->words:(!d->depth || !d->stencil)) || !d->raw || !d->edits) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if(!overwrite) {
        r=d->packed?ch_vk_guest_z24s8_load(surface,state,d->guest,d->bytes,d->words):
            ch_vk_guest_depth_load(surface,state,d->guest,d->bytes,d->depth,d->stencil);
        if(r==VK_SUCCESS) r=d->packed?ch_vk_target_write_z24s8(target,d->words,count):
            ch_vk_target_write_depth_stencil(target,d->depth,d->stencil,count);
        if(r!=VK_SUCCESS) return r;
    }
    d->surface=*surface;d->state=*state;d->layout=layout;d->target=target;
    memcpy(d->raw,storage(d),(size_t)layout.span);
    d->write_range=ch_guest_write_range_create(d->watch,storage(d),(size_t)layout.span);
    if(overwrite) d->stats.clear_skips++;else d->stats.imports++;
    return VK_SUCCESS;
}
VkResult ch_vk_depth_resident_bind(ChVkDepthResident *d,ChVkTarget *t,const Nv2aSurface *s,const Nv2aRenderState *r) {
    return bind_depth(d,t,s,r,0);
}
VkResult ch_vk_depth_resident_bind_clear(ChVkDepthResident *d,ChVkTarget *t,const Nv2aSurface *s,const Nv2aRenderState *r) {
    return bind_depth(d,t,s,r,1);
}
VkResult ch_vk_depth_resident_dirty(ChVkDepthResident *d,ChVkTarget *target) {
    if(!d || !target || d->target!=target) return VK_ERROR_INITIALIZATION_FAILED;
    d->dirty=1;return VK_SUCCESS;
}
static int overlaps(uint64_t a,uint64_t n,uint64_t b,uint64_t m) {
    return n && m && (a<=b?b-a<n:a-b<m);
}
VkResult ch_vk_depth_resident_check_color(const ChVkDepthResident *d,uint32_t va,size_t span) {
    if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    return d->target && overlaps(va,span,d->state.zeta_va,d->layout.span)?
        VK_ERROR_FEATURE_NOT_PRESENT:VK_SUCCESS;
}
VkResult ch_vk_depth_resident_sample(ChVkDepthResident *d,const Nv2aTexture *tx) {
    if(!d || !tx) return VK_ERROR_INITIALIZATION_FAILED;
    if(!d->target || !d->dirty) return VK_SUCCESS;
    size_t span,palette;
    VkResult r=ch_vk_texture_storage_span(tx,&span,&palette);
    /* Unknown decoder storage conservatively publishes pending depth. */
    if(r!=VK_SUCCESS || overlaps(tx->offset,span,d->state.zeta_va,d->layout.span) ||
       (palette && overlaps(tx->palette,palette,d->state.zeta_va,d->layout.span)))
        return ch_vk_depth_resident_flush(d);
    return VK_SUCCESS;
}
void ch_vk_depth_resident_stats(const ChVkDepthResident *d,ChVkDepthResidentStats *out) {
    if(out) *out=d?d->stats:(ChVkDepthResidentStats){0};
}
void ch_vk_depth_resident_destroy(ChVkDepthResident *d) {
    if(!d) return;
    free(d->depth);free(d->stencil);free(d->words);free(d->raw);free(d->edits);free(d);
}
