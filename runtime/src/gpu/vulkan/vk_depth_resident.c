#include "vk_depth_resident.h"
#include "vk_depth_guest.h"
#include "vk_depth_quant_math.h"
#include "vk_guest_layout.h"
#include "vk_texture_cache.h"
#include <stdlib.h>
#include <string.h>
enum { CLEAN_DEPTH_SLOTS=4, CLEAN_DEPTH_BYTES=16*1024*1024 };
struct CleanDepth {ChVkTarget *target;Nv2aSurface surface;Nv2aRenderState state;uint8_t *raw;size_t span;};
typedef struct DepthImage DepthImage;
struct DepthImage {
    void *guest;size_t bytes;
    ChVkTarget *target;
    ChGuestWriteWatch *watch;ChGuestWriteRange *write_range;
    Nv2aSurface surface;Nv2aRenderState state;ChVkGuestLayout layout;
    float *depth;uint8_t *stencil,*raw,*edits;uint32_t *words;
    int dirty,gpu_copies,packed,gpu_layout,morton;
    int reuse_clean;unsigned clean_next;size_t clean_bytes;struct CleanDepth clean[CLEAN_DEPTH_SLOTS];
    ChVkDepthResidentStats stats;
};
static void drop_clean(DepthImage *d,unsigned i) {
    d->clean_bytes-=d->clean[i].span;free(d->clean[i].raw);memset(&d->clean[i],0,sizeof d->clean[i]);
}
static void invalidate_clean_target(DepthImage *d,ChVkTarget *target) {
    for(unsigned i=0;i<CLEAN_DEPTH_SLOTS;i++) if(d->clean[i].target==target) drop_clean(d,i);
}
/* Only published images are cached. RAM remains authoritative while dormant;
 * exact byte comparison on reuse catches edits and overlapping publications. */
