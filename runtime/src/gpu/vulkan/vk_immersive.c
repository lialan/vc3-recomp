#include "vk_immersive.h"
#include "vk_state.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define IMM_SURFACES 8
/* 3D batches in a frame that make a surface a scene, kept at full size: an
 * effect buffer gets a stray one (the mission 2 boss's feedback buffer: one
 * a frame), and growing its eyes rebuilt their pipelines (~130 ms). */
#define IMM_SCENE_BATCHES 8u
#define ZOOM_SLOTS 8
/* A screen-shaped guest surface's eyes (2*eye_width x eye_height). */
struct ImmSurface {
    uint32_t va,width,height;
    ChVkTarget *eyes; unsigned char *pixels;
    VrGameProj projection; int projection_ok;
    uint64_t serial;         /* the eyes' contents, for snapshots */
    /* Effect-only surfaces (never shown, no 3D: E.S.'s feedback buffer) get
     * eyes at half the size each way -- their full-screen passes were ~0.6 ms
     * each at native size; a blurred trail does not need native detail.
     * div: the eyes' current divisor; last_3d/last_shown: frames. */
    uint32_t div;uint64_t last_3d,last_shown;size_t pixels_bytes;
    uint32_t world,drawn,flat,hud,full;    /* this frame: 3D batches; any eye work; 2D flat/HUD/full-screen */
    uint32_t composite;    /* this frame: flat draws sampling eyes (the scene drawn elsewhere, composited in) */
};
struct ChVkImmersive {
    ChVkImmersiveConfig config;
    struct ImmSurface surfaces[IMM_SURFACES]; unsigned count;
    ChVkTarget *hud; unsigned char *hud_pixels; uint32_t hud_width,hud_height; int hud_used;
    Vc3VrView view; int on;          /* this frame */
    VrGameProj projection; int projection_ok; uint32_t previous_3d;
    uint32_t shown[4]; uint64_t shown_at[4];   /* recently presented surfaces, and when */
    struct {float k;uint32_t n;} zoom[ZOOM_SLOTS]; float kbase;
    /* per-batch projection cache: identical matrices, identical projection */
    Nv2aFixedState last_ff; float last_aa[2]; int last_valid,last_ok; VrGameProj last;
    int zoom_normalise; float zoom_max;   /* CHIHIRO_VULKAN_IMMERSIVE_ZOOM: 0 the true projection, 1 the OpenGL renderer's zoom_proj, 2 (default) follow zooms in */
    int half_effects;     /* CHIHIRO_VULKAN_IMMERSIVE_HALF_EFFECTS=0: every surface's eyes at native size */
    uint32_t effect_div;  /* CHIHIRO_VULKAN_IMMERSIVE_EFFECT_DIV: effect buffers' divisor each way (2; 4 a quarter) */
    uint64_t guest_eye_samples;
    int log; uint64_t frames,world_batches,hud_batches,flat_batches,skipped_batches;
    /* Dynamic resolution: the eyes drawn at k_levels[level] of their size. */
    int dynamic,level,went_finer; unsigned window,late,calm,calm_need,on_frames;
    uint64_t late_seen,level_since_ns;   /* the GPU-late count at the window's start */
    uint64_t trace_until; unsigned trace_left;   /* CHIHIRO_VULKAN_IMMERSIVE_TRACE (diagnostic) */
    struct ImmSurface *last_src; int last_hud; unsigned repeats;   /* the picture the last flip showed */
    /* The lens picture: its surface (read at each flip), drawn into since
     * the last flip; the log's count of frames handed over with it, a second */
    uint32_t lens_va; int lens_drawn; unsigned lens_fresh,lens_frames; uint64_t lens_log_ns;
    /* Its copy at config.lens_scale (lens_copy()): drawn into this frame;
     * draws without prepared GPU geometry, not copied (counted) */
    ChVkTarget *lens_copy; unsigned char *lens_pixels; uint32_t lens_w,lens_h; int lens_copied;
    uint64_t lens_skipped;
};
/* Dynamic resolution levels (fraction of each eye's width and height). A
 * scene heavier than the GPU's 16.7 ms (VC3's later areas at native 2160 on
 * the Steam Frame: 40 fps) steps down a level after half a second with 2+
 * late frames, two with 8+; a level up after calm_need quiet half-seconds
 * (3 s), doubled (to 32 s) when a step up is undone within 10 s. A late frame
 * is one the headset showed late because its GPU work was unfinished when it
 * was due (ch_vk_gpu_mailbox_gpu_late_count): what a lower resolution helps.
 * Frames the game itself makes late (loads, compiles, 30 fps scenes) do not
 * count, nor the first 2 s after the eyes turn on (a scene's first frames). (Gaps between emulated vblanks, as seen from this thread, counted
 * before: its own jitter behind the game made 2-7 a half second while the
 * headset showed every frame on time, holding native at 70-90%.)
 * CHIHIRO_VULKAN_IMMERSIVE_SCALE=k: fixed at the nearest level; =1: native
 * always. */
