#include "vk_immersive.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define IMM_SURFACES 8
#define ZOOM_SLOTS 8
/* A screen-shaped guest surface's eyes (2*eye_width x eye_height). */
struct ImmSurface {
    uint32_t va,width,height;
    ChVkTarget *eyes; unsigned char *pixels;
    VrGameProj projection; int projection_ok;
    uint64_t serial;         /* the eyes' contents, for snapshots */
    uint32_t world,drawn,flat,hud,full;    /* this frame: 3D batches; any eye work; 2D flat/HUD/full-screen */
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
    int zoom_normalise;   /* CHIHIRO_VULKAN_IMMERSIVE_ZOOM=1: the OpenGL renderer's zoom_proj */
    int log; uint64_t frames,world_batches,hud_batches,flat_batches,skipped_batches;
};
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
struct ImmResolve { ChVkImmersive *m; ChVkSampleResolver next; void *context; };
static VkResult imm_resolve(void *context,const Nv2aTexture *tx,ChVkSampleImage *out) {
    struct ImmResolve *c=context;
    struct ImmSurface *f=c->m->config.textures?eye_source(c->m,tx):NULL;
    if(f) {
        Nv2aTexture key=*tx;key.width=c->m->config.eye_width*2;key.height=c->m->config.eye_height;
        key.pitch=key.width*4;
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
    uint32_t w=m->config.eye_width*2,h=m->config.eye_height;
    size_t bytes=(size_t)w*h*4;
    f->pixels=malloc(bytes); if(!f->pixels) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult r=ch_vk_target_create_depth(m->config.physical,m->config.device,m->config.queue,m->config.family,
        w,h,VK_FORMAT_D24_UNORM_S8_UINT,&f->eyes);
    if(r==VK_ERROR_FORMAT_NOT_SUPPORTED) r=ch_vk_target_create(m->config.physical,m->config.device,
        m->config.queue,m->config.family,w,h,&f->eyes);
    if(r==VK_SUCCESS) r=configure(m,f->eyes,f->pixels,bytes);
    if(r==VK_SUCCESS && m->log) fprintf(stderr,"[immersive] eyes for surface %08X %ux%u: %ux%u\n",f->va,f->width,f->height,w,h);
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
VkResult ch_vk_immersive_create(const ChVkImmersiveConfig *config,ChVkImmersive **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;
    if(!config || !config->physical || !config->device || !config->queue || !config->eye_width ||
       !config->eye_height || config->eye_width>4096 || config->eye_height>4096) return VK_ERROR_INITIALIZATION_FAILED;
    ChVkImmersive *m=calloc(1,sizeof *m); if(!m) return VK_ERROR_OUT_OF_HOST_MEMORY;
    m->config=*config;
    if(!m->config.batch_limit) m->config.batch_limit=256;
    const char *e=getenv("CHIHIRO_VULKAN_IMMERSIVE_LOG");
    m->log=e?atoi(e):0;
    /* Off by default: the game's own projection recovers each vertex where
     * it is, so a game zoom changes nothing in the headset; normalising it
     * scaled the world when the game zoomed. */
    e=getenv("CHIHIRO_VULKAN_IMMERSIVE_ZOOM");
    m->zoom_normalise=e && atoi(e)==1;
    *out=m; return VK_SUCCESS;
}
void ch_vk_immersive_destroy(ChVkImmersive *m) {
    if(!m) return;
    for(unsigned i=0;i<m->count;i++) {ch_vk_target_destroy(m->surfaces[i].eyes);free(m->surfaces[i].pixels);}
    ch_vk_target_destroy(m->hud);free(m->hud_pixels);free(m);
}
int ch_vk_immersive_on(const ChVkImmersive *m) { return m && m->on; }
VkResult ch_vk_immersive_clear(ChVkImmersive *m,const Nv2aSurface *s,const Nv2aRenderState *rs,
    uint32_t flags,uint32_t argb,uint32_t zs) {
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
static VrGameProj zoomed(const ChVkImmersive *m,const VrGameProj *g) {
    VrGameProj o=*g;
    if(m->zoom_normalise && m->kbase>0 && fabsf(g->kx)>1) {float z=m->kbase/fabsf(g->kx);o.kx*=z;o.ky*=z;}
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
static int full_screen(const Nv2aBatch *b,const Nv2aSurface *s) {
    Nv2aRawExtent e;
    if(!b->raw->extent || !b->raw->extent(b->raw,&e)) return 0;
    return e.x1-e.x0>=.9f*s->width && e.y1-e.y0>=.9f*s->height;
}
VkResult ch_vk_immersive_draw(ChVkImmersive *m,const Nv2aSurface *s,const Nv2aBatch *b,
    const ChVkFixedGeometry *fixed,ChVkSampleResolver resolve,void *context) {
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
            }
        } else if(b->raw->w0!=1.0f && b->raw->w0==b->raw->w0) {
            /* A program batch: its surface's newest fixed projection, else
             * the frame's (as the OpenGL renderer does). */
            f=find(m,s);
            if(f && f->projection_ok) {g=f->projection;is3d=1;}
            else if(m->projection_ok) {g=m->projection;is3d=1;}
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
    if(!m->on) return VK_SUCCESS;
    if(!fixed) {m->skipped_batches++;return VK_SUCCESS;}
    Nv2aBatch copy=*b; copy.zpass_out=NULL;
    int full=!is3d && full_screen(b,s);
    if(!is3d) {if(!f) f=find(m,s);if(f) f->full+=full;}
    if(!is3d && shown(m,s->color_va) && !full) {
        if(f) f->hud++;
        /* The HUD has color only: depth and stencil cannot reject it. */
        VkResult r=hud(m,s); if(r!=VK_SUCCESS) return r;
        Nv2aRenderState state=*b->state;
        state.depth_test_enable=state.depth_write=state.stencil_enable=0;
        copy.state=&state;
        m->hud_batches++;
        return ch_vk_nv2a_draw_fixed_hud_read(m->hud,s,&copy,fixed,m->config.decode,resolve,context,
            m->hud_pixels,(size_t)m->hud_width*m->hud_height*4);
    }
    if(!f) f=find(m,s);
    if(!f) {m->skipped_batches++;return VK_SUCCESS;}
    VkResult r=eyes(m,f); if(r!=VK_SUCCESS) return r;
    ChVkImmDraw imm={.mode=is3d?1u:2u};
    if(!is3d && m->config.textures) {
        if(b->texture && eye_source(m,b->texture)) imm.sbs|=16;
        for(unsigned i=0;i<4;i++) if(b->stage[i] && eye_source(m,b->stage[i])) imm.sbs|=1u<<i;
    }
    if(is3d) {
        VrGameProj z=zoomed(m,&g);
        imm.gp[0]=z.kx;imm.gp[1]=z.ky;imm.gp[2]=z.cx;imm.gp[3]=z.cy;imm.gp[4]=z.inv_pz;
        m->world_batches++;
    } else {m->flat_batches++;f->flat++;}
    memcpy(imm.vp,m->view.vp,sizeof imm.vp);
    f->drawn++;f->serial++;
    struct ImmResolve chain={m,resolve,context};
    return ch_vk_nv2a_draw_immersive_read(f->eyes,s,&copy,fixed,&imm,m->config.decode,imm_resolve,&chain,
        f->pixels,(size_t)m->config.eye_width*2*m->config.eye_height*4);
}
VkResult ch_vk_immersive_flip(ChVkImmersive *m,uint32_t presented,const Vc3VrView *next,
    ChVkImmFrame *out,ChVkTarget **eyes_out,ChVkTarget **hud_out) {
    if(!m || !out || !eyes_out || !hud_out) return VK_ERROR_INITIALIZATION_FAILED;
    memset(out,0,sizeof *out);*eyes_out=*hud_out=NULL;
    /* The frame's projection: the screen-shaped surface the most 3D went
     * into -- the scene. */
    uint32_t best=0,total=0; struct ImmSurface *shown_f=NULL;
    for(unsigned i=0;i<m->count;i++) {
        struct ImmSurface *f=&m->surfaces[i];
        total+=f->world;
        if(f->projection_ok && f->world>best) {best=f->world;m->projection=f->projection;m->projection_ok=1;}
        if(f->va==presented) shown_f=f;
    }
    if(best) zoom_note(m,m->projection.kx);
    if(m->on && shown_f && shown_f->eyes && shown_f->world && m->projection_ok) {
        out->view=m->view;out->projection=zoomed(m,&m->projection);out->projection_ok=1;
        out->guest_width=shown_f->width;out->guest_height=shown_f->height;
        out->eye_width=m->config.eye_width;out->eye_height=m->config.eye_height;
        out->hud=m->hud_used;
        *eyes_out=shown_f->eyes;*hud_out=m->hud_used?m->hud:NULL;
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
        if(!f->eyes) continue;
        VkResult r=f->drawn?ch_vk_target_submit(f->eyes):ch_vk_target_finish(f->eyes);
        if(r!=VK_SUCCESS) return r;
    }
    if(m->hud) {VkResult r=m->hud_used?ch_vk_target_submit(m->hud):ch_vk_target_finish(m->hud);if(r!=VK_SUCCESS) return r;}
    m->previous_3d=total;
    m->on=next && next->on && total>0 && m->projection_ok;
    if(m->on) m->view=*next;
    m->hud_used=0;
    for(unsigned i=0;i<m->count;i++) {struct ImmSurface *f=&m->surfaces[i];f->world=f->drawn=f->flat=f->hud=f->full=0;}
    return VK_SUCCESS;
}
