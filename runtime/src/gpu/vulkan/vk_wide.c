#include "vk_wide.h"
#include "vk_state.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIDE_SURFACES 8
/* A full-screen texture at least this wide is a 4:3 picture (a menu
 * background, a movie); narrower, a small texture spread over the screen. */
#define WIDE_PICTURE_MIN_W 320u
/* A surface with 3D in the last few frames is in play: its full-screen
 * pictures (a sky drawn before the world) span the whole width. */
#define WIDE_PLAY_FRAMES 3u
struct WideSurface {
    uint32_t va,width,height;     /* the guest surface */
    ChVkTarget *target; unsigned char *pixels; size_t bytes;
    uint32_t tw,th;               /* the wide copy */
    float xs,xo;                  /* clip x for the guest's picture in the middle */
    uint64_t serial;              /* the copy's contents, for snapshots */
    uint64_t last_3d;             /* the frame 3D was last drawn here */
};
struct ChVkWide {
    ChVkWideConfig config;
    struct WideSurface surfaces[WIDE_SURFACES]; unsigned count;
    uint64_t frames,skipped,stretched,squeezed;
    unsigned char *shown; size_t shown_bytes;
    int log;
};
static int screen_shaped(const Nv2aSurface *s) {
    return s && s->width && s->height && (uint64_t)s->width*3==(uint64_t)s->height*4;
}
/* As tall as the surface, 16:9 rounded to an even width: 640x480 -> 854x480,
 * the guest's 640 columns at 107..746, whole pixels. */
static void wide_size(const Nv2aSurface *s,uint32_t *tw,uint32_t *th) {
    *th=s->height;
    *tw=((s->height*16+8)/9+1)&~1u;
    if(*tw<s->width) *tw=s->width;
}
static struct WideSurface *find(ChVkWide *w,const Nv2aSurface *s) {
    for(unsigned i=0;i<w->count;i++) if(w->surfaces[i].va==s->color_va) {
        struct WideSurface *f=&w->surfaces[i];
        if(f->width!=s->width || f->height!=s->height) {
            /* Another size at the address: a new copy at the next use. */
            ch_vk_target_destroy(f->target);free(f->pixels);
            memset(f,0,sizeof *f);f->va=s->color_va;
        }
        f->width=s->width;f->height=s->height;
        return f;
    }
    if(w->count==WIDE_SURFACES) return NULL;
    struct WideSurface *f=&w->surfaces[w->count++];
    memset(f,0,sizeof *f);f->va=s->color_va;f->width=s->width;f->height=s->height;
    return f;
}
static VkResult target(ChVkWide *w,struct WideSurface *f,const Nv2aSurface *s) {
    if(f->target) return VK_SUCCESS;
    wide_size(s,&f->tw,&f->th);
    f->xs=(float)s->width/(float)f->tw;f->xo=0;
    f->bytes=(size_t)f->tw*f->th*4;
    f->pixels=malloc(f->bytes); if(!f->pixels) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult r=ch_vk_target_create_depth(w->config.physical,w->config.device,w->config.queue,w->config.family,
        f->tw,f->th,VK_FORMAT_D24_UNORM_S8_UINT,&f->target);
    if(r==VK_ERROR_FORMAT_NOT_SUPPORTED) r=ch_vk_target_create(w->config.physical,w->config.device,
        w->config.queue,w->config.family,f->tw,f->th,&f->target);
    if(r==VK_SUCCESS) r=ch_vk_target_pipeline_cache(f->target,w->config.pipeline_cache);
    if(r==VK_SUCCESS) r=ch_vk_target_batch_limit(f->target,w->config.batch_limit);
    if(r==VK_SUCCESS) r=ch_vk_target_batching(f->target,1);
    const float blank[4]={0,0,0,1};
    if(r==VK_SUCCESS) r=ch_vk_target_clear_read(f->target,blank,f->pixels,f->bytes);
    if(r==VK_SUCCESS) ch_vk_target_defer_color_readback(f->target,1);
    if(r==VK_SUCCESS && w->log) fprintf(stderr,"[wide] copy of surface %08X %ux%u: %ux%u\n",f->va,f->width,f->height,f->tw,f->th);
    return r;
}
VkResult ch_vk_wide_create(const ChVkWideConfig *config,ChVkWide **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;
    if(!config || !config->physical || !config->device || !config->queue) return VK_ERROR_INITIALIZATION_FAILED;
    ChVkWide *w=calloc(1,sizeof *w); if(!w) return VK_ERROR_OUT_OF_HOST_MEMORY;
    w->config=*config;
    const char *e=getenv("CHIHIRO_VULKAN_WIDE_LOG");
    w->log=e?atoi(e):0;
    *out=w; return VK_SUCCESS;
}
void ch_vk_wide_destroy(ChVkWide *w) {
    if(!w) return;
    for(unsigned i=0;i<w->count;i++) {ch_vk_target_destroy(w->surfaces[i].target);free(w->surfaces[i].pixels);}
    free(w->shown);free(w);
}
VkResult ch_vk_wide_clear(ChVkWide *w,const Nv2aSurface *s,const Nv2aRenderState *rs,uint32_t flags,uint32_t argb,uint32_t zs) {
    if(!w || !screen_shaped(s) || !rs) return VK_SUCCESS;
    struct WideSurface *f=find(w,s); if(!f) return VK_SUCCESS;
    VkResult r=target(w,f,s); if(r!=VK_SUCCESS) return r;
    f->serial++;
    Nv2aSurface extent=*s;extent.width=f->tw;extent.height=f->th;
    /* The whole surface: the whole wide copy. A rectangle: the same pixels
     * of the guest's picture, in the middle. */
    if(!rs->clear_rect_set ||
       (rs->clear_rect_set==3 && !(rs->clear_rect_h&65535) && !(rs->clear_rect_v&65535) &&
        (rs->clear_rect_h>>16)+1>=s->width && (rs->clear_rect_v>>16)+1>=s->height))
        return ch_vk_nv2a_clear_read(f->target,&extent,flags,argb,zs,f->pixels,f->bytes);
    Nv2aRenderState moved=*rs;
    uint32_t off=(f->tw-(uint32_t)lroundf(f->xs*f->tw))/2;
    uint32_t x0=(rs->clear_rect_h&65535)+off,x1=(rs->clear_rect_h>>16)+off;
    if(x1>=f->tw) x1=f->tw-1;
    moved.clear_rect_h=(x1<<16)|(x0&65535);
    return ch_vk_nv2a_clear_panel_read(f->target,&extent,&moved,flags,argb,zs,f->pixels,f->bytes);
}
/* A perspective draw: fixed-function with a projection that divides by
 * depth, or a vertex program whose first w is not 1. */
