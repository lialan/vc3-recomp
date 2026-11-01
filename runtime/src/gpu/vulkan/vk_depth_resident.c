#include "vk_depth_resident.h"
#include "vk_depth_guest.h"
#include "vk_guest_layout.h"
#include "vk_texture_cache.h"
#include <stdlib.h>
#include <string.h>
struct ChVkDepthResident {
    void *guest;size_t bytes;
    ChVkTarget *target;
    Nv2aSurface surface;Nv2aRenderState state;ChVkGuestLayout layout;
    float *depth;uint8_t *stencil,*raw,*edits;
    int dirty;
    ChVkDepthResidentStats stats;
};
static uint8_t *storage(ChVkDepthResident *d) {return (uint8_t *)d->guest+d->state.zeta_va;}
static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static VkResult observe(ChVkDepthResident *d) {
    if(!d->target || !memcmp(storage(d),d->raw,(size_t)d->layout.span)) return VK_SUCCESS;
    memcpy(d->edits,storage(d),(size_t)d->layout.span);
    int changed=0;
    for(uint32_t y=0;y<d->surface.height && !changed;y++) for(uint32_t x=0;x<d->surface.width;x++) {
        size_t at=(size_t)ch_vk_guest_offset(&d->layout,x,y);
        if(word(d->raw+at)!=word(d->edits+at)) {changed=1;break;}
    }
    if(changed) {
        size_t count=(size_t)d->surface.width*d->surface.height;
        VkResult r=ch_vk_target_read_depth_stencil(d->target,d->depth,d->stencil,count);
        if(r!=VK_SUCCESS) return r;
        for(uint32_t y=0;y<d->surface.height;y++) for(uint32_t x=0;x<d->surface.width;x++) {
            size_t at=(size_t)ch_vk_guest_offset(&d->layout,x,y),i=(size_t)y*d->surface.width+x;
            uint32_t before=word(d->raw+at),after=word(d->edits+at);
            /* Updating only stencil must not resurrect stale CPU depth, and
             * updating only depth must retain stencil written by the GPU. */
            if((before^after)&0xffffff00u) d->depth[i]=(float)((double)(after>>8)/16777215.0);
            if((before^after)&255) d->stencil[i]=(uint8_t)after;
        }
        r=ch_vk_target_write_depth_stencil(d->target,d->depth,d->stencil,count);
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
    d->guest=guest;d->bytes=bytes;*out=d;return VK_SUCCESS;
}
VkResult ch_vk_depth_resident_flush(ChVkDepthResident *d) {
    if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=observe(d);
    if(r!=VK_SUCCESS || !d->target || !d->dirty) return r;
    r=ch_vk_target_read_depth_stencil(d->target,d->depth,d->stencil,(size_t)d->surface.width*d->surface.height);
    if(r==VK_SUCCESS) r=ch_vk_guest_depth_store(&d->surface,&d->state,d->guest,d->bytes,d->depth,d->stencil);
    if(r!=VK_SUCCESS) return r;
    memcpy(d->raw,storage(d),(size_t)d->layout.span);
    d->dirty=0;d->stats.writebacks++;return VK_SUCCESS;
}
VkResult ch_vk_depth_resident_release(ChVkDepthResident *d) {
    VkResult r=ch_vk_depth_resident_flush(d);
    if(r!=VK_SUCCESS) return r;
    d->target=NULL;free(d->depth);free(d->stencil);free(d->raw);free(d->edits);
    d->depth=NULL;d->stencil=d->raw=d->edits=NULL;return VK_SUCCESS;
}
VkResult ch_vk_depth_resident_bind(ChVkDepthResident *d,ChVkTarget *target,
    const Nv2aSurface *surface,const Nv2aRenderState *state) {
    if(!d || !target || !surface || !state || !state->zeta_va || ((state->surface_format>>4)&15)!=2)
        return VK_ERROR_INITIALIZATION_FAILED;
    ChVkGuestLayout layout;
    VkResult r=ch_vk_guest_layout(state->surface_format,surface->width,surface->height,state->zeta_pitch,4,&layout);
    if(r!=VK_SUCCESS) return r;
    if(state->zeta_va>d->bytes || layout.span>d->bytes-state->zeta_va ||
       (uint64_t)surface->width*surface->height>SIZE_MAX/4) return VK_ERROR_INITIALIZATION_FAILED;
    d->stats.binds++;
    if(d->target==target && d->state.zeta_va==state->zeta_va &&
       d->surface.width==surface->width && d->surface.height==surface->height &&
       d->state.zeta_pitch==state->zeta_pitch &&
       (d->state.surface_format&0xffffff00u)==(state->surface_format&0xffffff00u)) return observe(d);
    r=ch_vk_depth_resident_release(d);
    if(r!=VK_SUCCESS) return r;
    size_t count=(size_t)surface->width*surface->height;
    d->depth=malloc(count*4);d->stencil=malloc(count);
    d->raw=malloc((size_t)layout.span);d->edits=malloc((size_t)layout.span);
    if(!d->depth || !d->stencil || !d->raw || !d->edits) return VK_ERROR_OUT_OF_HOST_MEMORY;
    r=ch_vk_guest_depth_load(surface,state,d->guest,d->bytes,d->depth,d->stencil);
    if(r==VK_SUCCESS) r=ch_vk_target_write_depth_stencil(target,d->depth,d->stencil,count);
    if(r!=VK_SUCCESS) return r;
    d->surface=*surface;d->state=*state;d->layout=layout;d->target=target;
    memcpy(d->raw,storage(d),(size_t)layout.span);d->stats.imports++;return VK_SUCCESS;
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
    free(d->depth);free(d->stencil);free(d->raw);free(d->edits);free(d);
}
