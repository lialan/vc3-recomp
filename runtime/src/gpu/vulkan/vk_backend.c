#include "vk_backend.h"
#include "vk_depth_guest.h"
#include "vk_guest_layout.h"
#include "vk_texture_cache.h"
#include "vk_depth_resident.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#define MAX_SURFACES 32
struct Surface { Nv2aSurface guest; uint32_t zeta; Nv2aRenderState state; float *depth; uint8_t *stencil; ChVkTarget *target; unsigned char *rgba,*observed,*cpu,*raw; size_t raw_span; int observed_valid,resident,color_dirty,native_z24; size_t bytes; };
struct ChVkBackend { struct {int enabled;uint64_t start,n,surface,coherence,imports,draw,store;} profile; struct {uint64_t previous,us[120];unsigned count;} frame_profile; ChVkBackendConfig host; ChVkTextureCache *textures; ChVkDepthResident *zeta; uint64_t frames,raw_draws,raw_declined; struct Surface surfaces[MAX_SURFACES]; unsigned count; };
static ChVkBackend *active;
static void flush_color(struct Surface *);
static void observe_guest_colors(void);
static void observe_guest_color(struct Surface *);
static uint64_t now_us(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000u+t.tv_nsec/1000;
}
static void profile_draw(const uint64_t t[6]) {
    if(!active->profile.enabled) return;
    if(!active->profile.start) active->profile.start=t[0];
    active->profile.n++;
    active->profile.surface+=t[1]-t[0];active->profile.coherence+=t[2]-t[1];
    active->profile.imports+=t[3]-t[2];active->profile.draw+=t[4]-t[3];active->profile.store+=t[5]-t[4];
    if(t[5]-active->profile.start<1000000) return;
    fprintf(stderr,"[Vulkan backend] %llu batches: surface %.1fms CPU edits %.1fms imports %.1fms draw %.1fms writeback %.1fms\n",
        (unsigned long long)active->profile.n,active->profile.surface/1000.,active->profile.coherence/1000.,
        active->profile.imports/1000.,active->profile.draw/1000.,active->profile.store/1000.);
    active->profile.n=active->profile.surface=active->profile.coherence=active->profile.imports=0;
    active->profile.draw=active->profile.store=0;active->profile.start=t[5];
}
static int compare_us(const void *a,const void *b) {
    uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return (x>y)-(x<y);
}
static void profile_frame(void) {
    if(!active->profile.enabled) return;
    uint64_t now=now_us(),previous=active->frame_profile.previous;
    active->frame_profile.previous=now;
    if(!previous) return;
    active->frame_profile.us[active->frame_profile.count++]=now-previous;
    if(active->frame_profile.count<120) return;
    uint64_t sum=0;unsigned over20=0,over33=0;
    for(unsigned i=0;i<120;i++) {uint64_t us=active->frame_profile.us[i];sum+=us;over20+=us>20000;over33+=us>33333;}
    qsort(active->frame_profile.us,120,sizeof(uint64_t),compare_us);
    fprintf(stderr,"[Vulkan frame intervals] flips %llu..%llu: %.2f fps mean %.2fms p95 %.2fms p99 %.2fms max %.2fms >20ms=%u >33ms=%u (guest completions, not XR display)\n",
        (unsigned long long)(active->frames-120),(unsigned long long)active->frames,
        sum?120e6/sum:0,sum/120000.,active->frame_profile.us[113]/1000.,
        active->frame_profile.us[118]/1000.,active->frame_profile.us[119]/1000.,over20,over33);
    active->frame_profile.count=0;
}
static void fatal(VkResult result,const char *operation) {
    if(active && active->host.fatal) active->host.fatal(result,operation);
    abort();
}
static void release_surface(struct Surface *f) {
    ch_vk_target_destroy(f->target);free(f->rgba);free(f->observed);free(f->cpu);free(f->raw);
    free(f->depth);free(f->stencil);memset(f,0,sizeof *f);
}
static struct Surface *surface(const Nv2aSurface *s,const Nv2aRenderState *rs) {
    if(!active || !s || !rs) fatal(VK_ERROR_INITIALIZATION_FAILED,"surface arguments");
    ChVkGuestLayout layout;
    VkResult layout_result=ch_vk_guest_layout(rs->surface_format,s->width,s->height,s->pitch,s->bytes_per_pixel,&layout);
    if(layout_result!=VK_SUCCESS) {
        fprintf(stderr,"[Vulkan] rejected surface address=%08X size=%ux%u pitch=%u bpp=%u format=%08X aa=%u/%u\n",
            s->color_va,s->width,s->height,s->pitch,s->bytes_per_pixel,rs->surface_format,s->aa_sx,s->aa_sy);
        fatal(layout_result,"color surface layout");
    }
    if(active->zeta) {
        VkResult r=ch_vk_depth_resident_check_color(active->zeta,s->color_va,(size_t)layout.span);
        if(r!=VK_SUCCESS) fatal(r,"resident depth/color alias");
    }
    uint64_t span=layout.span;
    if(!s->width || !s->height ||
       (s->bytes_per_pixel!=2 && s->bytes_per_pixel!=4) ||
       s->color_va>active->host.guest_bytes || span>active->host.guest_bytes-s->color_va ||
       (uint64_t)s->width*s->height>SIZE_MAX/4)
        fatal(VK_ERROR_INITIALIZATION_FAILED,"surface bounds");
    struct Surface *f=NULL;
    for(unsigned i=0;i<active->count;i++) {
        struct Surface *previous=&active->surfaces[i];
        if(previous->guest.color_va==s->color_va) {f=previous;continue;}
        ChVkGuestLayout old_layout;
        VkResult old_result=ch_vk_guest_layout(previous->state.surface_format,previous->guest.width,previous->guest.height,
            previous->guest.pitch,previous->guest.bytes_per_pixel,&old_layout);
        if(old_result!=VK_SUCCESS) fatal(old_result,"previous color surface layout");
        uint64_t old_start=previous->guest.color_va,old_end=old_start+old_layout.span;
        if(s->color_va<old_end && old_start<(uint64_t)s->color_va+span)
            fatal(VK_ERROR_FEATURE_NOT_PRESENT,"overlapping surfaces");
    }
    if(f && f->guest.width==s->width && f->guest.height==s->height &&
       f->guest.pitch==s->pitch && f->guest.bytes_per_pixel==s->bytes_per_pixel) {
        if(f->resident && f->state.surface_format!=rs->surface_format) {
            observe_guest_color(f);flush_color(f);f->observed_valid=0;
        }
        if(f->resident && f->raw_span!=span) {
            void *raw=realloc(f->raw,(size_t)span);
            if(!raw) fatal(VK_ERROR_OUT_OF_HOST_MEMORY,"resident guest snapshot resize");
            f->raw=raw;f->raw_span=(size_t)span;
        }
        f->guest=*s;f->state=*rs;return f;
    }
    if(f) {
        fprintf(stderr,"[Vulkan] resizing surface %08X: %ux%u pitch=%u bpp=%u -> %ux%u pitch=%u bpp=%u\n",
            s->color_va,f->guest.width,f->guest.height,f->guest.pitch,f->guest.bytes_per_pixel,
            s->width,s->height,s->pitch,s->bytes_per_pixel);
        /* Clip extents and storage interpretation can change at the same VA.
         * Publish the old view before rebuilding, then seed the new view from
         * guest storage. This retains pixels outside a temporarily smaller
         * clip, CPU edits and shared depth without stretching old GPU pixels. */
        observe_guest_color(f);flush_color(f);
        if(active->zeta) {
            VkResult r=ch_vk_depth_resident_release(active->zeta);
            if(r!=VK_SUCCESS) fatal(r,"depth owner before target resize");
        }
        release_surface(f);
    } else {
        if(active->count==MAX_SURFACES) fatal(VK_ERROR_TOO_MANY_OBJECTS,"surface limit");
        f=&active->surfaces[active->count++];
    }
    f->guest=*s; f->zeta=rs->zeta_va; f->state=*rs; f->bytes=(size_t)s->width*s->height*4;
    f->rgba=malloc(f->bytes);
    f->resident=active->host.resident_color && s->bytes_per_pixel==4;
    if(active->host.guest_color_changed || f->resident) {
        f->observed=malloc(f->bytes);
        if(!f->observed) fatal(VK_ERROR_OUT_OF_HOST_MEMORY,"guest color observation allocation");
    }
    if(f->resident) {
        f->cpu=malloc(f->bytes); f->raw=malloc((size_t)span); f->raw_span=(size_t)span;
        if(!f->cpu || !f->raw) fatal(VK_ERROR_OUT_OF_HOST_MEMORY,"CPU color staging");
    }
    if(!f->rgba) fatal(VK_ERROR_OUT_OF_HOST_MEMORY,"surface readback allocation");
    VkResult r=VK_ERROR_FORMAT_NOT_SUPPORTED;
    if(active->zeta && ((rs->surface_format>>4)&15)==2) {
        r=ch_vk_target_create_depth(active->host.physical,active->host.device,active->host.queue,
            active->host.family,s->width,s->height,VK_FORMAT_D24_UNORM_S8_UINT,&f->target);
        if(r==VK_SUCCESS) f->native_z24=1;
    }
    if(r==VK_ERROR_FORMAT_NOT_SUPPORTED) r=ch_vk_target_create(active->host.physical,active->host.device,active->host.queue,
        active->host.family,s->width,s->height,&f->target);
    if(r!=VK_SUCCESS) fatal(r,"target creation");
    if(f->resident && active->host.batch_draws) {
        r=ch_vk_target_batching(f->target,1);
        if(r!=VK_SUCCESS) fatal(r,"resident command batch setup");
    }
    if(active->host.precise_queries) {
        r=ch_vk_target_enable_queries(f->target);
        if(r!=VK_SUCCESS) fatal(r,"query creation");
    }
    f->depth=malloc(f->bytes); f->stencil=malloc(f->bytes/4);
    if(!f->depth || !f->stencil) fatal(VK_ERROR_OUT_OF_HOST_MEMORY,"depth staging");
    const float blank[]={0,0,0,0};
    r=ch_vk_target_clear_read(f->target,blank,f->rgba,f->bytes);
    if(r!=VK_SUCCESS) fatal(r,"initial target state");
    ch_vk_target_defer_color_readback(f->target,f->resident);
    return f;
}
static void store(struct Surface *f,VkResult result,const char *operation,int depth_dirty) {
    if(result!=VK_SUCCESS) fatal(result,operation);
    if(f->resident) f->color_dirty=1;
    else {
        result=ch_vk_nv2a_store_color_state(&f->guest,&f->state,f->rgba,f->bytes,active->host.guest_base,active->host.guest_bytes);
        if(result!=VK_SUCCESS) fatal(result,"color writeback");
        if(f->observed) {
            /* RGB565 writeback quantizes: compare with canonical stored pixels,
             * not the higher precision GPU output, to avoid false CPU edits. */
            result=ch_vk_nv2a_load_color_state(&f->guest,&f->state,f->observed,f->bytes,active->host.guest_base,active->host.guest_bytes);
            if(result!=VK_SUCCESS) fatal(result,"color observation baseline");
            f->observed_valid=1;
        }
    }
    if(f->state.zeta_va && depth_dirty) {
        if(active->zeta && f->native_z24 && ((f->state.surface_format>>4)&15)==2) {
            result=ch_vk_depth_resident_dirty(active->zeta,f->target);
            if(result!=VK_SUCCESS) fatal(result,"resident depth ownership");
            return;
        }
        result=ch_vk_target_read_depth_stencil(f->target,f->depth,f->stencil,f->bytes/4);
        if(result==VK_SUCCESS) result=ch_vk_guest_depth_store(&f->guest,&f->state,active->host.guest_base,active->host.guest_bytes,f->depth,f->stencil);
        if(result!=VK_SUCCESS) fatal(result,"depth writeback");
    }
}
static void flush_color(struct Surface *f) {
    if(!f->resident || !f->color_dirty) return;
    VkResult r=ch_vk_target_read_color(f->target,f->rgba,f->bytes);
    if(r==VK_SUCCESS) r=ch_vk_nv2a_store_color_state(&f->guest,&f->state,f->rgba,f->bytes,
        active->host.guest_base,active->host.guest_bytes);
    if(r!=VK_SUCCESS) fatal(r,"resident color writeback");
    memcpy(f->observed,f->rgba,f->bytes);
    memcpy(f->raw,(const unsigned char *)active->host.guest_base+f->guest.color_va,f->raw_span);
    f->observed_valid=1; f->color_dirty=0;
}
static void observe_guest_color(struct Surface *f) {
    if(!f->observed) return;
    const unsigned char *raw=(const unsigned char *)active->host.guest_base+f->guest.color_va;
    /* Fast unchanged-storage check before Morton/pixel decoding. Padding
     * changes fall through once, then the pixel comparison ignores them. */
    if(f->resident && f->observed_valid && !memcmp(raw,f->raw,f->raw_span)) return;
    unsigned char *cpu=f->resident?f->cpu:f->rgba;
    VkResult r=ch_vk_nv2a_load_color_state(&f->guest,&f->state,cpu,f->bytes,active->host.guest_base,active->host.guest_bytes);
    if(r!=VK_SUCCESS) fatal(r,"guest color observation");
    if(!f->observed_valid || memcmp(cpu,f->observed,f->bytes)) {
        if(f->resident) {
            if(!f->observed_valid) memcpy(f->rgba,cpu,f->bytes);
            else {
                r=ch_vk_target_read_color(f->target,f->rgba,f->bytes);
                if(r!=VK_SUCCESS) fatal(r,"resident CPU edit merge read");
                /* A changed guest pixel replaces that pixel; preserve GPU
                 * content in pixels the CPU did not change. */
                for(size_t p=0;p<f->bytes;p+=4)
                    if(memcmp(cpu+p,f->observed+p,4)) memcpy(f->rgba+p,cpu+p,4);
            }
            r=ch_vk_target_import_color(f->target,f->rgba,f->bytes);
            if(r!=VK_SUCCESS) fatal(r,"resident CPU edit import");
        }
        if(active->host.guest_color_changed) {
            r=active->host.guest_color_changed(active->host.render_context,&f->guest,cpu,f->bytes,!f->observed_valid);
            if(r!=VK_SUCCESS) fatal(r,"host CPU color coherence");
        }
        memcpy(f->observed,cpu,f->bytes); f->observed_valid=1;
    }
    if(f->resident) memcpy(f->raw,raw,f->raw_span);
}
static void observe_guest_colors(void) {
    for(unsigned i=0;i<active->count;i++) observe_guest_color(&active->surfaces[i]);
}
static void observe_target_color(struct Surface *f) {
    /* Routing hooks observe all guest edits before publishing host packets.
     * Pure resident targets only need observation when read or written;
     * completion still reconciles every pending target. */
    if(active->host.guest_color_changed) observe_guest_colors();
    else observe_guest_color(f);
}
static void flush(void) {
    for(unsigned i=0;i<active->count;i++) {
        VkResult r=ch_vk_target_finish(active->surfaces[i].target);
        if(r!=VK_SUCCESS) fatal(r,"pending guest command completion");
    }
    observe_guest_colors();
    for(unsigned i=0;i<active->count;i++) flush_color(&active->surfaces[i]);
    if(active->zeta) {
        VkResult r=ch_vk_depth_resident_flush(active->zeta);
        if(r!=VK_SUCCESS) fatal(r,"resident depth completion");
    }
}
static void flush_sampled_colors(const Nv2aBatch *b) {
    if(!active->host.resident_color && !active->zeta) return;
    for(unsigned stage=0;stage<4;stage++) {
        const Nv2aTexture *tx=b->stage[stage]?b->stage[stage]:(stage==0?b->texture:NULL);
        if(!tx) continue;
        if(active->zeta) {
            VkResult r=ch_vk_depth_resident_sample(active->zeta,tx);
            if(r!=VK_SUCCESS) fatal(r,"sampled resident depth");
        }
        size_t span=0,palette=0;
        VkResult storage=ch_vk_texture_storage_span(tx,&span,&palette);
        for(unsigned i=0;i<active->count;i++) {
            struct Surface *f=&active->surfaces[i];
            ChVkGuestLayout layout;
            VkResult r=ch_vk_guest_layout(f->state.surface_format,f->guest.width,f->guest.height,
                f->guest.pitch,f->guest.bytes_per_pixel,&layout);
            if(r!=VK_SUCCESS) fatal(r,"sampled target layout");
            uint64_t color=f->guest.color_va;
            int overlap=storage!=VK_SUCCESS ||
                ((uint64_t)tx->offset<=color?color-tx->offset<span:(uint64_t)tx->offset-color<layout.span) ||
                (palette && ((uint64_t)tx->palette<=color?color-tx->palette<palette:(uint64_t)tx->palette-color<layout.span));
            if(overlap) {
                observe_guest_color(f);
                flush_color(f);
            }
        }
    }
}
static void import_color(struct Surface *f) {
    if(f->resident) return; /* CPU changes already merged by observation. */
    VkResult r=ch_vk_nv2a_load_color_state(&f->guest,&f->state,f->rgba,f->bytes,active->host.guest_base,active->host.guest_bytes);
    if(r!=VK_SUCCESS) fatal(r,"guest color read");
    r=ch_vk_target_import_color(f->target,f->rgba,f->bytes);
    if(r!=VK_SUCCESS) fatal(r,"guest color import");
}
static void import_depth(struct Surface *f,int overwrite) {
    if(!f->state.zeta_va) return;
    if(active->zeta) {
        ChVkGuestLayout depth_layout;
        uint32_t format=(f->state.surface_format>>4)&15;
        VkResult r=ch_vk_guest_layout(f->state.surface_format,f->guest.width,f->guest.height,
            f->state.zeta_pitch,format==1?2:4,&depth_layout);
        if(r!=VK_SUCCESS) fatal(r,"resident depth layout");
        /* Cross color/depth reinterpretation needs a unified image owner.
         * Fail explicitly rather than letting either resident view go stale. */
        for(unsigned i=0;i<active->count;i++) {
            struct Surface *c=&active->surfaces[i];ChVkGuestLayout color_layout;
            r=ch_vk_guest_layout(c->state.surface_format,c->guest.width,c->guest.height,
                c->guest.pitch,c->guest.bytes_per_pixel,&color_layout);
            if(r!=VK_SUCCESS) fatal(r,"depth alias color layout");
            if((uint64_t)c->guest.color_va<f->state.zeta_va+depth_layout.span &&
               (uint64_t)f->state.zeta_va<c->guest.color_va+color_layout.span)
                fatal(VK_ERROR_FEATURE_NOT_PRESENT,"color/depth storage alias");
        }
        if(f->native_z24 && format==2) {
            r=overwrite?ch_vk_depth_resident_bind_clear(active->zeta,f->target,&f->guest,&f->state):
                ch_vk_depth_resident_bind(active->zeta,f->target,&f->guest,&f->state);
            if(r!=VK_SUCCESS) fatal(r,"resident depth bind");
            return;
        }
        r=ch_vk_depth_resident_release(active->zeta);
        if(r!=VK_SUCCESS) fatal(r,"resident depth strict transition");
    }
    VkResult r=ch_vk_guest_depth_load(&f->guest,&f->state,active->host.guest_base,active->host.guest_bytes,f->depth,f->stencil);
    if(r!=VK_SUCCESS) fprintf(stderr,"[Vulkan] depth surface=%08X zeta=%08X format=%08X pitch=%u size=%ux%u\n",
        f->guest.color_va,f->state.zeta_va,f->state.surface_format,f->state.zeta_pitch,f->guest.width,f->guest.height);
    if(r==VK_SUCCESS) r=ch_vk_target_write_depth_stencil(f->target,f->depth,f->stencil,f->bytes/4);
    if(r!=VK_SUCCESS) fatal(r,"guest depth import");
}
static void clear(const Nv2aSurface *s,const Nv2aRenderState *rs,uint32_t flags,uint32_t argb,uint32_t zs) {
    struct Surface *f=surface(s,rs);
    observe_target_color(f);
    if((flags & 0xf0)!=0xf0 || rs->clear_rect_set) import_color(f);
    if(flags & 3) import_depth(f,!active->host.strict_depth_clears && ch_vk_nv2a_clear_depth_full(s,rs,flags));
    store(f,ch_vk_nv2a_clear_state_read(f->target,s,rs,flags,argb,zs,f->rgba,f->bytes),"clear",flags & 3);
    if(active->host.render_clear) {
        VkResult r=active->host.render_clear(active->host.render_context,s,rs,flags,argb,zs);
        if(r!=VK_SUCCESS) fatal(r,"host clear routing");
    }
}
static void draw_batch(const Nv2aSurface *s,const Nv2aBatch *b,const ChVkFixedGeometry *fixed) {
    if(!b) fatal(VK_ERROR_INITIALIZATION_FAILED,"batch arguments");
    uint64_t t[6]={0};
    if(active->profile.enabled) t[0]=now_us();
    struct Surface *f=surface(s,b->state);
    if(active->profile.enabled) t[1]=now_us();
    observe_target_color(f);
    if(active->profile.enabled) t[2]=now_us();
    import_color(f);
    flush_sampled_colors(b);
    if(b->state->depth_test_enable || b->state->stencil_enable) import_depth(f,0);
    if(active->profile.enabled) t[3]=now_us();
    VkResult result=fixed?ch_vk_nv2a_draw_fixed_resolved_read(f->target,s,b,fixed,active->host.decode,
        active->textures?ch_vk_texture_cache_resolve:NULL,active->textures,f->rgba,f->bytes):
        ch_vk_nv2a_draw_resolved_read(f->target,s,b,active->host.decode,
        active->textures?ch_vk_texture_cache_resolve:NULL,active->textures,f->rgba,f->bytes);
    if(active->profile.enabled) t[4]=now_us();
    if(result!=VK_SUCCESS) {
        fprintf(stderr,"[Vulkan] rejected draw surface=%08X size=%ux%u vertices=%u ext=%d rc=%d stages=%08X\n",
            s->color_va,s->width,s->height,b->count,b->ext!=NULL,b->rc!=NULL,b->rc?b->rc->stage_program:0);
        const Nv2aRenderState *state=b->state;
        fprintf(stderr,"[Vulkan] rejected state raw=%d alpha=%u/%04X blend=%u/%04X/%04X/%04X cull=%u/%04X/%04X depth=%u/%u/%04X stencil=%u/%04X/%04X/%04X/%04X\n",
            b->raw!=NULL,state->alpha_test_enable,state->alpha_func,state->blend_enable,state->blend_src,state->blend_dst,state->blend_eq,
            state->cull_enable,state->cull_face,state->front_face,state->depth_test_enable,state->depth_write,state->depth_func,
            state->stencil_enable,state->stencil_func,state->stencil_fail,state->stencil_zfail,state->stencil_zpass);
        for(unsigned i=0;i<4;i++) {
            const Nv2aTexture *texture=b->stage[i]?b->stage[i]:(i==0?b->texture:NULL);
            if(texture) fprintf(stderr,"[Vulkan] texture%u address=%08X size=%ux%u format=%02X levels=%u filter=%08X address=%u/%u cube=%u\n",
                i,texture->offset,texture->width,texture->height,texture->color,texture->levels,texture->filter,
                texture->addr_u,texture->addr_v,texture->cube);
        }
    }
    store(f,result,"draw",(b->state->depth_test_enable && b->state->depth_write)
        || (b->state->stencil_enable && b->state->stencil_write_mask));
    if(active->profile.enabled) {t[5]=now_us();profile_draw(t);}
    if(active->host.render_draw) {
        VkResult r=active->host.render_draw(active->host.render_context,s,b);
        if(r!=VK_SUCCESS) fatal(r,"host draw routing");
    }
}
static void flip(void) {
    /* All guest work completed. Snapshot guest-authoritative pixels before
     * crossing the submission/display thread boundary. */
    if(!active) fatal(VK_ERROR_INITIALIZATION_FAILED,"flip without backend");
    flush();
    active->frames++;
    profile_frame();
    if(active->profile.enabled && (active->frames==1 || active->frames%60==0))
        fprintf(stderr,"[Vulkan raw vertices] GPU fixed batches=%llu CPU declined batches=%llu\n",
            (unsigned long long)active->raw_draws,(unsigned long long)active->raw_declined);
    const char *profile=getenv("CHIHIRO_VULKAN_PROFILE");
    if(active->profile.enabled && (active->frames==1 || active->frames%60==0)) {
        uint64_t draws=0,submits=0;
        for(unsigned i=0;i<active->count;i++) {ChVkTargetResourceStats stats;ch_vk_target_resource_stats(active->surfaces[i].target,&stats);draws+=stats.batch_draws;submits+=stats.batch_submissions;}
        fprintf(stderr,"[Vulkan command batches] live targets: draws=%llu submissions=%llu\n",(unsigned long long)draws,(unsigned long long)submits);
    }
    if(active->zeta && active->profile.enabled && (active->frames==1 || active->frames%60==0)) {
        ChVkDepthResidentStats stats;ch_vk_depth_resident_stats(active->zeta,&stats);
        fprintf(stderr,"[Vulkan depth residency] binds=%llu imports=%llu writebacks=%llu CPU merges=%llu GPU copies=%llu clear skips=%llu\n",
            (unsigned long long)stats.binds,(unsigned long long)stats.imports,
            (unsigned long long)stats.writebacks,(unsigned long long)stats.cpu_merges,(unsigned long long)stats.gpu_copies,
            (unsigned long long)stats.clear_skips);
    }
    if(active->textures && profile && atoi(profile) && (active->frames==1 || active->frames%60==0)) {
        ChVkTextureCacheStats stats;ch_vk_texture_cache_stats(active->textures,&stats);
        fprintf(stderr,"[Vulkan texture cache] hits=%llu uploads=%llu evictions=%llu live=%.1fMiB entries=%u/%u\n",
            (unsigned long long)stats.hits,(unsigned long long)stats.uploads,
            (unsigned long long)stats.evictions,stats.bytes/(1024.*1024.),stats.entries,stats.capacity);
    }
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
static void queue_lock(ChVkBackend *b) {
    if(b && b->host.lock_queue) b->host.lock_queue(b->host.queue_context);
}
static void queue_unlock(ChVkBackend *b) {
    if(b && b->host.unlock_queue) b->host.unlock_queue(b->host.queue_context);
}
static void locked_clear(const Nv2aSurface *s,const Nv2aRenderState *rs,uint32_t flags,uint32_t argb,uint32_t zs) {
    queue_lock(active); clear(s,rs,flags,argb,zs); queue_unlock(active);
}
static void locked_draw(const Nv2aSurface *s,const Nv2aBatch *b) {
    queue_lock(active); draw_batch(s,b,NULL); queue_unlock(active);
}
static void locked_flip(void) {
    queue_lock(active); flip(); queue_unlock(active);
}
static void locked_flush(void) {
    queue_lock(active); flush(); queue_unlock(active);
}
static int locked_observe_raw(const Nv2aSurface *s,const Nv2aBatch *b) {
    queue_lock(active); int result=observe_raw(s,b); queue_unlock(active); return result;
}
static int locked_draw_raw(const Nv2aSurface *s,const Nv2aBatch *b) {
    if(active->host.observe_raw) return locked_observe_raw(s,b);
    if(!active->host.raw_fixed || active->host.render_draw || !b || !b->state) return 0;
    /* Attribute unpacking is CPU-only and must not hold the shared XR queue
     * mutex. The original guest streams are borrowed until this call returns. */
    ChVkFixedGeometry fixed;
    VkResult r=ch_vk_fixed_prepare(b->raw,&fixed);
    if(r==VK_ERROR_FEATURE_NOT_PRESENT) {active->raw_declined++;return 0;}
    if(r!=VK_SUCCESS) fatal(r,"raw fixed attribute upload");
    queue_lock(active);draw_batch(s,b,&fixed);active->raw_draws++;queue_unlock(active);
    ch_vk_fixed_release(&fixed);return 1;
}
static Nv2aBackend callbacks={.clear=locked_clear,.draw=locked_draw,.flip=locked_flip};
const Nv2aBackend *ch_vk_backend_callbacks(void) { return &callbacks; }
VkResult ch_vk_backend_create(const ChVkBackendConfig *config,ChVkBackend **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;
    if(!config || !config->physical || !config->device || !config->queue ||
       !config->guest_base || !config->guest_bytes || !config->decode ||
       (!!config->lock_queue != !!config->unlock_queue) ||
       ((config->resident_color || config->resident_depth) && (config->render_clear || config->render_draw ||
        config->render_flip || config->guest_color_changed)))
        return VK_ERROR_INITIALIZATION_FAILED;
    ChVkBackend *b=calloc(1,sizeof(*b));
    if(!b) return VK_ERROR_OUT_OF_HOST_MEMORY;
    b->host=*config;
    const char *profile=getenv("CHIHIRO_VULKAN_PROFILE");
    b->profile.enabled=profile && atoi(profile)!=0;
    if(config->resident_depth) {
        VkResult r=ch_vk_depth_resident_create(config->guest_base,config->guest_bytes,&b->zeta);
        if(r!=VK_SUCCESS) {free(b);return r;}
        ch_vk_depth_resident_gpu_copies(b->zeta,!config->strict_depth_switches);
    }
    if(config->cache_textures) {
        ChVkTextureCacheConfig texture_config={.physical=config->physical,.device=config->device,
            .queue=config->queue,.family=config->family,.guest_base=config->guest_base,
            .guest_bytes=config->guest_bytes,.decode=config->decode,.max_entries=config->texture_cache_entries};
        VkResult r=ch_vk_texture_cache_create(&texture_config,&b->textures);
        if(r!=VK_SUCCESS) {ch_vk_depth_resident_destroy(b->zeta);free(b);return r;}
    }
    *out=b; return VK_SUCCESS;
}
VkResult ch_vk_backend_attach(ChVkBackend *b) {
    if(!b || active) return VK_ERROR_INITIALIZATION_FAILED;
    callbacks.draw_raw=(b->host.observe_raw || b->host.raw_fixed)?locked_draw_raw:NULL;
    callbacks.flush=(b->host.resident_color || b->host.resident_depth)?locked_flush:NULL;
    active=b; nv2a_backend_register(&callbacks); return VK_SUCCESS;
}
void ch_vk_backend_destroy(ChVkBackend *b) {
    if(!b) return;
    queue_lock(b);
    if(b->zeta) {
        VkResult r=ch_vk_depth_resident_release(b->zeta);
        if(r!=VK_SUCCESS) fatal(r,"resident depth teardown");
    }
    if(active==b) { nv2a_backend_register(NULL); active=NULL; }
    for(unsigned i=0;i<b->count;i++) release_surface(&b->surfaces[i]);
    ch_vk_texture_cache_destroy(b->textures);
    ch_vk_depth_resident_destroy(b->zeta);
    queue_unlock(b);
    free(b);
}