static const float k_levels[4]={1.f,.9f,.8f,.7f};
void ch_vk_target_set_foveation_level(int level);
uint64_t ch_vk_gpu_mailbox_gpu_late_count(void) __attribute__((weak));   /* absent in tools */
static uint64_t mono_ns(void) {struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return (uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec;}
static void scale_control(ChVkImmersive *m) {
    if(!ch_vk_gpu_mailbox_gpu_late_count) return;
    uint64_t total=ch_vk_gpu_mailbox_gpu_late_count();
    /* not on, or within 2 s of turning on (a new scene's first frames do
     * one-off GPU work): nothing to judge */
    if(!m->dynamic || !m->on) m->on_frames=0;
    if(!m->dynamic || !m->on || ++m->on_frames<=120) {m->late_seen=total;m->window=0;return;}
    uint64_t now=mono_ns();
    if(++m->window<30) return;
    m->late=(unsigned)(total-m->late_seen);m->late_seen=total;
    if(m->log && m->late) fprintf(stderr,"[immersive] %u GPU-late frames in %u at %.0f%%\n",m->late,m->window,k_levels[m->level]*100);
    int prev=m->level;
    if(m->late>=2 && m->level<3) {
        if(m->went_finer && now-m->level_since_ns<10000000000ull) m->calm_need=m->calm_need*2>64?64:m->calm_need*2;
        m->level+=m->late>=8 && m->level<2?2:1;m->calm=0;m->went_finer=0;
    } else if(m->late==0) {
        if(++m->calm>=m->calm_need && m->level>0) {m->level--;m->calm=0;m->went_finer=1;}
    } else m->calm=0;
    if(m->level!=prev) {
        m->level_since_ns=now;
        ch_vk_target_set_foveation_level(m->level);
        fprintf(stderr,"[immersive] render scale %.0f%% (%u late frames in %u)\n",k_levels[m->level]*100,m->late,m->window);
    } else if(now-m->level_since_ns>20000000000ull) m->calm_need=6;
    m->window=m->late=0;
}
static int screen_shaped(const Nv2aSurface *s) {
    return s && s->width && s->height && (uint64_t)s->width*3==(uint64_t)s->height*4;
}
static int shown(const ChVkImmersive *m,uint32_t va) {
    /* presented within the last few frames (triple buffering rotates) */
    for(unsigned i=0;va && i<4;i++) if(va==m->shown[i] && m->frames-m->shown_at[i]<=6) return 1;
    return 0;
}
static struct ImmSurface *eye_source(ChVkImmersive *m,const Nv2aTexture *tx) {
    if(!tx || tx->cube || (tx->color!=0x12 && tx->color!=0x1e)) return NULL;
    for(unsigned i=0;i<m->count;i++) {
        struct ImmSurface *f=&m->surfaces[i];
        if(f->eyes && f->serial && f->va==tx->offset && f->width==tx->width && f->height==tx->height) return f;
    }
    return NULL;
}
/* A flat draw sampling a surface that has eyes samples its eyes (each eye
 * its half: imm_sbs); everything else resolves as for the guest. */
struct ImmResolve { ChVkImmersive *m; ChVkSampleResolver next; void *context; ChVkTarget *reader; };
/* A lent image lives as long as its target: nothing to release, but a lease
 * with a release keeps the draw asynchronous. */
static void lend_release(void *context,ChVkTexture *image) {(void)context;(void)image;}
static VkResult imm_resolve(void *context,const Nv2aTexture *tx,ChVkSampleImage *out) {
    struct ImmResolve *c=context;
    struct ImmSurface *f=c->m->config.textures?eye_source(c->m,tx):NULL;
    if(f) {
        Nv2aTexture key=*tx;ch_vk_target_size(f->eyes,&key.width,&key.height);
        key.pitch=key.width*4;
        static int lend=-1;
        if(lend<0) {const char *e=getenv("CHIHIRO_VULKAN_IMMERSIVE_LEND");lend=!e || atoi(e)!=0;}
        if(lend && c->reader) {   /* in place: no copy of the eye image */
            VkSamplerCreateInfo sampler;ChVkTexture *image=NULL;
            VkResult r=ch_vk_sampler_state(&key,&sampler);
            if(r==VK_SUCCESS) r=ch_vk_target_lend_texture(f->eyes,c->reader,tx->color==0x1e || tx->color==0x07,&sampler,&image);
            if(r==VK_SUCCESS) {*out=(ChVkSampleImage){.width=key.width,.height=key.height,.gpu=image,.release=lend_release};return VK_SUCCESS;}
            if(r!=VK_NOT_READY) return r;
        }
        VkResult r=ch_vk_texture_cache_resolve_target(c->m->config.textures,&key,f->eyes,f->serial,out);
        if(r!=VK_NOT_READY) return r;
    }
    return c->next?c->next(c->context,tx,out):VK_NOT_READY;
}
static struct ImmSurface *find(ChVkImmersive *m,const Nv2aSurface *s) {
    for(unsigned i=0;i<m->count;i++) if(m->surfaces[i].va==s->color_va) {
        struct ImmSurface *f=&m->surfaces[i];
        if(f->width!=s->width || f->height!=s->height) {f->width=s->width;f->height=s->height;f->projection_ok=0;}
        return f;
    }
    if(m->count==IMM_SURFACES) return NULL;
    struct ImmSurface *f=&m->surfaces[m->count++];
    memset(f,0,sizeof *f);f->va=s->color_va;f->width=s->width;f->height=s->height;
    return f;
}
static VkResult configure(ChVkImmersive *m,ChVkTarget *t,unsigned char *pixels,size_t bytes) {
    VkResult r=ch_vk_target_pipeline_cache(t,m->config.pipeline_cache);
    if(r==VK_SUCCESS) r=ch_vk_target_batch_limit(t,m->config.batch_limit);
    if(r==VK_SUCCESS) r=ch_vk_target_batching(t,1);
    const float blank[4]={0,0,0,0};
    if(r==VK_SUCCESS) r=ch_vk_target_clear_read(t,blank,pixels,bytes);
    if(r==VK_SUCCESS) ch_vk_target_defer_color_readback(t,1);
    return r;
}
static VkResult eyes(ChVkImmersive *m,struct ImmSurface *f) {
    if(f->eyes) return VK_SUCCESS;
    /* A new surface not on screen starts as an effect buffer (half size):
     * full-size eyes made only to be halved at the flip cost a rebuild. */
    if(!f->div) f->div=m->half_effects && !shown(m,f->va)?m->effect_div:1;
    uint32_t w=m->config.eye_width*2/f->div,h=m->config.eye_height/f->div;
    size_t bytes=(size_t)w*h*4;
    f->pixels=malloc(bytes); if(!f->pixels) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult r=ch_vk_target_create_depth(m->config.physical,m->config.device,m->config.queue,m->config.family,
        w,h,VK_FORMAT_D24_UNORM_S8_UINT,&f->eyes);
    if(r==VK_ERROR_FORMAT_NOT_SUPPORTED) r=ch_vk_target_create(m->config.physical,m->config.device,
        m->config.queue,m->config.family,w,h,&f->eyes);
    if(r==VK_SUCCESS) r=configure(m,f->eyes,f->pixels,bytes);
    if(r==VK_SUCCESS && m->log) fprintf(stderr,"[immersive] eyes for surface %08X %ux%u: %ux%u\n",f->va,f->width,f->height,w,h);
    f->pixels_bytes=bytes;
    return r;
}
static VkResult hud(ChVkImmersive *m,const Nv2aSurface *s) {
    if(m->hud && (m->hud_width!=s->width || m->hud_height!=s->height)) {
        ch_vk_target_destroy(m->hud);m->hud=NULL;free(m->hud_pixels);m->hud_pixels=NULL;
    }
    size_t bytes=(size_t)s->width*s->height*4;
    if(!m->hud) {
        m->hud_pixels=malloc(bytes); if(!m->hud_pixels) return VK_ERROR_OUT_OF_HOST_MEMORY;
        VkResult r=ch_vk_target_create(m->config.physical,m->config.device,m->config.queue,m->config.family,
            s->width,s->height,&m->hud);
        if(r==VK_SUCCESS) r=configure(m,m->hud,m->hud_pixels,bytes);
        if(r!=VK_SUCCESS) return r;
        m->hud_width=s->width;m->hud_height=s->height;
    }
    if(!m->hud_used) {
        Nv2aSurface extent=*s;
        VkResult r=ch_vk_nv2a_clear_read(m->hud,&extent,0xf0,0,0,m->hud_pixels,bytes);
        if(r!=VK_SUCCESS) return r;
        m->hud_used=1;
    }
    return VK_SUCCESS;
}
/* The lens picture at config.lens_scale: a host-only copy of the lens
 * surface that its clears and draws go to as well while immersive (as the
 * 16:9 copies, vk_wide.c: the guest's clip position as drawn, mode 3, into
 * the bigger target), handed to the host instead of the guest's target.
 * The guest's own is still drawn: the game samples it. NULL: not wanted
 * (scale <= 1, not the lens surface, not immersive). */
static int lens_copied_surface(const ChVkImmersive *m,const Nv2aSurface *s) {
    return m && m->on && s && s->width && s->height && m->lens_va && s->color_va==m->lens_va &&
        m->config.lens_scale>1.0f && m->config.lens_scale<=4.0f;
}
static VkResult lens_copy(ChVkImmersive *m,const Nv2aSurface *s) {
    const uint32_t w=(uint32_t)lroundf(s->width*m->config.lens_scale),h=(uint32_t)lroundf(s->height*m->config.lens_scale);
    if(m->lens_copy && m->lens_w==w && m->lens_h==h) return VK_SUCCESS;
    if(m->lens_copy) {
        VkResult r=ch_vk_target_finish(m->lens_copy);if(r!=VK_SUCCESS) return r;
        if(m->config.textures) ch_vk_texture_cache_forget_source(m->config.textures,m->lens_copy);
        ch_vk_target_destroy(m->lens_copy);m->lens_copy=NULL;free(m->lens_pixels);m->lens_pixels=NULL;
    }
    m->lens_pixels=malloc((size_t)w*h*4); if(!m->lens_pixels) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult r=ch_vk_target_create_depth(m->config.physical,m->config.device,m->config.queue,m->config.family,
        w,h,VK_FORMAT_D24_UNORM_S8_UINT,&m->lens_copy);
    if(r==VK_ERROR_FORMAT_NOT_SUPPORTED) r=ch_vk_target_create(m->config.physical,m->config.device,
        m->config.queue,m->config.family,w,h,&m->lens_copy);
    if(r==VK_SUCCESS) r=configure(m,m->lens_copy,m->lens_pixels,(size_t)w*h*4);
    if(r!=VK_SUCCESS) return r;
    m->lens_w=w;m->lens_h=h;
    fprintf(stderr,"[immersive] lens surface %08X %ux%u drawn at %ux%u\n",s->color_va,s->width,s->height,w,h);
    return VK_SUCCESS;
}
VkResult ch_vk_immersive_create(const ChVkImmersiveConfig *config,ChVkImmersive **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;
    if(!config || !config->physical || !config->device || !config->queue || !config->eye_width ||
       !config->eye_height || config->eye_width>4096 || config->eye_height>4096) return VK_ERROR_INITIALIZATION_FAILED;
    ChVkImmersive *m=calloc(1,sizeof *m); if(!m) return VK_ERROR_OUT_OF_HOST_MEMORY;
    m->config=*config;
    /* Each batch is a render pass, and on a tiler each pass loads and stores
     * the whole eye image: the eyes take the largest batches, 512 draws
     * (mission 2's first area, ~500 eye draws a frame: 1.4 -> 0.4 batch
     * ends a frame at the limit, tiled -7% GPU, the Frame's direct rendering
     * -3.5%, against 256; diagnostic override CHIHIRO_VULKAN_IMMERSIVE_BATCH). */
    {const char *b=getenv("CHIHIRO_VULKAN_IMMERSIVE_BATCH");
     m->config.batch_limit=b && atoi(b)>0 && atoi(b)<=(int)CH_VK_MAX_BATCH_DRAWS?(uint32_t)atoi(b):512;}
    const char *e=getenv("CHIHIRO_VULKAN_IMMERSIVE_LOG");
    m->log=e?atoi(e):0;
    /* 2 (follow) by default: a game zoom in (a far enemy, E.S., the death
     * close-up) zooms the headset's world as much (zoomed()); 0 recovers
     * each vertex where it is (no zoom); 1 normalises to the most used
     * lens, which drifted to the close-up's after a long continue. */
    e=getenv("CHIHIRO_VULKAN_IMMERSIVE_ZOOM");
    m->zoom_normalise=e?atoi(e):2;
    e=getenv("CHIHIRO_VULKAN_IMMERSIVE_ZOOM_MAX");
    /* No limit by default: the game's lens goes to ~4.6x (a far enemy, the
     * death close-up), and a view that magnifies less shows its targets
     * smaller than the game judges a hit by -- shots on them missed. The
     * extra GPU cost (textures a mip level sharper from 2x on) is what the
     * dynamic resolution absorbs. n >= 1: at most n (1.75 held E.S. to 60
     * before the dynamic resolution). */
    m->zoom_max=e?(float)atof(e):0.0f;
    e=getenv("CHIHIRO_VULKAN_IMMERSIVE_HALF_EFFECTS");
    m->half_effects=!e || atoi(e);
    e=getenv("CHIHIRO_VULKAN_IMMERSIVE_EFFECT_DIV");
    m->effect_div=e && (atoi(e)==4 || atoi(e)==2)?(uint32_t)atoi(e):2;
    m->dynamic=1;m->calm_need=6;
    e=getenv("CHIHIRO_VULKAN_IMMERSIVE_SCALE");
    if(e && atof(e)>0) {
        float k=(float)atof(e);m->dynamic=0;
        for(int i=0;i<4;i++) if(fabsf(k_levels[i]-k)<fabsf(k_levels[m->level]-k)) m->level=i;
        ch_vk_target_set_foveation_level(m->level);
    }
    *out=m; return VK_SUCCESS;
}
void ch_vk_immersive_destroy(ChVkImmersive *m) {
    if(!m) return;
    for(unsigned i=0;i<m->count;i++) {ch_vk_target_destroy(m->surfaces[i].eyes);free(m->surfaces[i].pixels);}
    ch_vk_target_destroy(m->hud);free(m->hud_pixels);
    ch_vk_target_destroy(m->lens_copy);free(m->lens_pixels);free(m);
}
int ch_vk_immersive_on(const ChVkImmersive *m) { return m && m->on; }
VkResult ch_vk_immersive_clear(ChVkImmersive *m,const Nv2aSurface *s,const Nv2aRenderState *rs,
    uint32_t flags,uint32_t argb,uint32_t zs) {
    if(rs && lens_copied_surface(m,s)) {   /* a clear rectangle: the same part of the copy */
        VkResult r=lens_copy(m,s);
        if(r==VK_SUCCESS) r=ch_vk_nv2a_clear_panel_read(m->lens_copy,s,rs,flags,argb,zs,m->lens_pixels,(size_t)m->lens_w*m->lens_h*4);
        if(r!=VK_SUCCESS) return r;
        m->lens_copied=1;
    }
    if(!m || !m->on || !screen_shaped(s) || !rs) return VK_SUCCESS;
    struct ImmSurface *f=find(m,s); if(!f) return VK_SUCCESS;
    VkResult r=eyes(m,f); if(r!=VK_SUCCESS) return r;
    f->serial++;
    /* A guest clear rectangle does not map onto the re-projected eyes: the
     * whole eye image, with the clear's color and depth. */
    Nv2aSurface extent=*s; ch_vk_target_size(f->eyes,&extent.width,&extent.height);
    f->drawn++;
    return ch_vk_nv2a_clear_read(f->eyes,&extent,flags,argb,zs,f->pixels,(size_t)extent.width*extent.height*4);
}
/* The game's projection as the eyes use it; with zoom_normalise, the
 * baseline (most used) focal length, this one's centre and aspect (the
 * OpenGL renderer's zoom_proj). */
static VrGameProj zoomed(const ChVkImmersive *m,const VrGameProj *g,uint32_t width) {
    VrGameProj o=*g;
    if(m->zoom_normalise==1 && m->kbase>0 && fabsf(g->kx)>1) {float z=m->kbase/fabsf(g->kx);o.kx*=z;o.ky*=z;}
    /* Follow (the default): read back through VC3's usual 45-degree lens,
     * so a longer one -- the zoom on a far enemy, E.S., the death close-up
     * (~14 degrees) -- magnifies the world by as much; never zoomed out.
     * The host's projection is this one: aim and the HUD panel follow. */
    if(m->zoom_normalise==2 && width) {
        float k=(m->config.lens_k>0.0f?m->config.lens_k:772.5f)*(float)width/640.0f;   /* the game's usual lens */
        if(m->zoom_max>=1 && fabsf(g->kx)>k*m->zoom_max) k=fabsf(g->kx)/m->zoom_max;   /* magnification at most zoom_max */
        if(fabsf(g->kx)>k) {float z=k/fabsf(g->kx);o.kx*=z;o.ky*=z;}
    }
    return o;
}
static void zoom_note(ChVkImmersive *m,float kx) {
    float a=fabsf(kx); int best=-1,empty=-1,i;
    for(i=0;i<ZOOM_SLOTS;i++) {
        if(m->zoom[i].n && fabsf(m->zoom[i].k-a)<=.01f*a) {m->zoom[i].n++;break;}
        if(!m->zoom[i].n && empty<0) empty=i;
    }
    if(i==ZOOM_SLOTS) {
        if(empty<0) {   /* full: the least used slot goes */
            empty=0;for(i=1;i<ZOOM_SLOTS;i++) if(m->zoom[i].n<m->zoom[empty].n) empty=i;
        }
        m->zoom[empty].k=a;m->zoom[empty].n=1;
    }
    for(i=0;i<ZOOM_SLOTS;i++) if(m->zoom[i].n && (best<0 || m->zoom[i].n>m->zoom[best].n)) best=i;
    if(best>=0) m->kbase=m->zoom[best].k;
}
static int batch_projection(ChVkImmersive *m,const Nv2aBatch *b,VrGameProj *out) {
    const Nv2aRawGeometry *raw=b->raw;
    if(raw->kind!=NV2A_RAW_FIXED || !raw->ff) return 0;
    if(!m->last_valid || m->last_aa[0]!=raw->aa_sx || m->last_aa[1]!=raw->aa_sy || memcmp(&m->last_ff,raw->ff,sizeof m->last_ff)) {
        m->last_ff=*raw->ff;m->last_aa[0]=raw->aa_sx;m->last_aa[1]=raw->aa_sy;m->last_valid=1;
        m->last_ok=ch_vk_nv2a_projection_from_raw_surface(b,&m->last)==VK_SUCCESS;
    }
    if(m->last_ok) *out=m->last;
    return m->last_ok;
}
static struct ImmSurface *eye_source(ChVkImmersive *,const Nv2aTexture *);
/* Clip w depends on the position (a perspective transform), not the
 * orthographic 0,0,0,1 of real 2D. */
static int perspective_w(const Nv2aFixedState *ff) {
    const float *m=ff->use_composite?ff->composite:ff->proj;
    float s=fabsf(m[12])+fabsf(m[13])+fabsf(m[14]);
    return isfinite(s) && s>1e-4f*(fabsf(m[15])+1e-6f);
}
static int full_screen(ChVkImmersive *m,const Nv2aBatch *b,const Nv2aSurface *s) {
    Nv2aRawExtent e;
    if(!b->raw->extent || !b->raw->extent(b->raw,&e)) {
        /* No extent (a big screen-space mesh, not transformed on the CPU):
         * one that samples a surface with eyes is a composite of the scene
         * (E.S. distortion), drawn across the eyes; anything else the HUD. */
        if(b->texture && eye_source(m,b->texture)) return 1;
        for(unsigned i=0;i<4;i++) if(b->stage[i] && eye_source(m,b->stage[i])) return 1;
        return 0;
    }
    return e.x1-e.x0>=.9f*s->width && e.y1-e.y0>=.9f*s->height;
}
/* A draw sampling a screen-shaped surface: a picture of the screen, whose
 * picture is in its eyes -- its own, flat, is not drawn while immersive.
 * The mission 2 boss's heat haze draws ~40 soft blobs a frame that each
 * redraw the screen under them (stage 1: the surface being drawn); on the
 * HUD they showed that surface's last flat picture -- the route map before
 * the fight -- in front of the world for the whole fight. */
static int samples_screen(const ChVkImmersive *m,const Nv2aBatch *b) {
    for(unsigned k=0;k<5;k++) {
        const Nv2aTexture *tx=k<4?b->stage[k]:b->texture;
        if(tx) for(unsigned i=0;i<m->count;i++) if(m->surfaces[i].va==tx->offset) return 1;
    }
    return 0;
}
/* CHIHIRO_VULKAN_IMMERSIVE_TRACE=N (diagnostic): from immersive frame N,
 * every draw of the 3 frames after a switch away from the eyes (40 such
 * frames at most): where it went and what it sampled.
 * CHIHIRO_VULKAN_IMMERSIVE_TRACE_AT=N: every draw of frames N and N+1. */
static void trace_draw(const ChVkImmersive *m,const Nv2aSurface *s,const Nv2aBatch *b,int is3d) {
    if(m->frames>m->trace_until || !m->trace_until) return;
    Nv2aRawExtent e;int ok=b->raw && b->raw->extent && b->raw->extent(b->raw,&e);
    const Nv2aTexture *tx=b->texture?b->texture:b->stage[0];
    int rt=tx && m->config.guest_target && m->config.guest_target(m->config.guest_context,tx->offset);   /* the game renders into it */
    fprintf(stderr,"[imm trace] f%llu on %d into %08X %ux%u 3d %d %s w0 %g idx %u ext %.0f..%.0f x %.0f..%.0f tex %08X %ux%u fmt %02X rt %d st1 %08X blend %u %04X/%04X mask %08X depth %u/%04X write %u query %d\n",
        (unsigned long long)m->frames,m->on,s->color_va,s->width,s->height,is3d,
        b->raw?b->raw->kind==NV2A_RAW_FIXED?"fixed":"prog":"none",b->raw?b->raw->w0:0,b->raw?b->raw->index_count:0,
        ok?e.x0:-1,ok?e.x1:-1,ok?e.y0:-1,ok?e.y1:-1,tx?tx->offset:0,tx?tx->width:0,tx?tx->height:0,tx?tx->color:0,rt,
        b->stage[1]?b->stage[1]->offset:0,b->state->blend_enable,b->state->blend_src,b->state->blend_dst,b->state->color_mask,
        b->state->depth_test_enable,b->state->depth_func,b->state->depth_write,b->zpass_out!=NULL);
}
VkResult ch_vk_immersive_draw(ChVkImmersive *m,const Nv2aSurface *s,const Nv2aBatch *b,
    const ChVkFixedGeometry *fixed,ChVkSampleResolver resolve,void *context) {
    if(m && s && m->lens_va && s->color_va==m->lens_va) m->lens_drawn=1;
    if(b && b->state && b->raw && lens_copied_surface(m,s)) {
        if(!fixed) m->lens_skipped++;
        else {
            VkResult r=lens_copy(m,s);if(r!=VK_SUCCESS) return r;
            Nv2aBatch copy=*b;copy.zpass_out=NULL;
            const ChVkImmDraw imm={.mode=3,.scale=1};   /* the clip position as drawn */
            r=ch_vk_nv2a_draw_immersive_read(m->lens_copy,s,&copy,fixed,&imm,m->config.decode,resolve,context,
                m->lens_pixels,(size_t)m->lens_w*m->lens_h*4);
            if(r!=VK_SUCCESS) return r;
            m->lens_copied=1;
        }
    }
    if(m && s && b && b->state) {
        /* CHIHIRO_VULKAN_IMMERSIVE_TRACE_TEX=va (diagnostic): every draw of
         * the 3 frames from the first draw sampling that texture */
        static uint32_t trace_tex=1;
        if(trace_tex==1) {const char *e=getenv("CHIHIRO_VULKAN_IMMERSIVE_TRACE_TEX");trace_tex=e?(uint32_t)strtoul(e,NULL,16):0;}
        if(trace_tex && !m->trace_until && ((b->texture && b->texture->offset==trace_tex) ||
           (b->stage[0] && b->stage[0]->offset==trace_tex))) m->trace_until=m->frames+2;
    }
    if(m && s && b && b->state && !screen_shaped(s)) trace_draw(m,s,b,-1);
    if(!m || !s || !b || !b->state || !screen_shaped(s)) return VK_SUCCESS;
    if(b->zpass_out) return VK_SUCCESS;   /* visibility queries are the guest's */
    struct ImmSurface *f=NULL;
    int is3d=0; VrGameProj g={0};
    if(b->raw) {
        if(b->raw->kind==NV2A_RAW_FIXED) {
            is3d=batch_projection(m,b,&g);
            if(is3d) {
                f=find(m,s);
                if(f) {f->projection=g;f->projection_ok=1;}
            } else if(b->raw->ff && perspective_w(b->raw->ff)) {
                /* World-space vertices under a combined view-projection (the
                 * model-view the identity): no camera projection of their
                 * own, but on screen through the camera's, as program
                 * batches are -- not 2D. Flat, their depth-only passes put
                 * screen-shaped depth in front of the world. */
                f=find(m,s);
                if(f && f->projection_ok) {g=f->projection;is3d=1;}
                else if(m->projection_ok) {g=m->projection;is3d=1;}
            }
        } else if(b->raw->w0!=1.0f && b->raw->w0==b->raw->w0) {
            /* A program batch: its surface's newest fixed projection, else
             * the frame's (as the OpenGL renderer does), else the game's
             * own (a game with no fixed-function 3D), sized to the surface. */
            f=find(m,s);
            if(f && f->projection_ok) {g=f->projection;is3d=1;}
            else if(m->projection_ok) {g=m->projection;is3d=1;}
            else if(m->config.program_projection.kx!=0.0f && f) {
                const float sx=(float)s->width/640.0f,sy=(float)s->height/480.0f;
                g=m->config.program_projection;g.kx*=sx;g.cx*=sx;g.ky*=sy;g.cy*=sy;
                f->projection=g;f->projection_ok=1;is3d=1;
            }
        }
    }
    if(is3d) {
        if(!f) f=find(m,s);
        if(f) f->world++;
    }
    if(is3d && m->log>1 && b->raw && b->raw->kind==NV2A_RAW_FIXED && b->raw->ff && m->frames%60==0) {
        const float *mv=b->raw->ff->mv[0];
        static uint64_t lf;static unsigned nl;
        if(lf!=m->frames) {lf=m->frames;nl=0;}
        if(nl++<3) {   /* the camera: -R^T t (row-major, column vectors) */
            float c[3];
            for(int k=0;k<3;k++) c[k]=-(mv[k]*mv[3]+mv[4+k]*mv[7]+mv[8+k]*mv[11]);
            fprintf(stderr,"[immersive camera] frame %llu at %.2f %.2f %.2f up-row %.2f %.2f %.2f\n",
                (unsigned long long)m->frames,c[0],c[1],c[2],mv[4],mv[5],mv[6]);
        }
    } else if(is3d && m->log>1 && b->raw && b->raw->kind!=NV2A_RAW_FIXED && m->frames%10==0) {
        static uint64_t lp;static unsigned np;
        if(lp!=m->frames) {lp=m->frames;np=0;}
        Nv2aRawExtent e;
        if(b->raw->w0>1 && b->raw->extent && b->raw->extent(b->raw,&e) && m->projection_ok)
            fprintf(stderr,"[immersive height] w %.2f extent %.0f x %.0f px: %.2f x %.2f units\n",b->raw->w0,e.x1-e.x0,e.y1-e.y0,
                (e.x1-e.x0)*b->raw->w0/fabsf(m->projection.kx),(e.y1-e.y0)*b->raw->w0/fabsf(m->projection.ky));
        (void)np;
    }
    trace_draw(m,s,b,is3d);
    if(!m->on) return VK_SUCCESS;
    if(!fixed) {m->skipped_batches++;return VK_SUCCESS;}
    Nv2aBatch copy=*b; copy.zpass_out=NULL;
    int full=!is3d && full_screen(m,b,s);
    if(!is3d) {if(!f) f=find(m,s);if(f) f->full+=full;}
    if(!is3d && shown(m,s->color_va) && !full) {
        /* a screen effect over part of the screen (samples_screen): left
         * out -- in front of the world it would be a stale flat picture */
        if(samples_screen(m,b)) {m->skipped_batches++;return VK_SUCCESS;}
        if(f) f->hud++;
        /* The HUD has color only: depth and stencil cannot reject it. */
        VkResult r=hud(m,s); if(r!=VK_SUCCESS) return r;
        Nv2aRenderState state=*b->state;
        state.depth_test_enable=state.depth_write=state.stencil_enable=0;
        copy.state=&state;
        m->hud_batches++;
        if(m->log) {   /* a big 2D draw on the HUD covers the world behind it */
            Nv2aRawExtent e;int ok=b->raw->extent && b->raw->extent(b->raw,&e);
            float area=ok?(e.x1-e.x0)*(e.y1-e.y0)/((float)s->width*s->height):-1;
            if(!ok || area>.2f)
                fprintf(stderr,"[immersive big hud] frame %llu into %08X %s w0 %g idx %u extent %.0f..%.0f x %.0f..%.0f (%.0f%%) blend %u %04X/%04X mask %08X alpha %u tex %08X %ux%u fmt %02X\n",
                    (unsigned long long)m->frames,s->color_va,b->raw->kind==NV2A_RAW_FIXED?"fixed":"prog",b->raw->w0,b->raw->index_count,
                    ok?e.x0:0,ok?e.x1:0,ok?e.y0:0,ok?e.y1:0,area*100,state.blend_enable,state.blend_src,state.blend_dst,state.color_mask,
                    state.alpha_test_enable,b->texture?b->texture->offset:0,b->texture?b->texture->width:0,b->texture?b->texture->height:0,
                    b->texture?b->texture->color:0);
        }
        return ch_vk_nv2a_draw_fixed_hud_read(m->hud,s,&copy,fixed,m->config.decode,resolve,context,
            m->hud_pixels,(size_t)m->hud_width*m->hud_height*4);
    }
    if(!f) f=find(m,s);
    if(!f) {m->skipped_batches++;return VK_SUCCESS;}
    VkResult r=eyes(m,f); if(r!=VK_SUCCESS) return r;
    ChVkImmDraw imm={.mode=is3d?1u:2u,.scale=k_levels[m->level]};
    if(!is3d && m->config.textures) {
        if(b->texture && eye_source(m,b->texture)) imm.sbs|=16;
        for(unsigned i=0;i<4;i++) if(b->stage[i] && eye_source(m,b->stage[i])) imm.sbs|=1u<<i;
        if(imm.sbs) f->composite++;
    }
    static int screen_tex=-1;   /* CHIHIRO_VULKAN_IMMERSIVE_SCREEN_TEX=0: the game's coordinates, as before */
    if(screen_tex<0) {const char *e=getenv("CHIHIRO_VULKAN_IMMERSIVE_SCREEN_TEX");screen_tex=!e || atoi(e)!=0;}
    if(is3d && screen_tex && m->config.textures) {   /* 3D sampling a surface with eyes: per eye (immersive.glsl) */
        static unsigned n3;
        for(unsigned i=0;i<5;i++) {
            const Nv2aTexture *tx=i<4?b->stage[i]:b->texture;
            struct ImmSurface *src=tx?eye_source(m,tx):NULL;
            if(src) imm.sbs|=1u<<(8+i);
            if(!src || !m->log || n3>=200) continue;
            if(n3++<200) {
                Nv2aRawExtent e;int ok=b->raw->extent && b->raw->extent(b->raw,&e);
                fprintf(stderr,"[immersive 3D eyes-sampled] frame %llu into %08X stage %u from %08X %ux%u fmt %02X div %u: xy %.0f..%.0f %.0f..%.0f uv %.2f..%.2f %.2f..%.2f %s idx %u blend %u %04X/%04X\n",
                    (unsigned long long)m->frames,s->color_va,i,tx->offset,tx->width,tx->height,tx->color,src->div,
                    ok?e.x0:-1,ok?e.x1:-1,ok?e.y0:-1,ok?e.y1:-1,ok?e.u0:-1,ok?e.u1:-1,ok?e.v0:-1,ok?e.v1:-1,
                    b->raw->kind==NV2A_RAW_FIXED?"fixed":"prog",b->raw->index_count,b->state->blend_enable,b->state->blend_src,b->state->blend_dst);
            }
        }
    }
    if(is3d) {
        VrGameProj z=zoomed(m,&g,s->width);
        imm.gp[0]=z.kx;imm.gp[1]=z.ky;imm.gp[2]=z.cx;imm.gp[3]=z.cy;imm.gp[4]=z.inv_pz;
        m->world_batches++;
    } else {m->flat_batches++;f->flat++;}
    memcpy(imm.vp,m->view.vp,sizeof imm.vp);
    if(!is3d && m->log>2 && m->frames%30==0) {
        Nv2aRawExtent e;int ok=b->raw->extent && b->raw->extent(b->raw,&e);
        const Nv2aRenderState *st=b->state;
        if(b->raw->kind==NV2A_RAW_FIXED && b->raw->ff) {
            const Nv2aFixedState *ff=b->raw->ff;
            fprintf(stderr,"[immersive flat ff] usecomp %u skin %u proj row3 %g %g %g %g col2 %g %g %g %g comp row3 %g %g %g %g\n",
                ff->use_composite,ff->skin_mode,ff->proj[12],ff->proj[13],ff->proj[14],ff->proj[15],
                ff->proj[2],ff->proj[6],ff->proj[10],ff->proj[14],ff->composite[12],ff->composite[13],ff->composite[14],ff->composite[15]);
        } else fprintf(stderr,"[immersive flat prog] w0 %g\n",b->raw->w0);
        fprintf(stderr,"[immersive flat] frame %llu into %08X idx %u extent %s %.0f..%.0f x %.0f..%.0f blend %u %04X/%04X mask %08X depth %u alpha %u tex %08X/%02X st0 %08X/%02X st1 %08X st2 %08X st3 %08X sbs %x\n",
            (unsigned long long)m->frames,s->color_va,b->raw->index_count,ok?"":"(none)",ok?e.x0:0,ok?e.x1:0,ok?e.y0:0,ok?e.y1:0,
            st->blend_enable,st->blend_src,st->blend_dst,st->color_mask,st->depth_test_enable,st->alpha_test_enable,
            b->texture?b->texture->offset:0,b->texture?b->texture->color:0,b->stage[0]?b->stage[0]->offset:0,b->stage[0]?b->stage[0]->color:0,
            b->stage[1]?b->stage[1]->offset:0,b->stage[2]?b->stage[2]->offset:0,b->stage[3]?b->stage[3]->offset:0,imm.sbs);
    }
    if((imm.sbs&31u) && m->log>1 && m->frames%30==0) {
        Nv2aRawExtent e;int ok=b->raw->extent && b->raw->extent(b->raw,&e);
        const Nv2aTexture *tx=(imm.sbs&16)?b->texture:b->stage[__builtin_ctz(imm.sbs&15u)];
        fprintf(stderr,"[immersive eyes-sampled] frame %llu into %08X from %08X %ux%u fmt %02X: xy %.0f..%.0f %.0f..%.0f uv %.1f..%.1f %.1f..%.1f mask %x\n",
            (unsigned long long)m->frames,s->color_va,tx?tx->offset:0,tx?tx->width:0,tx?tx->height:0,tx?tx->color:0,
            ok?e.x0:-1,ok?e.x1:-1,ok?e.y0:-1,ok?e.y1:-1,ok?e.u0:-1,ok?e.u1:-1,ok?e.v0:-1,ok?e.v1:-1,imm.sbs);
    }
    f->drawn++;f->serial++;
    struct ImmResolve chain={m,resolve,context,f->eyes};
    return ch_vk_nv2a_draw_immersive_read(f->eyes,s,&copy,fixed,&imm,m->config.decode,imm_resolve,&chain,
        f->pixels,f->pixels_bytes);
}
static void dump_ppm_target(ChVkTarget *t,const char *path) {
    uint32_t w,h;ch_vk_target_size(t,&w,&h);
    unsigned char *px=malloc((size_t)w*h*4);
    FILE *f=px && ch_vk_target_read_color(t,px,(VkDeviceSize)w*h*4)==VK_SUCCESS?fopen(path,"wb"):NULL;
    if(f) {
        fprintf(f,"P6\n%u %u\n255\n",w,h);
        for(size_t i=0;i<(size_t)w*h;i++) fwrite(px+i*4,1,3,f);
        fclose(f);fprintf(stderr,"[immersive] dumped %s\n",path);
    }
    free(px);
}
/* CHIHIRO_VULKAN_IMMERSIVE_DUMP=dir (every CHIHIRO_VULKAN_IMMERSIVE_DUMP_EVERY
 * immersive frames, 300 by default, from immersive frame
 * CHIHIRO_VULKAN_IMMERSIVE_DUMP_FROM on): the scene surface's eyes and the
 * guest's own picture of it, the same frame, and the eyes the headset gets. */
static void dump_scene(ChVkImmersive *m,struct ImmSurface *scene,struct ImmSurface *shown) {
    static const char *dir;static unsigned every,n;static unsigned long long from;
    if(!dir) {dir=getenv("CHIHIRO_VULKAN_IMMERSIVE_DUMP");if(!dir) dir="";
        const char *e=getenv("CHIHIRO_VULKAN_IMMERSIVE_DUMP_EVERY");every=e && atoi(e)>0?(unsigned)atoi(e):300;
        e=getenv("CHIHIRO_VULKAN_IMMERSIVE_DUMP_FROM");from=e?strtoull(e,NULL,10):0;}   /* immersive frame */
    if(!*dir || !scene || !scene->eyes || !m->config.guest_target || m->frames<from || ++n%every) return;
    ChVkTarget *guest=m->config.guest_target(m->config.guest_context,scene->va);
    char path[512];
    snprintf(path,sizeof path,"%s/scene-%05llu-eyes.ppm",dir,(unsigned long long)m->frames);dump_ppm_target(scene->eyes,path);
    if(guest) {snprintf(path,sizeof path,"%s/scene-%05llu-flat.ppm",dir,(unsigned long long)m->frames);dump_ppm_target(guest,path);}
    if(m->hud && m->hud_used) {snprintf(path,sizeof path,"%s/scene-%05llu-hud.ppm",dir,(unsigned long long)m->frames);dump_ppm_target(m->hud,path);}
    if(shown && shown->eyes) {snprintf(path,sizeof path,"%s/scene-%05llu-shown.ppm",dir,(unsigned long long)m->frames);dump_ppm_target(shown->eyes,path);}
}
int ch_vk_immersive_guest_draw(ChVkImmersive *m,ChVkTarget *t,const Nv2aSurface *s,const Nv2aBatch *b,
    const ChVkFixedGeometry *fixed,ChVkSampleResolver resolve,void *context,void *pixels,VkDeviceSize bytes,VkResult *result) {
    if(!m || !m->on || !fixed || !b || !b->raw || !m->config.textures) return 0;
    ChVkImmDraw imm={.mode=0,.scale=k_levels[m->level]};
    if(b->texture && eye_source(m,b->texture)) imm.sbs|=16;
    for(unsigned i=0;i<4;i++) if(b->stage[i] && eye_source(m,b->stage[i])) imm.sbs|=1u<<i;
    if(!imm.sbs) return 0;
    struct ImmResolve chain={m,resolve,context,t};
    *result=ch_vk_nv2a_draw_immersive_read(t,s,b,fixed,&imm,m->config.decode,imm_resolve,&chain,pixels,bytes);
    m->guest_eye_samples++;
    return 1;
}
VkResult ch_vk_immersive_flip(ChVkImmersive *m,uint32_t presented,const Vc3VrView *next,
    ChVkImmFrame *out,ChVkTarget **eyes_out,ChVkTarget **hud_out,ChVkTarget **lens_out) {
    if(!m || !out || !eyes_out || !hud_out || !lens_out) return VK_ERROR_INITIALIZATION_FAILED;
    memset(out,0,sizeof *out);*eyes_out=*hud_out=*lens_out=NULL;
    /* The frame's projection: the screen-shaped surface the most 3D went
     * into -- the scene. */
    uint32_t best=0,total=0; struct ImmSurface *shown_f=NULL,*scene=NULL;
    for(unsigned i=0;i<m->count;i++) {
        struct ImmSurface *f=&m->surfaces[i];
        total+=f->world;
        if(f->projection_ok && f->world>best) {best=f->world;m->projection=f->projection;m->projection_ok=1;scene=f;}
        if(f->va==presented) shown_f=f;
    }
    if(best) zoom_note(m,m->projection.kx);
    /* What the headset gets: VC3 draws its scene off-screen and composites
     * it onto the presented surface with one full-screen draw; only a few
     * effects go there as 3D (none, some frames) -- eyes composited from
     * eyes hold the scene. When the presented picture holds no scene but
     * the frame drew one (game over, name entry: the scene goes on behind a
     * 2D picture), the scene with the HUD over it. The screen only for a
     * frame without 3D: while immersive, screen-shaped surfaces are not
     * drawn flat, so the screen would be stale -- the next frame is flat.
     * Only full-size eyes: a half-size effect buffer presented is resized
     * below, in this flip (its eyes freed).
     * A flip that drew nothing on the presented surface repeats the last
     * picture, the eyes kept on: VC3 spreads a frame it cannot finish in a
     * vblank over several flips (the mission 2 boss: the scene in one, its
     * effects in the next, the composite onto the presented surface three
     * flips on). Dropping to the screen there made the next frame flat --
     * every screen-shaped draw drawn flat as well -- slower still: 10 fps.
     * At most 30 in a row (a long blank goes to the screen). */
    struct ImmSurface *src=NULL;
    /* (a clear alone does not count: VC3 clears the back buffer it draws three flips on) */
    int untouched=!shown_f || !(shown_f->world || shown_f->flat || shown_f->hud || shown_f->composite);
    int repeat=m->on && m->projection_ok && untouched && m->last_src && m->last_src->eyes && m->last_src->div<2 && m->repeats<30;
    if(repeat) src=m->last_src;
    else if(m->on && m->projection_ok) {
        if(shown_f && shown_f->eyes && shown_f->div<2 && (shown_f->world || shown_f->composite)) src=shown_f;
        else if(scene && scene->eyes && scene->div<2) src=scene;
    }
    if(m->on) dump_scene(m,scene,src);
    int hud=(repeat?m->last_hud:m->hud_used) && m->hud;
    /* Only a presented picture repeats: the scene buffer may be half redrawn. */
    int presented_src=src && src==shown_f;
    m->last_src=repeat || presented_src?src:NULL;m->last_hud=hud;m->repeats=repeat?m->repeats+1:0;
    if(src) {
        out->view=m->view;out->projection=zoomed(m,&m->projection,src->width);out->projection_ok=1;
        out->guest_width=src->width;out->guest_height=src->height;
        out->eye_width=m->config.eye_width;out->eye_height=m->config.eye_height;
        out->scale=k_levels[m->level];
        out->hud=hud;out->repeat=repeat;
        *eyes_out=src->eyes;*hud_out=hud?m->hud:NULL;
        if(m->log) {   /* a frame with no HUD drops the HUD layer: where its 2D went */
            static unsigned with,without,shown_lines;
            if(hud) with++; else without++;
            if(!hud && shown_lines<40) {
                shown_lines++;
                fprintf(stderr,"[immersive no hud] frame %llu (with %u, without %u) presented %08X:",
                    (unsigned long long)m->frames,with,without,presented);
                for(unsigned i=0;i<m->count;i++) {
                    const struct ImmSurface *f=&m->surfaces[i];
                    fprintf(stderr," %08X%s 3D %u flat %u hud %u full %u comp %u;",f->va,
                        f->va==presented?"*":shown(m,f->va)?"+":"",f->world,f->flat,f->hud,f->full,f->composite);
                }
                fputc('\n',stderr);
            }
            if(m->frames%600==0) fprintf(stderr,"[immersive hud frames] with %u without %u\n",with,without);
            static float pk[4];static unsigned changes,change_lines;
            const float nk[4]={out->projection.kx,out->projection.ky,out->projection.cx,out->projection.cy};
            int moved=0;for(int k=0;k<4;k++) if(fabsf(nk[k]-pk[k])>.005f*fmaxf(fabsf(pk[k]),1.f)) moved=1;
            if(moved) {
                changes++;
                if(change_lines++<80) fprintf(stderr,"[immersive proj] frame %llu (%u changes) kx %.1f->%.1f ky %.1f->%.1f cx %.1f->%.1f cy %.1f->%.1f scene %08X src %08X\n",
                    (unsigned long long)m->frames,changes,pk[0],nk[0],pk[1],nk[1],pk[2],nk[2],pk[3],nk[3],scene?scene->va:0,src->va);
                memcpy(pk,nk,sizeof pk);
            }
        }
    }
    /* The lens picture goes with a frame handed over only when the game drew
     * into it since the last flip: a frame without (Silent Scope's stage
     * intro, its continue screen: no scope pass) has none, and the host
     * shows dark glass rather than the last picture. */
    ChVkTarget *lens=!src || repeat || !m->lens_drawn?NULL:m->lens_copied?m->lens_copy:
        m->config.guest_target?m->config.guest_target(m->config.guest_context,m->lens_va):NULL;
    if(lens) {
        out->lens=1;ch_vk_target_size(lens,&out->lens_w,&out->lens_h);*lens_out=lens;
        /* the aim it was drawn with: the game's, as the flip comes in the
         * push buffer (after this frame's lens draws, before the next's) */
        if(!m->config.lens_aim || !m->config.lens_aim(&out->lens_aim)) memset(&out->lens_aim,0,sizeof out->lens_aim);
    }
    if(m->log && src && !repeat) {
        m->lens_frames++;m->lens_fresh+=out->lens;
        uint64_t now=mono_ns();
        if(now-m->lens_log_ns>=1000000000ull) {
            fprintf(stderr,"[immersive] frame %llu lens %08X: fresh in %u of %u frames (%ux%u, %llu draws not copied); "
                "drawn at yaw %.3f pitch %.3f deg, focal %.3f, guard %.2f\n",
                (unsigned long long)m->frames,m->lens_va,m->lens_fresh,m->lens_frames,out->lens_w,out->lens_h,
                (unsigned long long)m->lens_skipped,out->lens_aim.yaw*57.29578f,out->lens_aim.pitch*57.29578f,
                out->lens_aim.focal,out->lens_aim.guard);
            m->lens_log_ns=now;m->lens_fresh=m->lens_frames=0;
        }
    }
    if(m->lens_copy) {   /* as the HUD below: its work submitted, or finished when unused */
        VkResult r=m->lens_copied?ch_vk_target_submit(m->lens_copy):ch_vk_target_finish(m->lens_copy);
        if(r!=VK_SUCCESS) return r;
    }
    m->lens_drawn=m->lens_copied=0;
    m->lens_va=m->config.lens_surface?m->config.lens_surface():0;
    {   /* every switch between the eyes and the screen, and why */
        static int was=-1;
        int now=repeat?3:presented_src?1:src?2:0;
        static const char *const what[]={"screen","eyes","scene behind 2D","the last picture again"};
        if(m->log && now!=was)
            fprintf(stderr,"[immersive switch] frame %llu -> %s: on %d presented %08X (3D %u, composited %u, eyes %d) total 3D %u projection %d\n",
                (unsigned long long)m->frames,what[now],m->on,presented,shown_f?shown_f->world:0,
                shown_f?shown_f->composite:0,shown_f && shown_f->eyes,total,m->projection_ok);
        static long long trace_from=-2;
        if(trace_from==-2) {const char *e=getenv("CHIHIRO_VULKAN_IMMERSIVE_TRACE");trace_from=e?atoll(e):-1;m->trace_left=40;}
        if(trace_from>=0 && now!=1 && now!=was && m->frames>=(uint64_t)trace_from && m->trace_left) {
            m->trace_until=m->frames+3;m->trace_left--;
        }
        static long long trace_at=-2;
        if(trace_at==-2) {const char *e=getenv("CHIHIRO_VULKAN_IMMERSIVE_TRACE_AT");trace_at=e?atoll(e):-1;}
        if(trace_at>=0 && m->frames+1==(uint64_t)trace_at) m->trace_until=m->frames+2;
        if(m->frames<=m->trace_until && m->trace_until)
            fprintf(stderr,"[imm trace] f%llu flip presented %08X -> %s\n",(unsigned long long)m->frames,presented,what[now]);
        was=now;
    }
    m->frames++;
    if(m->log && m->frames%120==0)
        fprintf(stderr,"[immersive] frame %llu %s: 3D %u (presented %08X), batches world/hud/flat/skipped %llu/%llu/%llu/%llu,"
            " projection %s kx %.1f ky %.1f cx %.1f cy %.1f 1/pz %.4g zoom base %.1f\n",
            (unsigned long long)m->frames,m->on?"immersive":"flat",total,presented,
            (unsigned long long)m->world_batches,(unsigned long long)m->hud_batches,(unsigned long long)m->flat_batches,
            (unsigned long long)m->skipped_batches,m->projection_ok?"known":"unknown",
            m->projection.kx,m->projection.ky,m->projection.cx,m->projection.cy,m->projection.inv_pz,m->kbase);
    if(m->log && m->frames%120==0) for(unsigned i=0;i<m->count;i++) {
        const struct ImmSurface *f=&m->surfaces[i];
        fprintf(stderr,"[immersive]   surface %08X %ux%u%s: 3D %u, 2D flat %u hud %u (full-screen %u), eye work %u\n",
            f->va,f->width,f->height,f->va==presented?" presented":shown(m,f->va)?" shown":"",f->world,f->flat,f->hud,f->full,f->drawn);
    }
    /* The next frame: immersive when the host has a view and this frame had
     * 3D (a frame without -- a movie, a menu -- goes on the screen). */
    if(presented) {
        unsigned k=0;while(k<3 && m->shown[k]!=presented) k++;
        memmove(&m->shown[1],&m->shown[0],k*sizeof m->shown[0]);memmove(&m->shown_at[1],&m->shown_at[0],k*sizeof m->shown_at[0]);
        m->shown[0]=presented;m->shown_at[0]=m->frames;
    }
    /* Texture-cache pins last until a target retires its batches, which it
     * does when it is next used: submit this frame's eye work without a wait
     * (it retires next frame), and finish targets left unused (their work is
     * long done), so no pins pile up across frames. */
    for(unsigned i=0;i<m->count;i++) {
        struct ImmSurface *f=&m->surfaces[i];
        if(f->world>=IMM_SCENE_BATCHES) f->last_3d=m->frames;
        if(f->va==presented || shown(m,f->va)) f->last_shown=m->frames;
        if(!f->eyes) continue;
        /* Resize: effect-only for ~2 s -> half size; a scene or shown -> full. */
        uint32_t want=m->half_effects && m->frames-f->last_3d>120 && m->frames-f->last_shown>120?m->effect_div:1;
        if(want!=f->div) {
            VkResult r=ch_vk_target_finish(f->eyes);if(r!=VK_SUCCESS) return r;
            if(m->config.textures) ch_vk_texture_cache_forget_source(m->config.textures,f->eyes);
            ch_vk_target_destroy(f->eyes);f->eyes=NULL;free(f->pixels);f->pixels=NULL;
            f->div=want;f->serial=0;
            if(m->log) fprintf(stderr,"[immersive] surface %08X eyes now 1/%u size\n",f->va,want);
            continue;
        }
        VkResult r=f->drawn?ch_vk_target_submit(f->eyes):ch_vk_target_finish(f->eyes);
        if(r!=VK_SUCCESS) return r;
    }
    if(m->hud) {VkResult r=m->hud_used?ch_vk_target_submit(m->hud):ch_vk_target_finish(m->hud);if(r!=VK_SUCCESS) return r;}
    m->previous_3d=total;
    /* On while the frames draw 3D or show eyes: the flip with the composite
     * may hold no 3D of its own (drawn flips before, as above). */
    m->on=next && next->on && (total>0 || src) && m->projection_ok;
    if(m->on) m->view=*next;
    m->hud_used=0;
    for(unsigned i=0;i<m->count;i++) {struct ImmSurface *f=&m->surfaces[i];f->world=f->drawn=f->flat=f->hud=f->full=f->composite=0;}
    scale_control(m);   /* after this frame's eye work is submitted: the next frame's level */
    return VK_SUCCESS;
}
