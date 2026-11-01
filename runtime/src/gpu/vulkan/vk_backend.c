#include "vk_backend.h"
#include "vk_depth_guest.h"
#include "vk_guest_layout.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define MAX_SURFACES 32
struct Surface { Nv2aSurface guest; uint32_t zeta; Nv2aRenderState state; float *depth; uint8_t *stencil; ChVkTarget *target; unsigned char *rgba,*observed; int observed_valid; size_t bytes; };
struct ChVkBackend { ChVkBackendConfig host; struct Surface surfaces[MAX_SURFACES]; unsigned count; };
static ChVkBackend *active;
static void fatal(VkResult result,const char *operation) {
    if(active && active->host.fatal) active->host.fatal(result,operation);
    abort();
}
static struct Surface *surface(const Nv2aSurface *s,const Nv2aRenderState *rs) {
    if(!active || !s || !rs) fatal(VK_ERROR_INITIALIZATION_FAILED,"surface arguments");
    ChVkGuestLayout layout;
    VkResult layout_result=ch_vk_guest_layout(rs->surface_format,s->width,s->height,s->pitch,s->bytes_per_pixel,&layout);
    if(layout_result!=VK_SUCCESS) fatal(layout_result,"color surface layout");
    uint64_t span=layout.span;
    if(!s->width || !s->height ||
       (s->bytes_per_pixel!=2 && s->bytes_per_pixel!=4) ||
       s->color_va>active->host.guest_bytes || span>active->host.guest_bytes-s->color_va ||
       (uint64_t)s->width*s->height>SIZE_MAX/4)
        fatal(VK_ERROR_INITIALIZATION_FAILED,"surface bounds");
    for(unsigned i=0;i<active->count;i++) {
        struct Surface *f=&active->surfaces[i];
        if(f->guest.color_va==s->color_va) {
            if(f->guest.width!=s->width || f->guest.height!=s->height ||
               f->guest.pitch!=s->pitch || f->guest.bytes_per_pixel!=s->bytes_per_pixel) fatal(VK_ERROR_FEATURE_NOT_PRESENT,"surface reinterpretation");
            f->guest=*s; f->state=*rs; return f;
        }
        uint64_t old_start=f->guest.color_va;
        ChVkGuestLayout old_layout;
        VkResult old_result=ch_vk_guest_layout(f->state.surface_format,f->guest.width,f->guest.height,
            f->guest.pitch,f->guest.bytes_per_pixel,&old_layout);
        if(old_result!=VK_SUCCESS) fatal(old_result,"previous color surface layout");
        uint64_t old_end=old_start+old_layout.span;
        if(s->color_va<old_end && old_start<(uint64_t)s->color_va+span)
            fatal(VK_ERROR_FEATURE_NOT_PRESENT,"overlapping surfaces");
    }
    if(active->count==MAX_SURFACES) fatal(VK_ERROR_TOO_MANY_OBJECTS,"surface limit");
    struct Surface *f=&active->surfaces[active->count++];
    f->guest=*s; f->zeta=rs->zeta_va; f->state=*rs; f->bytes=(size_t)s->width*s->height*4;
    f->rgba=malloc(f->bytes);
    if(active->host.guest_color_changed) {
        f->observed=malloc(f->bytes);
        if(!f->observed) fatal(VK_ERROR_OUT_OF_HOST_MEMORY,"guest color observation allocation");
    }
    if(!f->rgba) fatal(VK_ERROR_OUT_OF_HOST_MEMORY,"surface readback allocation");
    VkResult r=ch_vk_target_create(active->host.physical,active->host.device,active->host.queue,
        active->host.family,s->width,s->height,&f->target);
    if(r!=VK_SUCCESS) fatal(r,"target creation");
    if(active->host.precise_queries) {
        r=ch_vk_target_enable_queries(f->target);
        if(r!=VK_SUCCESS) fatal(r,"query creation");
    }
    f->depth=malloc(f->bytes); f->stencil=malloc(f->bytes/4);
    if(!f->depth || !f->stencil) fatal(VK_ERROR_OUT_OF_HOST_MEMORY,"depth staging");
    const float blank[]={0,0,0,0};
    r=ch_vk_target_clear_read(f->target,blank,f->rgba,f->bytes);
    if(r!=VK_SUCCESS) fatal(r,"initial target state");
    return f;
}
static void store(struct Surface *f,VkResult result,const char *operation,int depth_dirty) {
    if(result!=VK_SUCCESS) fatal(result,operation);
    result=ch_vk_nv2a_store_color_state(&f->guest,&f->state,f->rgba,f->bytes,active->host.guest_base,active->host.guest_bytes);
    if(result!=VK_SUCCESS) fatal(result,"color writeback");
    if(f->observed) {
        /* RGB565 writeback quantizes: compare with canonical stored pixels,
         * not the higher precision GPU output, to avoid false CPU edits. */
        result=ch_vk_nv2a_load_color_state(&f->guest,&f->state,f->observed,f->bytes,active->host.guest_base,active->host.guest_bytes);
        if(result!=VK_SUCCESS) fatal(result,"color observation baseline");
        f->observed_valid=1;
    }
    if(f->state.zeta_va && depth_dirty) {
        result=ch_vk_target_read_depth_stencil(f->target,f->depth,f->stencil,f->bytes/4);
        if(result==VK_SUCCESS) result=ch_vk_guest_depth_store(&f->guest,&f->state,active->host.guest_base,active->host.guest_bytes,f->depth,f->stencil);
        if(result!=VK_SUCCESS) fatal(result,"depth writeback");
    }
}
static void observe_guest_colors(void) {
    if(!active->host.guest_color_changed) return;
    for(unsigned i=0;i<active->count;i++) {
        struct Surface *f=&active->surfaces[i];
        VkResult r=ch_vk_nv2a_load_color_state(&f->guest,&f->state,f->rgba,f->bytes,active->host.guest_base,active->host.guest_bytes);
        if(r!=VK_SUCCESS) fatal(r,"guest color observation");
        if(!f->observed_valid || memcmp(f->rgba,f->observed,f->bytes)) {
            r=active->host.guest_color_changed(active->host.render_context,&f->guest,f->rgba,f->bytes,!f->observed_valid);
            if(r!=VK_SUCCESS) fatal(r,"host CPU color coherence");
            memcpy(f->observed,f->rgba,f->bytes); f->observed_valid=1;
        }
    }
}
static void import_color(struct Surface *f) {
    VkResult r=ch_vk_nv2a_load_color_state(&f->guest,&f->state,f->rgba,f->bytes,active->host.guest_base,active->host.guest_bytes);
    if(r!=VK_SUCCESS) fatal(r,"guest color read");
    r=ch_vk_target_import_color(f->target,f->rgba,f->bytes);
    if(r!=VK_SUCCESS) fatal(r,"guest color import");
}
static void import_depth(struct Surface *f) {
    if(!f->state.zeta_va) return;
    VkResult r=ch_vk_guest_depth_load(&f->guest,&f->state,active->host.guest_base,active->host.guest_bytes,f->depth,f->stencil);
    if(r!=VK_SUCCESS) fprintf(stderr,"[Vulkan] depth surface=%08X zeta=%08X format=%08X pitch=%u size=%ux%u\n",
        f->guest.color_va,f->state.zeta_va,f->state.surface_format,f->state.zeta_pitch,f->guest.width,f->guest.height);
    if(r==VK_SUCCESS) r=ch_vk_target_write_depth_stencil(f->target,f->depth,f->stencil,f->bytes/4);
    if(r!=VK_SUCCESS) fatal(r,"guest depth import");
}
static void clear(const Nv2aSurface *s,const Nv2aRenderState *rs,uint32_t flags,uint32_t argb,uint32_t zs) {
    struct Surface *f=surface(s,rs);
    observe_guest_colors();
    if((flags & 0xf0)!=0xf0 || rs->clear_rect_set) import_color(f);
    if(flags & 3) import_depth(f);
    store(f,ch_vk_nv2a_clear_state_read(f->target,s,rs,flags,argb,zs,f->rgba,f->bytes),"clear",flags & 3);
    if(active->host.render_clear) {
        VkResult r=active->host.render_clear(active->host.render_context,s,rs,flags,argb,zs);
        if(r!=VK_SUCCESS) fatal(r,"host clear routing");
    }
}
static void draw(const Nv2aSurface *s,const Nv2aBatch *b) {
    if(!b) fatal(VK_ERROR_INITIALIZATION_FAILED,"batch arguments");
    struct Surface *f=surface(s,b->state);
    observe_guest_colors();
    import_color(f);
    if(b->state->depth_test_enable || b->state->stencil_enable) import_depth(f);
    VkResult result=ch_vk_nv2a_draw_decoded_read(f->target,s,b,active->host.decode,f->rgba,f->bytes);
    if(result!=VK_SUCCESS) {
        fprintf(stderr,"[Vulkan] rejected draw surface=%08X size=%ux%u vertices=%u ext=%d rc=%d stages=%08X\n",
            s->color_va,s->width,s->height,b->count,b->ext!=NULL,b->rc!=NULL,b->rc?b->rc->stage_program:0);
        for(unsigned i=0;i<4;i++) {
            const Nv2aTexture *texture=b->stage[i]?b->stage[i]:(i==0?b->texture:NULL);
            if(texture) fprintf(stderr,"[Vulkan] texture%u address=%08X size=%ux%u format=%02X levels=%u filter=%08X address=%u/%u cube=%u\n",
                i,texture->offset,texture->width,texture->height,texture->color,texture->levels,texture->filter,
                texture->addr_u,texture->addr_v,texture->cube);
        }
    }
    store(f,result,"draw",(b->state->depth_test_enable && b->state->depth_write)
        || (b->state->stencil_enable && b->state->stencil_write_mask));
    if(active->host.render_draw) {
        VkResult r=active->host.render_draw(active->host.render_context,s,b);
        if(r!=VK_SUCCESS) fatal(r,"host draw routing");
    }
}
static void flip(void) {
    /* All guest work completed. Snapshot guest-authoritative pixels before
     * crossing the submission/display thread boundary. */
    if(!active) fatal(VK_ERROR_INITIALIZATION_FAILED,"flip without backend");
    observe_guest_colors();
    uint32_t presented=nv2a_backend_presented_va();
    if(active->host.present_rgba && presented) {
        struct Surface *f=NULL;
        for(unsigned i=0;i<active->count;i++) if(active->surfaces[i].guest.color_va==presented) { f=&active->surfaces[i]; break; }
        if(!f) fatal(VK_ERROR_FEATURE_NOT_PRESENT,"unknown presented surface");
        VkResult r=ch_vk_nv2a_load_color_state(&f->guest,&f->state,f->rgba,f->bytes,active->host.guest_base,active->host.guest_bytes);
        if(r==VK_SUCCESS) r=active->host.present_rgba(active->host.render_context,&f->guest,f->rgba,f->bytes);
        if(r!=VK_SUCCESS) fatal(r,"host RGBA frame handoff");
    }
    if(active->host.render_flip) {
        VkResult r=active->host.render_flip(active->host.render_context,presented);
        if(r!=VK_SUCCESS) fatal(r,"host frame presentation");
    }
}
static int observe_raw(const Nv2aSurface *s,const Nv2aBatch *b) {
    if(!active || !active->host.observe_raw) fatal(VK_ERROR_INITIALIZATION_FAILED,"raw observer state");
    VkResult r=active->host.observe_raw(active->host.render_context,s,b);
    if(r!=VK_SUCCESS) fatal(r,"raw metadata observation");
    return 0; /* Retain the executor's reference CPU transform path. */
}
static Nv2aBackend callbacks={.clear=clear,.draw=draw,.flip=flip};
const Nv2aBackend *ch_vk_backend_callbacks(void) { return &callbacks; }
VkResult ch_vk_backend_create(const ChVkBackendConfig *config,ChVkBackend **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;
    if(!config || !config->physical || !config->device || !config->queue ||
       !config->guest_base || !config->guest_bytes || !config->decode)
        return VK_ERROR_INITIALIZATION_FAILED;
    ChVkBackend *b=calloc(1,sizeof(*b));
    if(!b) return VK_ERROR_OUT_OF_HOST_MEMORY;
    b->host=*config; *out=b; return VK_SUCCESS;
}
VkResult ch_vk_backend_attach(ChVkBackend *b) {
    if(!b || active) return VK_ERROR_INITIALIZATION_FAILED;
    callbacks.draw_raw=b->host.observe_raw?observe_raw:NULL;
    active=b; nv2a_backend_register(&callbacks); return VK_SUCCESS;
}
void ch_vk_backend_destroy(ChVkBackend *b) {
    if(!b) return;
    if(active==b) { nv2a_backend_register(NULL); active=NULL; }
    for(unsigned i=0;i<b->count;i++) { ch_vk_target_destroy(b->surfaces[i].target); free(b->surfaces[i].rgba); free(b->surfaces[i].observed); free(b->surfaces[i].depth); free(b->surfaces[i].stencil); }
    free(b);
}