static void save_clean(DepthImage *d) {
    if(!d->reuse_clean || !d->target || d->dirty || d->layout.span>CLEAN_DEPTH_BYTES) return;
    invalidate_clean_target(d,d->target);
    while(d->clean_bytes+d->layout.span>CLEAN_DEPTH_BYTES) {
        drop_clean(d,d->clean_next);d->clean_next=(d->clean_next+1)%CLEAN_DEPTH_SLOTS;
    }
    unsigned i=d->clean_next;drop_clean(d,i);d->clean_next=(i+1)%CLEAN_DEPTH_SLOTS;
    d->clean[i]=(struct CleanDepth){d->target,d->surface,d->state,d->raw,(size_t)d->layout.span};
    d->clean_bytes+=d->layout.span;d->raw=NULL;
}
static uint8_t *take_clean(DepthImage *d,ChVkTarget *target,const Nv2aSurface *s,const Nv2aRenderState *rs,size_t span) {
    uint8_t *raw=NULL;
    for(unsigned i=0;i<CLEAN_DEPTH_SLOTS;i++) if(d->clean[i].target==target) {
        struct CleanDepth *c=&d->clean[i];
        if(c->surface.width==s->width && c->surface.height==s->height && c->state.zeta_va==rs->zeta_va &&
           c->state.zeta_pitch==rs->zeta_pitch && c->state.surface_format==rs->surface_format && c->span==span &&
           !memcmp(c->raw,(uint8_t *)d->guest+rs->zeta_va,span)) {raw=c->raw;c->raw=NULL;}
        drop_clean(d,i);break;
    }
    return raw;
}
static void single_reuse_clean(DepthImage *d,int enabled) {
    if(!d || d->target) return;
    d->reuse_clean=!!enabled;
    for(unsigned i=0;i<CLEAN_DEPTH_SLOTS;i++) drop_clean(d,i);
}
static uint8_t *storage(DepthImage *d) {return (uint8_t *)d->guest+d->state.zeta_va;}
static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static VkResult read_target(DepthImage *d) {
    size_t count=(size_t)d->surface.width*d->surface.height;
    return d->packed?ch_vk_target_read_z24s8(d->target,d->words,count):
        ch_vk_target_read_depth_stencil(d->target,d->depth,d->stencil,count);
}
static VkResult write_target(DepthImage *d) {
    size_t count=(size_t)d->surface.width*d->surface.height;
    return d->packed?ch_vk_target_write_z24s8(d->target,d->words,count):
        ch_vk_target_write_depth_stencil(d->target,d->depth,d->stencil,count);
}
static VkResult observe(DepthImage *d) {
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
                size_t index=d->morton?at/4:i;
                d->words[index]=(d->words[index]&~mask)|(after&mask);
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
static VkResult single_create(void *guest,size_t bytes,DepthImage **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;
    if(!guest || !bytes) return VK_ERROR_INITIALIZATION_FAILED;
    DepthImage *d=calloc(1,sizeof *d);
    if(!d) return VK_ERROR_OUT_OF_HOST_MEMORY;
    d->guest=guest;d->bytes=bytes;d->gpu_copies=d->packed=1;*out=d;return VK_SUCCESS;
}
static void single_gpu_layout(DepthImage *d,int enabled) {if(d && !d->target) d->gpu_layout=!!enabled;}
static void single_gpu_copies(DepthImage *d,int enabled) {if(d) d->gpu_copies=!!enabled;}
static void single_write_watch(DepthImage *d,ChGuestWriteWatch *watch) {
    if(d && !d->target) d->watch=watch;
}
static VkResult single_packed(DepthImage *d,int enabled) {
    if(!d || d->target || d->words || d->depth || d->stencil) return VK_ERROR_INITIALIZATION_FAILED;
    d->packed=!!enabled;return VK_SUCCESS;
}
static ChVkTarget *single_dirty_target(const DepthImage *d) {
    return d && d->dirty?d->target:NULL;
}
static VkResult publish_depth(DepthImage *d,VkResult r) {
    ch_guest_write_watch_begin(d->watch,storage(d),(size_t)d->layout.span);
    if(r==VK_SUCCESS && d->morton) memcpy(storage(d),d->words,(size_t)d->surface.width*d->surface.height*4);
    else if(r==VK_SUCCESS) r=d->packed?ch_vk_guest_z24s8_store(&d->surface,&d->state,d->guest,d->bytes,d->words):
        ch_vk_guest_depth_store(&d->surface,&d->state,d->guest,d->bytes,d->depth,d->stencil);
    if(r==VK_SUCCESS) {
        uint64_t pixels=(uint64_t)d->surface.width*d->surface.height;
        if(d->layout.swizzled && !d->morton) d->stats.cpu_layout_pixels+=pixels;
        if(!d->packed) d->stats.cpu_float_pixels+=pixels;
        if(d->packed && d->layout.span==pixels*4 && (d->morton ||
           (!d->layout.swizzled && d->layout.pitch==(uint64_t)d->surface.width*4))) {
            /* The packed readback already is the exact guest baseline. Its
             * request is retired; keep it and recycle the previous baseline. */
            uint8_t *scratch=d->raw;d->raw=(uint8_t *)d->words;d->words=(uint32_t *)scratch;
            d->stats.snapshot_copy_bytes_saved+=d->layout.span;
        } else memcpy(d->raw,storage(d),(size_t)d->layout.span);
    }
    ch_guest_write_watch_end(d->watch,storage(d),(size_t)d->layout.span);
    if(r!=VK_SUCCESS) return r;
    d->dirty=0;d->stats.writebacks++;return VK_SUCCESS;
}
static VkResult flush_depth(DepthImage *d,void *rgba,size_t bytes) {
    if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    /* With no GPU writes, guest RAM already is authoritative. The next bind
     * observes CPU edits before GPU use; release may discard the idle image. */
    if(!d->target || !d->dirty) return VK_SUCCESS;
    VkResult r=observe(d);
    if(r!=VK_SUCCESS) return r;
    r=rgba?ch_vk_target_read_depth_color(d->target,d->packed?(void *)d->words:(void *)d->depth,
        d->stencil,(size_t)d->surface.width*d->surface.height,d->packed,rgba,bytes):read_target(d);
    return publish_depth(d,r);
}
static VkResult single_flush(DepthImage *d) {return flush_depth(d,NULL,0);}
static VkResult single_flush_color(DepthImage *d,void *rgba,size_t bytes) {
    if(!d || !d->target || !d->dirty || !rgba) return VK_ERROR_INITIALIZATION_FAILED;
    return flush_depth(d,rgba,bytes);
}
static VkResult release_current(DepthImage *d,int retain) {
    VkResult r=single_flush(d);
    if(r!=VK_SUCCESS) return r;
    if(retain) save_clean(d);
    d->target=NULL;free(d->depth);free(d->stencil);free(d->words);free(d->raw);free(d->edits);
    ch_guest_write_range_destroy(d->write_range);d->write_range=NULL;
    d->depth=NULL;d->words=NULL;d->stencil=d->raw=d->edits=NULL;return VK_SUCCESS;
}
static VkResult single_release(DepthImage *d) {
    VkResult r=release_current(d,0);
    if(r==VK_SUCCESS) for(unsigned i=0;i<CLEAN_DEPTH_SLOTS;i++) drop_clean(d,i);
    return r;
}
static VkResult bind_depth(DepthImage *d,ChVkTarget *target,
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
        invalidate_clean_target(d,target);
        /* The following full clear supersedes every logical pixel, including
         * intervening CPU edits. Snapshot RAM so those old edits cannot be
         * misidentified as new writes after the clear. Padding is untouched. */
        r=ch_vk_target_depth_morton(target,d->morton);
        if(r!=VK_SUCCESS) return r;
        memcpy(d->raw,(uint8_t *)d->guest+state->zeta_va,(size_t)layout.span);
        d->target=target;d->surface=*surface;d->state=*state;d->layout=layout;
        d->dirty=0;d->stats.clear_skips++;return VK_SUCCESS;
    }
    if(same && d->target==target) return observe(d);
    if(same && d->gpu_copies) {
        invalidate_clean_target(d,target);
        /* RAM remains the last published snapshot. Merge any intervening CPU
         * component edits before transferring the exact GPU depth/stencil.
         * Completion/sample/layout boundaries still publish through flush. */
        r=ch_vk_target_depth_morton(target,d->morton);
        if(r==VK_SUCCESS) r=observe(d);
        if(r==VK_SUCCESS) r=ch_vk_target_copy_depth_stencil(d->target,target);
        if(r!=VK_SUCCESS) return r;
        d->target=target;d->surface=*surface;d->state=*state;d->layout=layout;
        d->stats.gpu_copies++;return VK_SUCCESS;
    }
    if(d->target) {
        uint64_t old_begin=d->state.zeta_va,old_end=old_begin+d->layout.span;
        uint64_t new_begin=state->zeta_va,new_end=new_begin+layout.span;
        unsigned kind=(d->target!=target?2u:0u)|((old_begin<new_end && new_begin<old_end)?1u:0u);
        d->stats.switches[kind]++;
        if(d->dirty) d->stats.dirty_switches[kind]++;
    }
    r=release_current(d,d->target!=target);
    if(r!=VK_SUCCESS) return r;
    uint32_t wx=(state->surface_format>>16)&255,hy=state->surface_format>>24;
    d->morton=d->gpu_layout && d->packed && layout.swizzled && wx<=15 && hy<=15 &&
        surface->width==(1u<<wx) && surface->height==(1u<<hy);
    r=ch_vk_target_depth_morton(target,d->morton);
    if(r!=VK_SUCCESS) return r;
    size_t count=(size_t)surface->width*surface->height;
    if(d->packed) d->words=malloc(count*4);
    else {d->depth=malloc(count*4);d->stencil=malloc(count);}
    uint8_t *cached=take_clean(d,target,surface,state,(size_t)layout.span);
    int reused=cached && !overwrite;
    d->raw=cached?cached:malloc((size_t)layout.span);d->edits=malloc((size_t)layout.span);
    if((d->packed?!d->words:(!d->depth || !d->stencil)) || !d->raw || !d->edits) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if(!overwrite && !reused) {
        if(d->morton) memcpy(d->words,(uint8_t *)d->guest+state->zeta_va,count*4);
        else r=d->packed?ch_vk_guest_z24s8_load(surface,state,d->guest,d->bytes,d->words):
            ch_vk_guest_depth_load(surface,state,d->guest,d->bytes,d->depth,d->stencil);
        if(r==VK_SUCCESS) r=d->packed?ch_vk_target_write_z24s8(target,d->words,count):
            ch_vk_target_write_depth_stencil(target,d->depth,d->stencil,count);
        if(r!=VK_SUCCESS) return r;
    }
    d->surface=*surface;d->state=*state;d->layout=layout;d->target=target;
    memcpy(d->raw,storage(d),(size_t)layout.span);
    d->write_range=ch_guest_write_range_create(d->watch,storage(d),(size_t)layout.span);
    if(overwrite) d->stats.clear_skips++;else if(reused) d->stats.clean_reuses++;else {
        d->stats.imports++;
        if(layout.swizzled && !d->morton) d->stats.cpu_layout_pixels+=count;
        if(!d->packed) d->stats.cpu_float_pixels+=count;
    }
    return VK_SUCCESS;
}
static VkResult single_dirty(DepthImage *d,ChVkTarget *target) {
    if(!d || !target || d->target!=target) return VK_ERROR_INITIALIZATION_FAILED;
    d->dirty=1;return VK_SUCCESS;
}
static int overlaps(uint64_t a,uint64_t n,uint64_t b,uint64_t m) {
    return n && m && (a<=b?b-a<n:a-b<m);
}
static VkResult single_check_color(const DepthImage *d,uint32_t va,size_t span) {
    if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    return d->target && overlaps(va,span,d->state.zeta_va,d->layout.span)?
        VK_ERROR_FEATURE_NOT_PRESENT:VK_SUCCESS;
}
static VkResult single_sample(DepthImage *d,const Nv2aTexture *tx) {
    if(!d || !tx) return VK_ERROR_INITIALIZATION_FAILED;
    if(!d->target || !d->dirty) return VK_SUCCESS;
    size_t span,palette;
    VkResult r=ch_vk_texture_storage_span(tx,&span,&palette);
    /* Unknown decoder storage conservatively publishes pending depth. */
    if(r!=VK_SUCCESS || overlaps(tx->offset,span,d->state.zeta_va,d->layout.span) ||
       (palette && overlaps(tx->palette,palette,d->state.zeta_va,d->layout.span)))
        return single_flush(d);
    return VK_SUCCESS;
}
static void single_stats(const DepthImage *d,ChVkDepthResidentStats *out) {
    if(out) *out=d?d->stats:(ChVkDepthResidentStats){0};
}
static void single_destroy(DepthImage *d) {
    if(!d) return;
    for(unsigned i=0;i<CLEAN_DEPTH_SLOTS;i++) drop_clean(d,i);
    ch_guest_write_range_destroy(d->write_range);
    free(d->depth);free(d->stencil);free(d->words);free(d->raw);free(d->edits);free(d);
}

