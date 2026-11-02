#include "vk_backend.h"
#include "vk_state.h"
#include "vk_depth_guest.h"
#include "vk_guest_layout.h"
#include "vk_texture_cache.h"
#include "vk_depth_resident.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#endif
#define MAX_SURFACES 32
struct Surface { Nv2aSurface guest; ChVkGuestLayout layout; uint32_t zeta; Nv2aRenderState state; float *depth; uint8_t *stencil; ChVkTarget *target; unsigned char *rgba,*observed,*cpu,*raw; ChGuestWriteRange *write_range; size_t raw_span; int observed_valid,resident,color_bgra,color_fallback_reported,color_dirty,native_z24,quantized_z24; size_t bytes; uint64_t color_serial; };
typedef struct {uint64_t pipeline_ns,batch_wait_ns,batch_gpu_ns,upload_wait_ns;} FrameCosts;
struct ChVkBackend { ChVkFixedScratch raw_scratch; struct {int enabled;uint64_t completion_us,completion_cpu_ns,completion_calls,prepare_us,prepare_cpu_ns;uint64_t draw_cpu_ns,clear_cpu_ns,flip_cpu_ns,trim_cpu_ns;uint64_t draw_us,clear_us,flip_us,trim_us,surface_us,coherence_us,imports_us,raster_us,store_us,texture_uploads;FrameCosts retired,previous;} costs; VkPipelineCache pipeline_cache; struct {int enabled;uint64_t start,n,surface,coherence,imports,draw,store,no_color,flushes,depth_flushes,paired_flushes;} profile; struct {int enabled;uint64_t previous,us[120];unsigned count;} frame_profile; ChVkBackendConfig host; ChVkTextureCache *textures; ChVkDepthResident *zeta; uint64_t raw_compact_batches,raw_snapshot_bytes,raw_expanded_bytes; uint64_t color_snapshot_copy_bytes_saved,unchanged_color_draws,unchanged_depth_draws,early_color_reads,clear_color_imports_avoided,clear_color_bytes_avoided; uint64_t frames,raw_draws,raw_programs,raw_program_vertices,raw_declined,color_clock,cpu_batches,cpu_vertices,cpu_texels,cpu_texture_us,cpu_color_pixels,cpu_color_us,cpu_depth_pixels; struct Surface surfaces[MAX_SURFACES]; unsigned count; };
static ChVkBackend *active;
static void flush_color(struct Surface *);
static void observe_guest_colors(void);
static void observe_guest_color(struct Surface *);
/* Optional host scheduling experiment, once on each actual rendering thread.
 * Uses normal macOS QoS, not realtime policy, affinity, or busy waiting. */
static void configure_render_thread(void) {
#ifdef __APPLE__
    static _Thread_local int configured;
    if(configured) return;
    configured=1;
    const char *setting=getenv("CHIHIRO_VULKAN_RENDER_QOS");
    if(!setting) return;
    int mode=atoi(setting),relative=0,result=0;
    qos_class_t before=QOS_CLASS_UNSPECIFIED,after=QOS_CLASS_UNSPECIFIED;
    int queried=pthread_get_qos_class_np(pthread_self(),&before,&relative);
    qos_class_t requested=mode==1?QOS_CLASS_USER_INITIATED:mode==2?QOS_CLASS_USER_INTERACTIVE:QOS_CLASS_UNSPECIFIED;
    if(requested!=QOS_CLASS_UNSPECIFIED && !(before==QOS_CLASS_USER_INTERACTIVE && mode==1))
        result=pthread_set_qos_class_self_np(requested,0);
    int verified=pthread_get_qos_class_np(pthread_self(),&after,&relative);
    fprintf(stderr,"[Vulkan render thread] mode=%d qos_before=%u qos_after=%u query=%d set=%d verify=%d\n",
        mode,(unsigned)before,(unsigned)after,queried,result,verified);
#endif
}
static uint64_t now_us(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000u+t.tv_nsec/1000;
}
/* Optional diagnostic: thread CPU excludes fence blocking and descheduling,
 * but includes driver work on this calling thread. It is not shader time. */
static uint64_t thread_cpu_ns(void) {
#ifdef CLOCK_THREAD_CPUTIME_ID
    struct timespec t;
    if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t)==0)
        return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;