static int perspective(const Nv2aBatch *b) {
    if(b->raw->kind==NV2A_RAW_FIXED) {
        const Nv2aFixedState *ff=b->raw->ff;
        if(!ff) return 0;
        const float *m=ff->use_composite?ff->composite:ff->proj;
        float s=fabsf(m[12])+fabsf(m[13])+fabsf(m[14]);
        return isfinite(s) && s>1e-4f*(fabsf(m[15])+1e-6f);
    }
    return b->raw->w0!=1.0f && b->raw->w0==b->raw->w0;
}
static struct WideSurface *wide_source(ChVkWide *w,const Nv2aTexture *tx) {
    if(!tx || tx->cube || (tx->color!=0x12 && tx->color!=0x1e)) return NULL;
    for(unsigned i=0;i<w->count;i++) {
        struct WideSurface *f=&w->surfaces[i];
        if(f->target && f->serial && f->va==tx->offset && f->width==tx->width && f->height==tx->height) return f;
    }
    return NULL;
}
/* A stretched draw sampling a surface that has a wide copy samples the copy:
 * a composite of a view stays a composite of the whole wide view. */
struct WideResolve { ChVkWide *w; ChVkSampleResolver next; void *context; ChVkTarget *reader; int stretch; };
static void lend_release(void *context,ChVkTexture *image) {(void)context;(void)image;}
static VkResult wide_resolve(void *context,const Nv2aTexture *tx,ChVkSampleImage *out) {
    struct WideResolve *c=context;
    struct WideSurface *f=c->stretch?wide_source(c->w,tx):NULL;
    if(f && f->target!=c->reader) {
        Nv2aTexture key=*tx;key.width=f->tw;key.height=f->th;key.pitch=key.width*4;
        VkSamplerCreateInfo sampler;ChVkTexture *image=NULL;
        VkResult r=ch_vk_sampler_state(&key,&sampler);
        if(r==VK_SUCCESS) r=ch_vk_target_lend_texture(f->target,c->reader,tx->color==0x1e,&sampler,&image);
        if(r==VK_SUCCESS) {*out=(ChVkSampleImage){.width=key.width,.height=key.height,.gpu=image,.release=lend_release};return VK_SUCCESS;}
        if(r!=VK_NOT_READY) return r;
        if(c->w->config.textures) {
            r=ch_vk_texture_cache_resolve_target(c->w->config.textures,&key,f->target,f->serial,out);
            if(r!=VK_NOT_READY) return r;
        }
    }
    return c->next?c->next(c->context,tx,out):VK_NOT_READY;
}
VkResult ch_vk_wide_draw(ChVkWide *w,const Nv2aSurface *s,const Nv2aBatch *b,const ChVkFixedGeometry *fixed,
    ChVkSampleResolver resolve,void *context) {
    if(!w || !screen_shaped(s) || !b || !b->state || !b->raw) return VK_SUCCESS;
    if(!fixed) {w->skipped++;return VK_SUCCESS;}
    struct WideSurface *f=find(w,s); if(!f) {w->skipped++;return VK_SUCCESS;}
    VkResult r=target(w,f,s); if(r!=VK_SUCCESS) return r;
    int is3d=perspective(b),stretch=0;
    Nv2aRawExtent e;
    if(is3d) f->last_3d=w->frames;
    else {
        if(b->raw->extent && b->raw->extent(b->raw,&e) && e.x1-e.x0>=.9f*s->width) {
            const Nv2aTexture *tx=b->texture?b->texture:b->stage[0];
            int full=e.y1-e.y0>=.9f*s->height;
            int in_play=f->last_3d && w->frames-f->last_3d<WIDE_PLAY_FRAMES;
            if(!tx || (e.u1-e.u0<1e-3f && e.v1-e.v0<1e-3f)) stretch=1;   /* a flat colour: a fade, a letterbox bar */
            else if(full && wide_source(w,tx)) stretch=1;                 /* a composite of a view */
            else if(full && in_play) stretch=1;              /* a sky, a flash: in play, the whole width */
            /* else a 4:3 picture (menu, movie) or a textured band: in the middle */
        }
    }
    if(stretch) w->stretched++; else w->squeezed++;
    /* CHIHIRO_VULKAN_WIDE_LOG=2: one frame's 2D, every 60; =3: 2D across
     * nearly the whole width, any frame (200 lines) */
    static unsigned wide_lines;
    Nv2aRawExtent le={0};int lok=!is3d && w->log>2 && b->raw->extent && b->raw->extent(b->raw,&le);
    if(!is3d && ((w->log==2 && w->frames%60==0) ||
                 (lok && le.x1-le.x0>=.8f*s->width && wide_lines++<200))) {
        Nv2aRawExtent e={0};int ok=b->raw->extent && b->raw->extent(b->raw,&e);
        const Nv2aTexture *tx=b->texture?b->texture:b->stage[0];
        fprintf(stderr,"[wide 2d] frame %llu into %08X %s w0 %g ext %d xy %.1f..%.1f %.1f..%.1f uv %.2f..%.2f %.2f..%.2f tex %08X %ux%u fmt %02X stretch %d\n",
            (unsigned long long)w->frames,s->color_va,b->raw->kind==NV2A_RAW_FIXED?"fixed":"prog",b->raw->w0,ok,
            e.x0,e.x1,e.y0,e.y1,e.u0,e.u1,e.v0,e.v1,tx?tx->offset:0,tx?tx->width:0,tx?tx->height:0,tx?tx->color:0,stretch);
    }
    Nv2aBatch copy=*b;copy.zpass_out=NULL;
    ChVkImmDraw imm={.mode=3,.scale=f->xs};
    imm.gp[0]=f->xo;
    if(stretch) {
        /* Edge to edge: its own left and right at the wide picture's (Silent
         * Scope's pause dimmer spans 3.2..636.8 of 640: stretched as is, it
         * left 4-pixel strips undimmed at both edges). */
        float x0=e.x0/(float)s->width*2-1,x1=e.x1/(float)s->width*2-1;
        imm.scale=2.f/(x1-x0);imm.gp[0]=-1.f-x0*imm.scale;
    }
    f->serial++;
    struct WideResolve chain={w,resolve,context,f->target,stretch};
    return ch_vk_nv2a_draw_immersive_read(f->target,s,&copy,fixed,&imm,w->config.decode,wide_resolve,&chain,f->pixels,f->bytes);
}
VkResult ch_vk_wide_flip(ChVkWide *w,uint32_t presented,const void **rgba,uint32_t *width,uint32_t *height) {
    if(!w) return VK_NOT_READY;
    w->frames++;
    if(w->log && w->frames%300==0)
        fprintf(stderr,"[wide] frame %llu: %llu squeezed, %llu stretched, %llu not mirrored\n",(unsigned long long)w->frames,
            (unsigned long long)w->squeezed,(unsigned long long)w->stretched,(unsigned long long)w->skipped);
    struct WideSurface *f=NULL;
    for(unsigned i=0;presented && i<w->count;i++) if(w->surfaces[i].va==presented && w->surfaces[i].target) f=&w->surfaces[i];
    if(!f || !f->serial) return VK_NOT_READY;
    if(w->shown_bytes<f->bytes) {
        void *p=realloc(w->shown,f->bytes); if(!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
        w->shown=p;w->shown_bytes=f->bytes;
    }
    VkResult r=ch_vk_target_read_color(f->target,w->shown,f->bytes);
    if(r!=VK_SUCCESS) return r;
    *rgba=w->shown;*width=f->tw;*height=f->th;
    return VK_SUCCESS;
}