/* A bounded set of disjoint dirty owners. All guest completion boundaries still
 * publish every image. Overlapping consumers publish before reinterpretation. */
struct ChVkDepthResident {DepthImage *image[4];unsigned current;int multiple;uint64_t clock,used[4];};
VkResult ch_vk_depth_resident_create(void *guest,size_t bytes,ChVkDepthResident **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;ChVkDepthResident *d=calloc(1,sizeof *d);
    if(!d) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult r=single_create(guest,bytes,&d->image[0]);
    if(r!=VK_SUCCESS) {free(d);return r;}*out=d;return VK_SUCCESS;
}
void ch_vk_depth_resident_multiple(ChVkDepthResident *d,int enabled) {
    if(!d) return;
    for(unsigned i=0;i<4;i++) if(d->image[i] && d->image[i]->target) return;
    d->multiple=!!enabled;d->current=0;
    if(enabled) for(unsigned i=0;i<4;i++) if(d->image[i]) single_reuse_clean(d->image[i],0);
}
void ch_vk_depth_resident_gpu_layout(ChVkDepthResident *d,int e) {if(d) for(unsigned i=0;i<4;i++) if(d->image[i]) single_gpu_layout(d->image[i],e);}
void ch_vk_depth_resident_gpu_copies(ChVkDepthResident *d,int e) {if(d) for(unsigned i=0;i<4;i++) if(d->image[i]) single_gpu_copies(d->image[i],e);}
void ch_vk_depth_resident_write_watch(ChVkDepthResident *d,ChGuestWriteWatch *w) {if(d) for(unsigned i=0;i<4;i++) if(d->image[i]) single_write_watch(d->image[i],w);}
void ch_vk_depth_resident_reuse_clean(ChVkDepthResident *d,int e) {if(d && !d->multiple) for(unsigned i=0;i<4;i++) if(d->image[i]) single_reuse_clean(d->image[i],e);}
VkResult ch_vk_depth_resident_packed(ChVkDepthResident *d,int e) {
    if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    for(unsigned i=0;i<4;i++) if(d->image[i] && d->image[i]->target) return VK_ERROR_INITIALIZATION_FAILED;
    for(unsigned i=0;i<4;i++) if(d->image[i]) {VkResult r=single_packed(d->image[i],e);if(r!=VK_SUCCESS)return r;}
    return VK_SUCCESS;
}
static VkResult pool_bind(ChVkDepthResident *d,ChVkTarget *target,const Nv2aSurface *s,const Nv2aRenderState *rs,int clear) {
    if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    if(!d->multiple) return bind_depth(d->image[0],target,s,rs,clear);
    if(!target || !s || !rs || !rs->zeta_va || ((rs->surface_format>>4)&15)!=2) return VK_ERROR_INITIALIZATION_FAILED;
    DepthImage *current=d->image[d->current];
    /* Most draws retain the same depth view. No other owner can overlap a
     * previously validated view, so avoid rescanning the pool per batch. */
    if(current->target==target && current->surface.width==s->width && current->surface.height==s->height &&
       current->state.zeta_va==rs->zeta_va && current->state.zeta_pitch==rs->zeta_pitch &&
       current->state.surface_format==rs->surface_format)
        return bind_depth(current,target,s,rs,clear);
    ChVkGuestLayout layout;VkResult r=ch_vk_guest_layout(rs->surface_format,s->width,s->height,rs->zeta_pitch,4,&layout);
    if(r!=VK_SUCCESS) return r;
    DepthImage *base=d->image[0];
    if(rs->zeta_va>base->bytes || layout.span>base->bytes-rs->zeta_va) return VK_ERROR_INITIALIZATION_FAILED;
    unsigned slot=4;
    for(unsigned i=0;i<4;i++) if(d->image[i] && d->image[i]->target==target) {slot=i;break;}
    /* Preserve the existing exact shared-zeta GPU-copy path. Move the source
     * owner to the destination image instead of publishing/reimporting it. */
    if(!clear) for(unsigned i=0;i<4;i++) {
        DepthImage *source=d->image[i];
        if(i==slot || !source || !source->target || !source->gpu_copies) continue;
        if(source->surface.width==s->width && source->surface.height==s->height &&
           source->state.zeta_va==rs->zeta_va && source->state.zeta_pitch==rs->zeta_pitch &&
           source->state.surface_format==rs->surface_format) {
            if(slot<4) {r=single_release(d->image[slot]);if(r!=VK_SUCCESS)return r;}
            r=bind_depth(source,target,s,rs,0);
            if(r==VK_SUCCESS) {d->current=i;d->used[i]=++d->clock;}
            return r;
        }
    }
    /* A single physical target cannot carry two interpretations. Aliasing guest
     * storage also needs the previous result published before the new import. */
    for(unsigned i=0;i<4;i++) if(i!=slot && d->image[i] && d->image[i]->target &&
        overlaps(rs->zeta_va,layout.span,d->image[i]->state.zeta_va,d->image[i]->layout.span)) {
        DepthImage *old=d->image[i];
        /* Full depth+stencil clear supersedes an exact logical view, including
         * CPU edits. Publishing it first only adds a readback of dead pixels. */
        if(clear && old->surface.width==s->width && old->surface.height==s->height &&
           old->state.zeta_va==rs->zeta_va && old->state.zeta_pitch==rs->zeta_pitch &&
           old->state.surface_format==rs->surface_format) old->dirty=0;
        r=single_release(old);if(r!=VK_SUCCESS)return r;
    }
    /* Bound retained staging to 16 MiB of guest spans (at most 64 MiB
     * including packed/float scratch). A larger individual target falls back
     * to single-owner behavior without retaining extra images. */
    uint64_t spans=layout.span;
    for(unsigned i=0;i<4;i++) if(i!=slot && d->image[i] && d->image[i]->target) spans+=d->image[i]->layout.span;
    for(unsigned i=0;i<4 && spans>CLEAN_DEPTH_BYTES;i++) if(i!=slot && d->image[i] && d->image[i]->target) {
        spans-=d->image[i]->layout.span;r=single_release(d->image[i]);if(r!=VK_SUCCESS)return r;
    }
    if(slot==4) for(unsigned i=0;i<4;i++) if(!d->image[i] || !d->image[i]->target) {slot=i;break;}
    if(slot==4) {
        slot=0;for(unsigned i=1;i<4;i++) if(d->used[i]<d->used[slot]) slot=i;
        r=single_release(d->image[slot]);if(r!=VK_SUCCESS)return r;
    }
    if(!d->image[slot]) {
        r=single_create(base->guest,base->bytes,&d->image[slot]);if(r!=VK_SUCCESS)return r;
        DepthImage *image=d->image[slot];image->watch=base->watch;image->packed=base->packed;
        image->gpu_layout=base->gpu_layout;image->gpu_copies=base->gpu_copies;
        /* Clean caches are not combined with dirty pooling: a dormant clean
         * entry in another owner could otherwise refer to a modified target. */
    }
    r=bind_depth(d->image[slot],target,s,rs,clear);
    if(r==VK_SUCCESS) {d->current=slot;d->used[slot]=++d->clock;}
    return r;
}
VkResult ch_vk_depth_resident_bind(ChVkDepthResident *d,ChVkTarget *t,const Nv2aSurface *s,const Nv2aRenderState *r) {return pool_bind(d,t,s,r,0);}
VkResult ch_vk_depth_resident_bind_clear(ChVkDepthResident *d,ChVkTarget *t,const Nv2aSurface *s,const Nv2aRenderState *r) {return pool_bind(d,t,s,r,1);}
VkResult ch_vk_depth_resident_dirty(ChVkDepthResident *d,ChVkTarget *t) {return d?single_dirty(d->image[d->current],t):VK_ERROR_INITIALIZATION_FAILED;}
ChVkTarget *ch_vk_depth_resident_dirty_target(const ChVkDepthResident *d) {return d?single_dirty_target(d->image[d->current]):NULL;}
VkResult ch_vk_depth_resident_flush(ChVkDepthResident *d) {
    if(!d)return VK_ERROR_INITIALIZATION_FAILED;
    for(unsigned i=0;i<4;i++) if(d->image[i]) {VkResult r=single_flush(d->image[i]);if(r!=VK_SUCCESS)return r;}
    return VK_SUCCESS;
}
VkResult ch_vk_depth_resident_flush_many(ChVkDepthResident *d,const ChVkDepthColorRead *colors,size_t count) {
    if(!d || (count && !colors)) return VK_ERROR_INITIALIZATION_FAILED;
    for(size_t j=0;j<count;j++) {
        if(!colors[j].rgba || !ch_vk_depth_resident_target_dirty(d,colors[j].target)) return VK_ERROR_INITIALIZATION_FAILED;
        for(size_t k=0;k<j;k++) if(colors[k].target==colors[j].target) return VK_ERROR_INITIALIZATION_FAILED;
    }
    /* Retained owners have disjoint guest depth ranges. Observe all edits
     * before submitting: observation may itself import/finish a target. Color
     * buffers are snapshots only; caller publishes them after CPU coherence. */
    for(unsigned i=0;i<4;i++) if(d->image[i] && d->image[i]->target && d->image[i]->dirty) {
        VkResult r=observe(d->image[i]);if(r!=VK_SUCCESS) return r;
    }
    unsigned started=0;VkResult result=VK_SUCCESS;
    for(unsigned i=0;i<4;i++) {
        DepthImage *image=d->image[i];
        if(!image || !image->target || !image->dirty) continue;
        void *rgba=NULL;size_t bytes=0;
        for(size_t j=0;j<count;j++) if(colors[j].target==image->target) {
            rgba=colors[j].rgba;bytes=colors[j].bytes;break;
        }
        result=ch_vk_target_read_depth_begin(image->target,image->packed?(void *)image->words:(void *)image->depth,
            image->stencil,(size_t)image->surface.width*image->surface.height,image->packed,rgba,bytes);
        if(result!=VK_SUCCESS) break;
        started|=1u<<i;image->stats.queued_readbacks++;
    }
    /* Drain every submitted request even if a subsequent begin failed. No
     * caller can release staging or acknowledge guest completion early. */
    for(unsigned i=0;i<4;i++) if(started&(1u<<i)) {
        DepthImage *image=d->image[i];VkResult r=ch_vk_target_read_depth_end(image->target);
        if(r==VK_SUCCESS) r=publish_depth(image,r);
        if(result==VK_SUCCESS) result=r;
    }
    return result;
}
int ch_vk_depth_resident_target_dirty(const ChVkDepthResident *d,ChVkTarget *target) {
    if(d) for(unsigned i=0;i<4;i++) if(d->image[i] && d->image[i]->target==target) return d->image[i]->dirty;
    return 0;
}
VkResult ch_vk_depth_resident_flush_target_color(ChVkDepthResident *d,ChVkTarget *target,void *rgba,size_t bytes) {
    if(d) for(unsigned i=0;i<4;i++) if(d->image[i] && d->image[i]->target==target)
        return single_flush_color(d->image[i],rgba,bytes);
    return VK_ERROR_INITIALIZATION_FAILED;
}
VkResult ch_vk_depth_resident_flush_color(ChVkDepthResident *d,void *rgba,size_t bytes) {
    if(!d)return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=single_flush_color(d->image[d->current],rgba,bytes);if(r!=VK_SUCCESS)return r;
    return ch_vk_depth_resident_flush(d);
}
VkResult ch_vk_depth_resident_release(ChVkDepthResident *d) {
    if(!d)return VK_ERROR_INITIALIZATION_FAILED;
    for(unsigned i=0;i<4;i++) if(d->image[i]) {VkResult r=single_release(d->image[i]);if(r!=VK_SUCCESS)return r;}
    return VK_SUCCESS;
}
VkResult ch_vk_depth_resident_sample(ChVkDepthResident *d,const Nv2aTexture *tx) {
    if(!d)return VK_ERROR_INITIALIZATION_FAILED;
    for(unsigned i=0;i<4;i++) if(d->image[i]) {VkResult r=single_sample(d->image[i],tx);if(r!=VK_SUCCESS)return r;}
    return VK_SUCCESS;
}
VkResult ch_vk_depth_resident_prepare_color(ChVkDepthResident *d,uint32_t va,size_t span) {
    if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    /* Keep the existing active color/depth alias rejection. Inactive pooled
     * owners, unlike the single-owner path, may still retain reused storage;
     * publish and retire them before color becomes authoritative there. */
    VkResult r=single_check_color(d->image[d->current],va,span);
    if(r!=VK_SUCCESS) return r;
    if(d->multiple) for(unsigned i=0;i<4;i++) if(i!=d->current && d->image[i] &&
        single_check_color(d->image[i],va,span)!=VK_SUCCESS) {
        r=single_release(d->image[i]);if(r!=VK_SUCCESS)return r;
    }
    return VK_SUCCESS;
}
VkResult ch_vk_depth_resident_check_color(const ChVkDepthResident *d,uint32_t va,size_t span) {
    if(!d)return VK_ERROR_INITIALIZATION_FAILED;
    for(unsigned i=0;i<4;i++) if(d->image[i]) {VkResult r=single_check_color(d->image[i],va,span);if(r!=VK_SUCCESS)return r;}
    return VK_SUCCESS;
}
void ch_vk_depth_resident_stats(const ChVkDepthResident *d,ChVkDepthResidentStats *out) {
    if(!out)return;memset(out,0,sizeof *out);if(!d)return;
    for(unsigned i=0;i<4;i++) if(d->image[i]) {
        ChVkDepthResidentStats s;single_stats(d->image[i],&s);
#define SUM(field) out->field+=s.field
        SUM(imports);SUM(writebacks);SUM(cpu_merges);SUM(binds);SUM(gpu_copies);SUM(clear_skips);SUM(clean_reuses);SUM(cpu_layout_pixels);SUM(cpu_float_pixels);SUM(queued_readbacks);SUM(snapshot_copy_bytes_saved);
#undef SUM
        for(unsigned j=0;j<4;j++){out->switches[j]+=s.switches[j];out->dirty_switches[j]+=s.dirty_switches[j];}
    }
}
void ch_vk_depth_resident_destroy(ChVkDepthResident *d) {if(d){for(unsigned i=0;i<4;i++)single_destroy(d->image[i]);free(d);}}