#endif
    return 0;
}
static void profile_draw(const uint64_t t[6]) {
    if(!active->profile.enabled) return;
    if(!active->profile.start) active->profile.start=t[0];
    active->profile.n++;
    if(active->costs.enabled) {
        active->costs.draw_us+=t[5]-t[0];active->costs.surface_us+=t[1]-t[0];
        active->costs.coherence_us+=t[2]-t[1];active->costs.imports_us+=t[3]-t[2];
        active->costs.raster_us+=t[4]-t[3];active->costs.store_us+=t[5]-t[4];
    }
    active->profile.surface+=t[1]-t[0];active->profile.coherence+=t[2]-t[1];
    active->profile.imports+=t[3]-t[2];active->profile.draw+=t[4]-t[3];active->profile.store+=t[5]-t[4];
    if(t[5]-active->profile.start<1000000) return;
    fprintf(stderr,"[Vulkan backend] %llu batches: surface %.1fms CPU edits %.1fms imports %.1fms draw %.1fms writeback %.1fms (color disabled=%llu)\n",
        (unsigned long long)active->profile.n,active->profile.surface/1000.,active->profile.coherence/1000.,
        active->profile.imports/1000.,active->profile.draw/1000.,active->profile.store/1000.,
        (unsigned long long)active->profile.no_color);
    fprintf(stderr,"[Vulkan publication overlap] flushes=%llu dirty_depth=%llu same_target_dirty_color=%llu\n",
        (unsigned long long)active->profile.flushes,(unsigned long long)active->profile.depth_flushes,
        (unsigned long long)active->profile.paired_flushes);
    active->profile.flushes=active->profile.depth_flushes=active->profile.paired_flushes=0;
    active->profile.n=active->profile.surface=active->profile.coherence=active->profile.imports=0;
    active->profile.draw=active->profile.store=active->profile.no_color=0;active->profile.start=t[5];
}
static int decode_cpu_texture(const Nv2aTexture *texture,uint32_t *argb) {
    uint64_t start=active->profile.enabled?now_us():0;
    int result=active->host.decode(texture,argb);
    if(active->profile.enabled) {
        active->cpu_texels+=(uint64_t)texture->width*texture->height;
        active->cpu_texture_us+=now_us()-start;
    }
    return result;
}
static int compare_us(const void *a,const void *b) {
    uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return (x>y)-(x<y);
}
static int add_target_costs(FrameCosts *costs,ChVkTarget *target) {
    ChVkTargetResourceStats resource;ChVkTargetTimingStats timing;
    ch_vk_target_resource_stats(target,&resource);ch_vk_target_timing_stats(target,&timing);
    costs->pipeline_ns+=resource.pipeline_create_ns;
    costs->batch_wait_ns+=timing.batch_wait_ns;costs->batch_gpu_ns+=timing.batch_gpu_ns;
    costs->upload_wait_ns+=timing.depth_upload_wait_ns;
    return timing.available;
}
static void profile_frame(void) {
    if(!active->frame_profile.enabled) return;
    uint64_t now=now_us(),previous=active->frame_profile.previous;
    active->frame_profile.previous=now;
    if(active->costs.enabled) {
        FrameCosts total=active->costs.retired,*old=&active->costs.previous;int timed=0;
        for(unsigned i=0;i<active->count;i++) timed|=add_target_costs(&total,active->surfaces[i].target);
        ChVkTextureCacheStats textures={0};if(active->textures) ch_vk_texture_cache_stats(active->textures,&textures);
        if(previous) fprintf(stderr,"[Vulkan frame costs] frame=%llu interval_ms=%.3f draw_ms=%.3f clear_ms=%.3f flip_flush_ms=%.3f cache_trim_ms=%.3f completion_ms=%.3f prepare_ms=%.3f other_ms=%.3f pipeline_ms=%.3f batch_wait_ms=%.3f batch_gpu_ms=%.3f depth_upload_wait_ms=%.3f gpu_timing=%d surface_ms=%.3f coherence_ms=%.3f imports_ms=%.3f raster_ms=%.3f store_ms=%.3f texture_uploads=%llu draw_cpu_ms=%.3f clear_cpu_ms=%.3f flip_cpu_ms=%.3f cache_trim_cpu_ms=%.3f completion_cpu_ms=%.3f prepare_cpu_ms=%.3f completion_calls=%llu thread_cpu_timing=%d\n",
            (unsigned long long)active->frames,(now-previous)/1000.,active->costs.draw_us/1000.,active->costs.clear_us/1000.,active->costs.flip_us/1000.,active->costs.trim_us/1000.,active->costs.completion_us/1000.,active->costs.prepare_us/1000.,
            ((double)(now-previous)-active->costs.draw_us-active->costs.clear_us-active->costs.flip_us-active->costs.trim_us-active->costs.completion_us-active->costs.prepare_us)/1000.,
            (total.pipeline_ns-old->pipeline_ns)/1e6,(total.batch_wait_ns-old->batch_wait_ns)/1e6,
            (total.batch_gpu_ns-old->batch_gpu_ns)/1e6,(total.upload_wait_ns-old->upload_wait_ns)/1e6,timed,active->costs.surface_us/1000.,active->costs.coherence_us/1000.,
            active->costs.imports_us/1000.,active->costs.raster_us/1000.,active->costs.store_us/1000.,
            (unsigned long long)(textures.uploads-active->costs.texture_uploads),active->costs.draw_cpu_ns/1e6,
            active->costs.clear_cpu_ns/1e6,active->costs.flip_cpu_ns/1e6,active->costs.trim_cpu_ns/1e6,active->costs.completion_cpu_ns/1e6,active->costs.prepare_cpu_ns/1e6,(unsigned long long)active->costs.completion_calls,thread_cpu_ns()!=0);
        active->costs.completion_us=active->costs.completion_cpu_ns=active->costs.completion_calls=active->costs.prepare_us=active->costs.prepare_cpu_ns=0;
        active->costs.texture_uploads=textures.uploads;
        active->costs.surface_us=active->costs.coherence_us=active->costs.imports_us=active->costs.raster_us=active->costs.store_us=0;
        active->costs.draw_cpu_ns=active->costs.clear_cpu_ns=active->costs.flip_cpu_ns=active->costs.trim_cpu_ns=0;
        *old=total;active->costs.draw_us=active->costs.clear_us=active->costs.flip_us=active->costs.trim_us=0;
    }
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
    ch_guest_write_range_destroy(f->write_range);
    ch_vk_target_destroy(f->target);free(f->rgba);free(f->observed);free(f->cpu);free(f->raw);
    free(f->depth);free(f->stencil);memset(f,0,sizeof *f);
}
static void color_encoding(struct Surface *f,int swizzled) {
    int bgra=active->host.gpu_color_pack && f->resident && !active->host.guest_color_changed;
    if(swizzled) {
        uint32_t wx=(f->state.surface_format>>16)&255,hy=f->state.surface_format>>24;
        bgra=bgra && active->host.gpu_morton_color && wx<=15 && hy<=15 &&
            f->guest.width==(1u<<wx) && f->guest.height==(1u<<hy)?2:0;
    }
    if(bgra==f->color_bgra) return;
    if(active->profile.enabled && !bgra && !f->color_fallback_reported) {
        fprintf(stderr,"[Vulkan color fallback] %ux%u bpp=%u Morton=%d\n",f->guest.width,f->guest.height,f->guest.bytes_per_pixel,swizzled);
        f->color_fallback_reported=1;
    }
    VkResult r=ch_vk_target_color_layout(f->target,bgra);
    if(r!=VK_SUCCESS) fatal(r,"GPU color conversion setup");
    f->color_bgra=bgra;f->observed_valid=0;
}
static VkResult load_surface_color(struct Surface *f,void *pixels) {
    if(!f->color_bgra) {
        uint64_t start=active->profile.enabled?now_us():0;
        VkResult r=ch_vk_nv2a_load_color_state(&f->guest,&f->state,pixels,f->bytes,active->host.guest_base,active->host.guest_bytes);
        if(active->profile.enabled) {active->cpu_color_pixels+=f->bytes/4;active->cpu_color_us+=now_us()-start;}
        return r;
    }
    const unsigned char *src=(const unsigned char *)active->host.guest_base+f->guest.color_va;
    if(f->color_bgra==2) {memcpy(pixels,src,f->bytes);return VK_SUCCESS;}
    for(uint32_t y=0;y<f->guest.height;y++)
        memcpy((unsigned char *)pixels+(size_t)y*f->guest.width*4,src+(size_t)y*f->guest.pitch,(size_t)f->guest.width*4);
    return VK_SUCCESS;
}
static VkResult store_surface_color(struct Surface *f,const void *pixels) {
    if(!f->color_bgra) {
        uint64_t start=active->profile.enabled?now_us():0;
        VkResult r=ch_vk_nv2a_store_color_state(&f->guest,&f->state,pixels,f->bytes,active->host.guest_base,active->host.guest_bytes);
        if(active->profile.enabled) {active->cpu_color_pixels+=f->bytes/4;active->cpu_color_us+=now_us()-start;}
        return r;
    }
    unsigned char *dst=(unsigned char *)active->host.guest_base+f->guest.color_va;
    if(f->color_bgra==2) {memcpy(dst,pixels,f->bytes);return VK_SUCCESS;}
    for(uint32_t y=0;y<f->guest.height;y++)
        memcpy(dst+(size_t)y*f->guest.pitch,(const unsigned char *)pixels+(size_t)y*f->guest.width*4,(size_t)f->guest.width*4);
    return VK_SUCCESS;
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
        VkResult r=ch_vk_depth_resident_prepare_color(active->zeta,s->color_va,(size_t)layout.span);
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
        uint64_t old_start=previous->guest.color_va,old_end=old_start+previous->layout.span;
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
            ch_guest_write_range_destroy(f->write_range);
            f->write_range=ch_guest_write_range_create(active->host.guest_write_watch,
                (unsigned char *)active->host.guest_base+s->color_va,(size_t)span);
        }
        f->guest=*s;f->state=*rs;f->layout=layout;color_encoding(f,layout.swizzled);return f;
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
        if(active->costs.enabled) add_target_costs(&active->costs.retired,f->target);
        release_surface(f);
    } else {
        if(active->count==MAX_SURFACES) fatal(VK_ERROR_TOO_MANY_OBJECTS,"surface limit");
        f=&active->surfaces[active->count++];
    }
    f->layout=layout; f->guest=*s; f->zeta=rs->zeta_va; f->state=*rs; f->bytes=(size_t)s->width*s->height*4;
    f->rgba=malloc(f->bytes);
    f->resident=active->host.resident_color && s->bytes_per_pixel==4;
    if(active->host.guest_color_changed || f->resident) {
        f->observed=malloc(f->bytes);
        if(!f->observed) fatal(VK_ERROR_OUT_OF_HOST_MEMORY,"guest color observation allocation");
    }
    if(f->resident) {
        f->cpu=malloc(f->bytes); f->raw=malloc((size_t)span); f->raw_span=(size_t)span;
        if(!f->cpu || !f->raw) fatal(VK_ERROR_OUT_OF_HOST_MEMORY,"CPU color staging");
        f->write_range=ch_guest_write_range_create(active->host.guest_write_watch,
            (unsigned char *)active->host.guest_base+s->color_va,(size_t)span);
    }
    if(!f->rgba) fatal(VK_ERROR_OUT_OF_HOST_MEMORY,"surface readback allocation");
    VkResult r=VK_ERROR_FORMAT_NOT_SUPPORTED;
    if(active->zeta && ((rs->surface_format>>4)&15)==2) {
        r=ch_vk_target_create_depth(active->host.physical,active->host.device,active->host.queue,
            active->host.family,s->width,s->height,VK_FORMAT_D24_UNORM_S8_UINT,&f->target);
        if(r==VK_SUCCESS) f->native_z24=1;
    }
    if(r==VK_ERROR_FORMAT_NOT_SUPPORTED) r=(active->host.depth_quant_inline?ch_vk_target_create_input_depth:ch_vk_target_create)(active->host.physical,active->host.device,active->host.queue,
        active->host.family,s->width,s->height,&f->target);
    if(r!=VK_SUCCESS) fatal(r,"target creation");
    /* Surface creation/draw/destruction already hold the backend queue lock.
     * Retain compiler data across targets and resizes without sharing mutable
     * descriptor pools or pending pipeline objects. */
    if(!active->pipeline_cache) {
        VkPipelineCacheCreateInfo info={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        r=vkCreatePipelineCache(active->host.device,&info,NULL,&active->pipeline_cache);
        if(r!=VK_SUCCESS) fatal(r,"shared pipeline cache creation");
    }
    r=ch_vk_target_pipeline_cache(f->target,active->pipeline_cache);
    if(r!=VK_SUCCESS) fatal(r,"shared pipeline cache binding");
    color_encoding(f,layout.swizzled);
    if(active->profile.enabled) fprintf(stderr,"[Vulkan color transfer] %ux%u bpp=%u Morton=%d GPU conversion=%d\n",
        s->width,s->height,s->bytes_per_pixel,layout.swizzled,f->color_bgra);
    if(f->resident && active->host.batch_draws) {
        r=ch_vk_target_batch_limit(f->target,active->host.batch_draw_limit);
        if(r==VK_SUCCESS) r=ch_vk_target_batching(f->target,1);
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
    if(active->host.depth_quant_inline && !f->native_z24) {
        /* Prepare the compatible pass before even a color-only draw can build
         * a pipeline. Actual quantization remains conditional on guest Z24. */
        r=ch_vk_target_depth_quantization(f->target,4);
        if(r==VK_SUCCESS) r=ch_vk_target_depth_quantization(f->target,0);
        if(r!=VK_SUCCESS) fatal(r,"input attachment target setup");
    }
    ch_vk_target_defer_color_readback(f->target,f->resident);
    return f;
}
static void store(struct Surface *f,VkResult result,const char *operation,int color_dirty,int depth_dirty) {
    if(result!=VK_SUCCESS) fatal(result,operation);
    if(color_dirty) f->color_serial=++active->color_clock;
    if(f->resident) f->color_dirty|=!!color_dirty;
    else {
        result=store_surface_color(f,f->rgba);
        if(result!=VK_SUCCESS) fatal(result,"color writeback");
        if(f->observed) {
            /* RGB565 writeback quantizes: compare with canonical stored pixels,
             * not the higher precision GPU output, to avoid false CPU edits. */
            result=load_surface_color(f,f->observed);
            if(result!=VK_SUCCESS) fatal(result,"color observation baseline");
            f->observed_valid=1;
        }
    }
    if(f->state.zeta_va && depth_dirty) {
        if(active->zeta && (f->native_z24 || f->quantized_z24) && ((f->state.surface_format>>4)&15)==2) {
            result=ch_vk_depth_resident_dirty(active->zeta,f->target);
            if(result!=VK_SUCCESS) fatal(result,"resident depth ownership");
            return;
        }
        active->cpu_depth_pixels+=f->bytes/4;
        result=ch_vk_target_read_depth_stencil(f->target,f->depth,f->stencil,f->bytes/4);
        if(result==VK_SUCCESS) result=ch_vk_guest_depth_store(&f->guest,&f->state,active->host.guest_base,active->host.guest_bytes,f->depth,f->stencil);
        if(result!=VK_SUCCESS) fatal(result,"depth writeback");
    }
}
/* In these layouts the canonical observation bytes are also an exact raw
 * guest snapshot. Pitched/padded or converted views retain a separate copy. */
static int color_snapshot_is_raw(const struct Surface *f) {
    return f->raw_span==f->bytes && (f->color_bgra==2 ||
        (f->color_bgra==1 && f->guest.pitch==(uint64_t)f->guest.width*4));
}
static const unsigned char *color_raw_snapshot(const struct Surface *f) {
    return color_snapshot_is_raw(f)?f->observed:f->raw;
}
static void publish_color(struct Surface *f,int ready) {
    if(!f->resident || !f->color_dirty) return;
    VkResult r=ready?VK_SUCCESS:ch_vk_target_read_color(f->target,f->rgba,f->bytes);
    void *storage=(unsigned char *)active->host.guest_base+f->guest.color_va;
    ch_guest_write_watch_begin(active->host.guest_write_watch,storage,f->raw_span);
    if(r==VK_SUCCESS) r=store_surface_color(f,f->rgba);
    if(r==VK_SUCCESS) {
        /* All readback requests have retired before publication. Transfer
         * ownership of their result instead of copying it into the baseline. */
        unsigned char *scratch=f->observed;f->observed=f->rgba;f->rgba=scratch;
        active->color_snapshot_copy_bytes_saved+=f->bytes;
        if(color_snapshot_is_raw(f)) active->color_snapshot_copy_bytes_saved+=f->raw_span;
        else memcpy(f->raw,storage,f->raw_span);
    }
    ch_guest_write_watch_end(active->host.guest_write_watch,storage,f->raw_span);
    if(r!=VK_SUCCESS) fatal(r,"resident color writeback");
    f->observed_valid=1; f->color_dirty=0;
}
static void flush_color(struct Surface *f) {publish_color(f,0);}
static void observe_guest_color_before_clear(struct Surface *f,int overwrite) {
    if(!f->observed) return;
    const unsigned char *raw=(const unsigned char *)active->host.guest_base+f->guest.color_va;
    if(f->resident && f->observed_valid && f->write_range &&
        !ch_guest_write_range_changed(f->write_range)) return;
    /* Fast unchanged-storage check before Morton/pixel decoding. Padding
     * changes fall through once, then the pixel comparison ignores them. */
    if(f->resident && f->observed_valid && !memcmp(raw,color_raw_snapshot(f),f->raw_span)) return;
    unsigned char *cpu=f->resident?f->cpu:f->rgba;
    VkResult r=load_surface_color(f,cpu);
    if(r!=VK_SUCCESS) fatal(r,"guest color observation");
    if(!f->observed_valid || memcmp(cpu,f->observed,f->bytes)) {
        if(f->resident && overwrite) {
            /* Full color clear immediately replaces every pixel. Still retain
             * the guest baseline so later CPU edits merge against RAM at the
             * clear boundary, not against superseded GPU/CPU contents. */
            active->clear_color_imports_avoided++;
            active->clear_color_bytes_avoided+=f->bytes;
        } else if(f->resident) {
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
            f->color_serial=++active->color_clock;
        }
        if(active->host.guest_color_changed) {
            r=active->host.guest_color_changed(active->host.render_context,&f->guest,cpu,f->bytes,!f->observed_valid);
            if(r!=VK_SUCCESS) fatal(r,"host CPU color coherence");
        }
        memcpy(f->observed,cpu,f->bytes); f->observed_valid=1;
    }
    if(f->resident && !color_snapshot_is_raw(f)) memcpy(f->raw,raw,f->raw_span);
}
static void observe_guest_color(struct Surface *f) {observe_guest_color_before_clear(f,0);}
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
    uint64_t paired_serial[MAX_SURFACES]={0};uint32_t paired=0,queued_colors=0;
    ChVkDepthColorRead colors[MAX_SURFACES];size_t color_count=0;
    if(active->profile.enabled) active->profile.flushes++;
    /* Pair every retained owner, not just the most recently bound target.
     * Reading other owners first would force a second color submission. */
    for(unsigned i=0;active->zeta && i<active->count;i++) {
        struct Surface *f=&active->surfaces[i];
        if(!ch_vk_depth_resident_target_dirty(active->zeta,f->target)) continue;
        if(active->profile.enabled) active->profile.depth_flushes++;
        if(f->resident && f->color_dirty) {
            colors[color_count++]=(ChVkDepthColorRead){f->target,f->rgba,f->bytes};
            paired|=1u<<i;paired_serial[i]=f->color_serial;
            if(active->profile.enabled) active->profile.paired_flushes++;
        }
    }
    /* Color-only targets with disjoint depth storage can run while depth
     * owners submit/retire. Hooks and overlapping storage keep the original
     * observation order. No guest color publication happens early. */
    if(active->zeta && !active->host.guest_color_changed) {
        for(unsigned i=0;i<active->count;i++) {
            struct Surface *f=&active->surfaces[i];
            if(!f->resident || !f->color_dirty || !ch_vk_target_pending(f->target) ||
                ch_vk_depth_resident_target_dirty(active->zeta,f->target) ||
                ch_vk_depth_resident_check_color(active->zeta,f->guest.color_va,f->raw_span)!=VK_SUCCESS) continue;
            observe_guest_color(f);
            /* A CPU edit merge may already have retired the batch. */
            if(!ch_vk_target_pending(f->target)) continue;
            VkResult r=ch_vk_target_read_color_begin(f->target,f->rgba,f->bytes);
            if(r!=VK_SUCCESS) fatal(r,"early color completion");
            queued_colors|=1u<<i;active->early_color_reads++;
        }
    }
    if(active->zeta) {
        VkResult r=ch_vk_depth_resident_flush_many(active->zeta,colors,color_count);
        if(r!=VK_SUCCESS) fatal(r,"resident depth completion");
    }
    /* Completion only publishes GPU-written storage. An idle target's RAM
     * already contains its authoritative CPU edits; observe those when the
     * target is next drawn, cleared, sampled or reinterpreted. Routing hooks
     * still require notifications for every surface at this boundary. */
    if(active->host.guest_color_changed) observe_guest_colors();
    else for(unsigned i=0;i<active->count;i++)
        if(active->surfaces[i].color_dirty && !(queued_colors&(1u<<i))) observe_guest_color(&active->surfaces[i]);
    for(unsigned i=0;i<active->count;i++) {
        struct Surface *f=&active->surfaces[i];
        /* CPU color observation may have merged edits after the paired read.
         * Such an import advances the serial and requires a fresh GPU read. */
        if(f->resident && f->color_dirty && !(queued_colors&(1u<<i)) && !((paired&(1u<<i)) && f->color_serial==paired_serial[i])) {
            VkResult r=ch_vk_target_read_color_begin(f->target,f->rgba,f->bytes);
            if(r!=VK_SUCCESS) fatal(r,"queued color completion");
            queued_colors|=1u<<i;
        }
    }
    for(unsigned i=0;i<active->count;i++) {
        struct Surface *f=&active->surfaces[i];
        if(queued_colors&(1u<<i)) {
            VkResult r=ch_vk_target_read_color_end(f->target);
            if(r!=VK_SUCCESS) fatal(r,"queued color retirement");
        }
        publish_color(f,1);
    }
    /* Draws with no dirty resident storage still own resources and must finish
     * before the executor publishes the guest semaphore/query boundary. */
    for(unsigned i=0;i<active->count;i++) {
        VkResult r=ch_vk_target_finish(active->surfaces[i].target);
        if(r!=VK_SUCCESS) fatal(r,"pending guest command completion");
    }
}
static int gpu_sample_matches(const struct Surface *f,const Nv2aTexture *tx) {
    if(!active->host.sample_render_targets || !f->resident || tx->cube ||
       tx->offset!=f->guest.color_va || tx->width!=f->guest.width || tx->height!=f->guest.height)
        return 0;
    const ChVkGuestLayout layout=f->layout;
    if(layout.swizzled) {
        uint32_t wx=(f->state.surface_format>>16)&255,hy=f->state.surface_format>>24;
        if(tx->color!=0x06 || tx->width!=(1u<<wx) || tx->height!=(1u<<hy)) return 0;
    } else if(tx->color!=0x12 || tx->pitch!=f->guest.pitch) return 0;
    size_t span,palette;
    return ch_vk_texture_storage_span(tx,&span,&palette)==VK_SUCCESS && !palette && span==layout.span;
}
static VkResult resolve_sample(void *context,const Nv2aTexture *tx,ChVkSampleImage *out) {
    ChVkBackend *b=context;
    if(!b->host.sample_render_targets) return ch_vk_texture_cache_resolve(b->textures,tx,out);
    for(unsigned i=0;i<b->count;i++) {
        struct Surface *f=&b->surfaces[i];
        if(!gpu_sample_matches(f,tx)) continue;
        /* Pre-draw alias handling already observes this source. Rechecking
         * here also covers callers that resolve a binding more than once. */
        observe_guest_color(f);
        VkResult r=ch_vk_texture_cache_resolve_target(b->textures,tx,f->target,f->color_serial,out);
        if(r!=VK_NOT_READY) return r;
        /* Budget/pin pressure retains the exact decoder path. Publish the
         * source before a CPU cache lookup or uncached decode can read it. */
        flush_color(f);break;
    }
    return b->textures?ch_vk_texture_cache_resolve(b->textures,tx,out):VK_NOT_READY;
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
            const ChVkGuestLayout layout=f->layout;
            uint64_t color=f->guest.color_va;
            int overlap=storage!=VK_SUCCESS ||
                ((uint64_t)tx->offset<=color?color-tx->offset<span:(uint64_t)tx->offset-color<layout.span) ||
                (palette && ((uint64_t)tx->palette<=color?color-tx->palette<palette:(uint64_t)tx->palette-color<layout.span));
            if(overlap) {
                observe_guest_color(f);
                if(!gpu_sample_matches(f,tx)) flush_color(f);
            }
        }
    }
}
static void import_color(struct Surface *f) {
    if(f->resident) return; /* CPU changes already merged by observation. */
    VkResult r=load_surface_color(f,f->rgba);
    if(r!=VK_SUCCESS) fatal(r,"guest color read");
    r=ch_vk_target_import_color(f->target,f->rgba,f->bytes);
    if(r!=VK_SUCCESS) fatal(r,"guest color import");
}
static void import_depth(struct Surface *f,int overwrite) {
    if(!f->state.zeta_va) return;
    if(active->host.quantized_depth && !f->native_z24) {
        f->quantized_z24=((f->state.surface_format>>4)&15)==2;
        VkResult q=ch_vk_target_depth_quantization(f->target,f->quantized_z24?(active->host.depth_quant_fragment?5:active->host.depth_quant_inline?4:active->host.depth_quant_draw?2:1):0);
        if(q!=VK_SUCCESS) fatal(q,"D32 depth quantization setup");
    }
    if(active->zeta) {
        ChVkGuestLayout depth_layout;
        uint32_t format=(f->state.surface_format>>4)&15;
        VkResult r=ch_vk_guest_layout(f->state.surface_format,f->guest.width,f->guest.height,
            f->state.zeta_pitch,format==1?2:4,&depth_layout);
        if(r!=VK_SUCCESS) fatal(r,"resident depth layout");
        /* Cross color/depth reinterpretation needs a unified image owner.
         * Fail explicitly rather than letting either resident view go stale. */
        for(unsigned i=0;i<active->count;i++) {
            struct Surface *c=&active->surfaces[i];const ChVkGuestLayout color_layout=c->layout;
            if((uint64_t)c->guest.color_va<f->state.zeta_va+depth_layout.span &&
               (uint64_t)f->state.zeta_va<c->guest.color_va+color_layout.span)
                fatal(VK_ERROR_FEATURE_NOT_PRESENT,"color/depth storage alias");
        }
        if((f->native_z24 || f->quantized_z24) && format==2) {
            r=overwrite?ch_vk_depth_resident_bind_clear(active->zeta,f->target,&f->guest,&f->state):
                ch_vk_depth_resident_bind(active->zeta,f->target,&f->guest,&f->state);
            if(r!=VK_SUCCESS) fatal(r,"resident depth bind");
            return;
        }
        r=ch_vk_depth_resident_release(active->zeta);
        if(r!=VK_SUCCESS) fatal(r,"resident depth strict transition");
    }
    active->cpu_depth_pixels+=f->bytes/4;
    VkResult r=ch_vk_guest_depth_load(&f->guest,&f->state,active->host.guest_base,active->host.guest_bytes,f->depth,f->stencil);
    if(r!=VK_SUCCESS) fprintf(stderr,"[Vulkan] depth surface=%08X zeta=%08X format=%08X pitch=%u size=%ux%u\n",
        f->guest.color_va,f->state.zeta_va,f->state.surface_format,f->state.zeta_pitch,f->guest.width,f->guest.height);
    if(r==VK_SUCCESS) r=ch_vk_target_write_depth_stencil(f->target,f->depth,f->stencil,f->bytes/4);
    if(r!=VK_SUCCESS) fatal(r,"guest depth import");
}
static void clear(const Nv2aSurface *s,const Nv2aRenderState *rs,uint32_t flags,uint32_t argb,uint32_t zs) {
    configure_render_thread();
    uint64_t cpu=active->costs.enabled?thread_cpu_ns():0;
    uint64_t start=active->costs.enabled?now_us():0;
    struct Surface *f=surface(s,rs);
    int full_color=ch_vk_nv2a_clear_color_full(s,rs,flags);
    if(f->resident && full_color &&
       !active->host.guest_color_changed && !active->host.render_clear)
        observe_guest_color_before_clear(f,1);
    else if(!f->resident || (flags&0xf0)) observe_target_color(f);
    if(!full_color) import_color(f);
    if(flags & 3) import_depth(f,!active->host.strict_depth_clears && ch_vk_nv2a_clear_depth_full(s,rs,flags));
    store(f,ch_vk_nv2a_clear_state_read(f->target,s,rs,flags,argb,zs,f->rgba,f->bytes),"clear",flags&0xf0,flags&3);
    if(active->host.render_clear) {
        VkResult r=active->host.render_clear(active->host.render_context,s,rs,flags,argb,zs);
        if(r!=VK_SUCCESS) fatal(r,"host clear routing");
    }
    if(active->costs.enabled) {active->costs.clear_us+=now_us()-start;active->costs.clear_cpu_ns+=thread_cpu_ns()-cpu;}
}
static void draw_batch(const Nv2aSurface *s,const Nv2aBatch *b,const ChVkFixedGeometry *fixed) {
    configure_render_thread();
    if(!b || !s) fatal(VK_ERROR_INITIALIZATION_FAILED,"batch arguments");
    /* An empty guest clip cannot rasterize fragments. Reset-state draws may
     * still carry an unset format/pitch; no image or guest memory is touched.
     * Pending earlier work remains ordered by the normal fence/flip path. */
    if(!s->width || !s->height) return;
    if(!fixed) {active->cpu_batches++;active->cpu_vertices+=b->count;}
    uint64_t cpu=active->costs.enabled?thread_cpu_ns():0;
    uint64_t t[6]={0};
    if(active->profile.enabled) t[0]=now_us();
    struct Surface *f=surface(s,b->state);
    if(active->profile.enabled) t[1]=now_us();
    int color_write=!!(b->state->color_mask&0x01010101);
    /* A depth/stencil-only pass cannot consume or overwrite destination
     * color. Preserve pending CPU edits until color is actually used. */
    if(!f->resident || color_write) observe_target_color(f);
    if(active->profile.enabled && !color_write) active->profile.no_color++;
    if(active->profile.enabled) t[2]=now_us();
    import_color(f);
    flush_sampled_colors(b);
    if(b->state->depth_test_enable || b->state->stencil_enable) import_depth(f,0);
    if(active->profile.enabled) t[3]=now_us();
    VkResult result=fixed?ch_vk_nv2a_draw_fixed_resolved_read(f->target,s,b,fixed,decode_cpu_texture,
        active->textures?resolve_sample:NULL,active,f->rgba,f->bytes):
        ch_vk_nv2a_draw_resolved_read(f->target,s,b,decode_cpu_texture,
        active->textures?resolve_sample:NULL,active,f->rgba,f->bytes);
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
    unsigned writes=ch_vk_attachment_write_mask(b->state);
    active->unchanged_color_draws+=color_write && !(writes&1);
    active->unchanged_depth_draws+=((b->state->depth_test_enable && b->state->depth_write) ||
        (b->state->stencil_enable && b->state->stencil_write_mask)) && !(writes&2);
    store(f,result,"draw",writes&1,writes&2);
    if(active->costs.enabled) active->costs.draw_cpu_ns+=thread_cpu_ns()-cpu;
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
    uint64_t cpu=active->costs.enabled?thread_cpu_ns():0;
    uint64_t start=active->costs.enabled?now_us():0;
    flush();
    if(active->costs.enabled) {active->costs.flip_us+=now_us()-start;active->costs.flip_cpu_ns+=thread_cpu_ns()-cpu;}
    active->frames++;
    cpu=active->costs.enabled?thread_cpu_ns():0;
    start=active->costs.enabled?now_us():0;
    ch_vk_texture_cache_advance_frame(active->textures);
    if(active->costs.enabled) {active->costs.trim_us+=now_us()-start;active->costs.trim_cpu_ns+=thread_cpu_ns()-cpu;}
    if(active->host.guest_write_watch && active->profile.enabled && (active->frames==1 || active->frames%60==0)) {
        ChGuestWriteStats stats;ch_guest_write_watch_stats(active->host.guest_write_watch,&stats);
        fprintf(stderr,"[Vulkan write tracking] ranges=%llu checks=%llu unchanged=%llu faults=%llu protection calls=%llu\n",
            (unsigned long long)stats.ranges,(unsigned long long)stats.checks,(unsigned long long)stats.unchanged,
            (unsigned long long)stats.faults,(unsigned long long)stats.protect_calls);
    }
    profile_frame();
    if(active->profile.enabled && (active->frames==1 || active->frames%60==0))
        fprintf(stderr,"[Vulkan raw vertices] GPU fixed batches=%llu program batches=%llu program vertices=%llu CPU declined batches=%llu\n",
            (unsigned long long)active->raw_draws,(unsigned long long)active->raw_programs,
            (unsigned long long)active->raw_program_vertices,(unsigned long long)active->raw_declined);
    const char *profile=getenv("CHIHIRO_VULKAN_PROFILE");
    if(active->profile.enabled && (active->frames==1 || active->frames%60==0)) {
        fprintf(stderr,"[Vulkan raw snapshots] compact_batches=%llu snapshot_bytes=%llu expanded_bytes=%llu\n",
            (unsigned long long)active->raw_compact_batches,(unsigned long long)active->raw_snapshot_bytes,(unsigned long long)active->raw_expanded_bytes);
        uint64_t draws=0,submits=0,depth_reads=0,color_reads=0,queued_uploads=0,queued_colors=0;
        uint64_t new_texture_copies=0;
        uint64_t depth_ops[8]={0},staging_skips=0,staging_bytes=0,pipeline_consecutive=0,pipeline_scans=0;
        for(unsigned i=0;i<active->count;i++) {ChVkTargetResourceStats stats;ch_vk_target_resource_stats(active->surfaces[i].target,&stats);for(unsigned op=0;op<8;op++) depth_ops[op]+=stats.depth_roundtrips_by_compare[op];new_texture_copies+=stats.combined_new_texture_copies;draws+=stats.batch_draws;submits+=stats.batch_submissions;depth_reads+=stats.combined_depth_readbacks;color_reads+=stats.combined_color_readbacks;queued_uploads+=stats.queued_depth_uploads;queued_colors+=stats.queued_color_readbacks;staging_skips+=stats.coherent_staging_skips;staging_bytes+=stats.staging_maintenance_bytes;pipeline_consecutive+=stats.pipeline_consecutive_hits;pipeline_scans+=stats.pipeline_scan_entries;}
        fprintf(stderr,"[Vulkan command batches] live targets: draws=%llu submissions=%llu combined_depth_reads=%llu combined_color_reads=%llu queued_depth_uploads=%llu queued_color_reads=%llu\n",(unsigned long long)draws,(unsigned long long)submits,(unsigned long long)depth_reads,(unsigned long long)color_reads,(unsigned long long)queued_uploads,(unsigned long long)queued_colors);
        fprintf(stderr,"[Vulkan combined new snapshots] live targets: copies=%llu\n",(unsigned long long)new_texture_copies);
        fprintf(stderr,"[Vulkan pipeline lookup] live targets: consecutive_hits=%llu scanned_entries=%llu\n",
            (unsigned long long)pipeline_consecutive,(unsigned long long)pipeline_scans);
        fprintf(stderr,"[Vulkan clear imports avoided] count=%llu bytes=%llu\n",
            (unsigned long long)active->clear_color_imports_avoided,(unsigned long long)active->clear_color_bytes_avoided);
        fprintf(stderr,"[Vulkan completion overlap] early_color_reads=%llu\n",(unsigned long long)active->early_color_reads);
        fprintf(stderr,"[Vulkan staging maintenance] live targets: coherent_skips=%llu explicit_bytes=%llu\n",
            (unsigned long long)staging_skips,(unsigned long long)staging_bytes);
        fprintf(stderr,"[Vulkan depth roundtrips] batched live targets NEVER/LESS/EQUAL/LEQUAL/GREATER/NOTEQUAL/GEQUAL/ALWAYS:");
        for(unsigned op=0;op<8;op++) fprintf(stderr," %llu",(unsigned long long)depth_ops[op]);
        fputc('\n',stderr);
    }
    if(active->zeta && active->profile.enabled && (active->frames==1 || active->frames%60==0)) {
        ChVkDepthResidentStats stats;ch_vk_depth_resident_stats(active->zeta,&stats);
        fprintf(stderr,"[Vulkan unchanged attachments] color_draws=%llu depth_draws=%llu\n",
            (unsigned long long)active->unchanged_color_draws,(unsigned long long)active->unchanged_depth_draws);
        fprintf(stderr,"[Vulkan snapshot copies avoided] color_bytes=%llu depth_bytes=%llu\n",
            (unsigned long long)active->color_snapshot_copy_bytes_saved,
            (unsigned long long)stats.snapshot_copy_bytes_saved);
        fprintf(stderr,"[Vulkan depth residency] binds=%llu imports=%llu writebacks=%llu CPU merges=%llu GPU copies=%llu clear skips=%llu clean reuses=%llu queued readbacks=%llu\n",
            (unsigned long long)stats.binds,(unsigned long long)stats.imports,
            (unsigned long long)stats.writebacks,(unsigned long long)stats.cpu_merges,(unsigned long long)stats.gpu_copies,
            (unsigned long long)stats.clear_skips,(unsigned long long)stats.clean_reuses,(unsigned long long)stats.queued_readbacks);
        fprintf(stderr,"[Vulkan CPU resident depth] layout_pixels=%llu float_pixels=%llu (cumulative)\n",
            (unsigned long long)stats.cpu_layout_pixels,(unsigned long long)stats.cpu_float_pixels);
        fprintf(stderr,"[Vulkan depth switches] same-image disjoint/overlap=%llu/%llu different-image disjoint/overlap=%llu/%llu dirty=%llu/%llu/%llu/%llu\n",
            (unsigned long long)stats.switches[0],(unsigned long long)stats.switches[1],
            (unsigned long long)stats.switches[2],(unsigned long long)stats.switches[3],
            (unsigned long long)stats.dirty_switches[0],(unsigned long long)stats.dirty_switches[1],
            (unsigned long long)stats.dirty_switches[2],(unsigned long long)stats.dirty_switches[3]);
    }
    if(active->textures && profile && atoi(profile) && (active->frames==1 || active->frames%60==0)) {
        ChVkTextureCacheStats stats;ch_vk_texture_cache_stats(active->textures,&stats);
        fprintf(stderr,"[Vulkan texture cache] hits=%llu uploads=%llu evictions=%llu live=%.1fMiB entries=%u/%u GPU copies/hits/reuses=%llu/%llu/%llu\n",
            (unsigned long long)stats.hits,(unsigned long long)stats.uploads,
            (unsigned long long)stats.evictions,stats.bytes/(1024.*1024.),stats.entries,stats.capacity,
            (unsigned long long)stats.gpu_copies,(unsigned long long)stats.gpu_hits,(unsigned long long)stats.gpu_reuses);
        fprintf(stderr,"[Vulkan texture lookup] hint_hits=%llu scanned_entries=%llu\n",
            (unsigned long long)stats.lookup_hint_hits,(unsigned long long)stats.lookup_scan_entries);
        fprintf(stderr,"[Vulkan texture comparisons] checks=%llu logical_bytes=%llu changed=%llu tracked=%llu reused=%llu GPU decoded=%llu CPU decoded pixels=%llu\n",
            (unsigned long long)stats.cpu_checks,(unsigned long long)stats.cpu_check_bytes,(unsigned long long)stats.cpu_changes,(unsigned long long)stats.tracked_checks,(unsigned long long)stats.cpu_reuses,(unsigned long long)stats.gpu_decodes,(unsigned long long)stats.cpu_decode_pixels);
    }
    if(profile && atoi(profile) && active->frames%60==0)
        fprintf(stderr,"[Vulkan CPU geometry] batches=%llu vertices=%llu (includes pretransformed input)\n",
            (unsigned long long)active->cpu_batches,(unsigned long long)active->cpu_vertices);
    if(profile && atoi(profile) && active->frames%60==0) {
        fprintf(stderr,"[Vulkan CPU strict depth] pixels=%llu (cumulative)\n",(unsigned long long)active->cpu_depth_pixels);
        fprintf(stderr,"[Vulkan CPU graphics] texture_pixels=%llu texture_ms=%.3f color_pixels=%llu color_ms=%.3f (cumulative)\n",
            (unsigned long long)active->cpu_texels,active->cpu_texture_us/1000.,
            (unsigned long long)active->cpu_color_pixels,active->cpu_color_us/1000.);
    }
    uint32_t presented=nv2a_backend_presented_va();
    if((active->host.present_rgba || active->host.present_bgra) && presented) {
        struct Surface *f=NULL;
        for(unsigned i=0;i<active->count;i++) if(active->surfaces[i].guest.color_va==presented) { f=&active->surfaces[i]; break; }
        if(!f) fatal(VK_ERROR_FEATURE_NOT_PRESENT,"unknown presented surface");
        VkResult r;
        if(f->color_bgra==1 && active->host.present_bgra) {
            r=load_surface_color(f,f->rgba);
            if(r==VK_SUCCESS) r=active->host.present_bgra(active->host.render_context,&f->guest,f->rgba,f->bytes);
        } else {
            r=ch_vk_nv2a_load_color_state(&f->guest,&f->state,f->rgba,f->bytes,active->host.guest_base,active->host.guest_bytes);
            if(r==VK_SUCCESS && active->host.present_rgba) r=active->host.present_rgba(active->host.render_context,&f->guest,f->rgba,f->bytes);
        }
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
    uint64_t start=active->costs.enabled?now_us():0,cpu=active->costs.enabled?thread_cpu_ns():0;
    queue_lock(active); flush(); queue_unlock(active);
    if(active->costs.enabled) {
        active->costs.completion_us+=now_us()-start;active->costs.completion_cpu_ns+=thread_cpu_ns()-cpu;
        active->costs.completion_calls++;
    }
}
static int locked_observe_raw(const Nv2aSurface *s,const Nv2aBatch *b) {
    queue_lock(active); int result=observe_raw(s,b); queue_unlock(active); return result;
}
static int locked_draw_raw(const Nv2aSurface *s,const Nv2aBatch *b) {
    if(active->host.observe_raw) return locked_observe_raw(s,b);
    if(!active->host.raw_fixed || active->host.render_draw || !b || !b->state) return 0;
    /* Snapshot encoded attributes and topology without holding the shared XR
     * queue mutex. Vertex shaders unpack the borrowed guest formats. */
    ChVkFixedGeometry fixed;
    uint64_t start=active->costs.enabled?now_us():0,cpu=active->costs.enabled?thread_cpu_ns():0;
    VkResult r=ch_vk_raw_prepare_reuse(b->raw,&fixed,&active->raw_scratch);
    if(active->costs.enabled) {
        active->costs.prepare_us+=now_us()-start;active->costs.prepare_cpu_ns+=thread_cpu_ns()-cpu;
    }
    if(r==VK_ERROR_FEATURE_NOT_PRESENT) {active->raw_declined++;return 0;}
    if(r!=VK_SUCCESS) fatal(r,"raw fixed attribute upload");
    queue_lock(active);draw_batch(s,b,&fixed);
    uint64_t expanded=(uint64_t)fixed.vertex_count*ch_vk_raw_vertex_stride(&fixed.params);
    active->raw_compact_batches+=fixed.params.stream_bytes!=0;
    active->raw_snapshot_bytes+=fixed.params.stream_bytes?fixed.params.stream_bytes:expanded;
    active->raw_expanded_bytes+=expanded;
    if(fixed.params.program_count) {active->raw_programs++;active->raw_program_vertices+=fixed.vertex_count;}
    else active->raw_draws++;
    queue_unlock(active);
    ch_vk_fixed_release(&fixed);return 1;
}
static Nv2aBackend callbacks={.clear=locked_clear,.draw=locked_draw,.flip=locked_flip};
const Nv2aBackend *ch_vk_backend_callbacks(void) { return &callbacks; }
VkResult ch_vk_backend_create(const ChVkBackendConfig *config,ChVkBackend **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;
    if(!config || !config->physical || !config->device || !config->queue ||
       !config->guest_base || !config->guest_bytes || !config->decode ||
       config->batch_draw_limit>CH_VK_MAX_BATCH_DRAWS ||
       (config->present_bgra && !config->present_rgba) ||
       (!!config->lock_queue != !!config->unlock_queue) ||
       (config->quantized_depth && !config->resident_depth) ||
       ((config->depth_quant_draw || config->depth_quant_inline || config->depth_quant_fragment) && !config->quantized_depth) ||
       ((!!config->depth_quant_draw + !!config->depth_quant_inline + !!config->depth_quant_fragment)>1) ||
       (config->sample_render_targets && (!config->resident_color || !config->cache_textures)) ||
       ((config->resident_color || config->resident_depth) && (config->render_clear || config->render_draw ||
        config->render_flip || config->guest_color_changed)))
        return VK_ERROR_INITIALIZATION_FAILED;
    ChVkBackend *b=calloc(1,sizeof(*b));
    if(!b) return VK_ERROR_OUT_OF_HOST_MEMORY;
    b->host=*config;b->pipeline_cache=config->pipeline_cache;
    const char *profile=getenv("CHIHIRO_VULKAN_PROFILE");
    const char *costs=getenv("CHIHIRO_VULKAN_FRAME_COSTS");
    b->costs.enabled=costs && atoi(costs)!=0;
    b->profile.enabled=(profile && atoi(profile)!=0) || b->costs.enabled;
    const char *frames=getenv("CHIHIRO_VULKAN_FRAME_PROFILE");
    b->frame_profile.enabled=b->profile.enabled || (frames && atoi(frames)!=0);
    if(config->resident_depth) {
        VkResult r=ch_vk_depth_resident_create(config->guest_base,config->guest_bytes,&b->zeta);
        if(r!=VK_SUCCESS) {free(b);return r;}
        ch_vk_depth_resident_write_watch(b->zeta,config->guest_write_watch);
        ch_vk_depth_resident_gpu_layout(b->zeta,config->gpu_morton_depth && config->gpu_depth_pack);
        r=ch_vk_depth_resident_packed(b->zeta,config->gpu_depth_pack && !config->depth_float_transfers);
        if(r!=VK_SUCCESS) {ch_vk_depth_resident_destroy(b->zeta);free(b);return r;}
        ch_vk_depth_resident_gpu_copies(b->zeta,!config->strict_depth_switches);
        ch_vk_depth_resident_reuse_clean(b->zeta,config->reuse_clean_depth);
        ch_vk_depth_resident_multiple(b->zeta,config->multiple_depth);
    }
    if(config->cache_textures) {
        ChVkTextureCacheConfig texture_config={.physical=config->physical,.device=config->device,
            .queue=config->queue,.family=config->family,.guest_base=config->guest_base,
            .guest_bytes=config->guest_bytes,.size_aware_eviction=config->size_aware_texture_eviction,.decode=decode_cpu_texture,.max_entries=config->texture_cache_entries,
            .max_bytes=config->texture_cache_bytes,.queued_uploads=1,.reuse_uploads=1,
            .gpu_decode=config->gpu_texture_decode,.pipeline_cache=config->pipeline_cache,
            .guest_write_watch=config->strict_texture_checks?NULL:config->guest_write_watch};
        VkResult r=ch_vk_texture_cache_create(&texture_config,&b->textures);
        if(r!=VK_SUCCESS) {ch_vk_depth_resident_destroy(b->zeta);free(b);return r;}
    }
    *out=b; return VK_SUCCESS;
}
VkResult ch_vk_backend_attach(ChVkBackend *b) {
    if(!b || active) return VK_ERROR_INITIALIZATION_FAILED;
    callbacks.draw_raw=(b->host.observe_raw || b->host.raw_fixed)?locked_draw_raw:NULL;
    /* Native Vulkan transforms/clips on the GPU and never reads CPU extents.
     * Keep legacy metadata for observer/routing configurations. */
    callbacks.raw_no_extent=b->host.raw_fixed && !b->host.observe_raw && !b->host.render_draw;
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
    if(b->pipeline_cache && !b->host.pipeline_cache) vkDestroyPipelineCache(b->host.device,b->pipeline_cache,NULL);
    ch_vk_texture_cache_destroy(b->textures);
    ch_vk_fixed_scratch_release(&b->raw_scratch);
    ch_vk_depth_resident_destroy(b->zeta);
    queue_unlock(b);
    free(b);
}
