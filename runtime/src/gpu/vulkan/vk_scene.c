#include "vk_scene.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define SCENE_SURFACES 32
struct SceneSurface {
    Nv2aSurface guest; VrGameProj projection; int projection_ok;
    uint32_t world_batches; ChVkTarget *eyes[2]; unsigned char *pixels[2],*seed; int cpu_owned;
};
struct ChVkScene {
    ChVkSceneConfig config; struct SceneSurface surfaces[SCENE_SURFACES]; unsigned count;
    Vc3VrView view; VrGameProj previous_projection; int previous_ok,previous_3d,active,immersive,hud_used;
    uint32_t shown; ChVkTarget *hud; unsigned char *hud_pixels;
};
static int screen(const Nv2aSurface *s) { return s && s->width && s->height && (uint64_t)s->width*3==(uint64_t)s->height*4; }
static struct SceneSurface *find(ChVkScene *s,uint32_t va) {
    for(unsigned i=0;i<s->count;i++) if(s->surfaces[i].guest.color_va==va) return &s->surfaces[i];
    return NULL;
}
static VkResult get(ChVkScene *s,const Nv2aSurface *guest,struct SceneSurface **out) {
    if(!s || !s->active || !guest || !guest->width || !guest->height) return VK_ERROR_INITIALIZATION_FAILED;
    struct SceneSurface *f=find(s,guest->color_va);
    if(f && (f->guest.width!=guest->width || f->guest.height!=guest->height || f->guest.pitch!=guest->pitch || f->guest.bytes_per_pixel!=guest->bytes_per_pixel))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if(!f) {
        if(s->count==SCENE_SURFACES) return VK_ERROR_TOO_MANY_OBJECTS;
        f=&s->surfaces[s->count++];
    }
    f->guest=*guest; *out=f; return VK_SUCCESS;
}
VkResult ch_vk_scene_create(const ChVkSceneConfig *config,ChVkScene **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;
    if(!config || !config->physical || !config->device || !config->queue || !config->decode ||
        !config->eye_width || !config->eye_height || !config->hud_width || !config->hud_height ||
        (uint64_t)config->eye_width*config->eye_height>SIZE_MAX/4 ||
        (uint64_t)config->hud_width*config->hud_height>SIZE_MAX/4) return VK_ERROR_INITIALIZATION_FAILED;
    ChVkScene *s=calloc(1,sizeof(*s)); if(!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->config=*config; *out=s; return VK_SUCCESS;
}
void ch_vk_scene_destroy(ChVkScene *s) {
    if(!s) return;
    for(unsigned i=0;i<s->count;i++) for(unsigned eye=0;eye<2;eye++) {
        ch_vk_target_destroy(s->surfaces[i].eyes[eye]); free(s->surfaces[i].pixels[eye]);
    }
    for(unsigned i=0;i<s->count;i++) free(s->surfaces[i].seed);
    ch_vk_target_destroy(s->hud); free(s->hud_pixels); free(s);
}
VkResult ch_vk_scene_begin(ChVkScene *s,const Vc3VrView *view,uint32_t shown) {
    if(!s || !view || s->active) return VK_ERROR_INITIALIZATION_FAILED;
    for(unsigned eye=0;eye<2;eye++) for(unsigned k=0;k<16;k++) if(view->on && !isfinite(view->vp[eye][k])) return VK_ERROR_INITIALIZATION_FAILED;
    s->view=*view; s->shown=shown; s->hud_used=0; s->active=1;
    s->immersive=view->on && s->previous_3d && s->previous_ok;
    for(unsigned i=0;i<s->count;i++) s->surfaces[i].world_batches=0;
    return VK_SUCCESS;
}
VkResult ch_vk_scene_observe(ChVkScene *s,const Nv2aSurface *guest,const Nv2aBatch *batch) {
    if(!batch || !batch->raw) return VK_ERROR_INITIALIZATION_FAILED;
    struct SceneSurface *f; VkResult r=get(s,guest,&f); if(r!=VK_SUCCESS) return r;
    VrGameProj projection;
    r=ch_vk_nv2a_projection_from_raw(batch,&projection);
    if(r==VK_SUCCESS) { f->projection=projection; f->projection_ok=1; return VK_SUCCESS; }
    /* Program batches still take the executor's CPU transform. Invalid fixed
     * metadata does not replace a valid cache with a guessed projection. */
    return r==VK_ERROR_FEATURE_NOT_PRESENT || r==VK_ERROR_INITIALIZATION_FAILED?VK_SUCCESS:r;
}
static VkResult seed_eye(ChVkScene *s,struct SceneSurface *f,unsigned eye) {
    if(!f->seed) return VK_SUCCESS;
    uint32_t width=s->config.eye_width,height=s->config.eye_height;
    for(uint32_t y=0;y<height;y++) for(uint32_t x=0;x<width;x++) {
        uint32_t gx=(uint32_t)(((uint64_t)x*2+1)*f->guest.width/(2*(uint64_t)width));
        uint32_t gy=(uint32_t)(((uint64_t)y*2+1)*f->guest.height/(2*(uint64_t)height));
        memcpy(f->pixels[eye]+((size_t)y*width+x)*4,f->seed+((size_t)gy*f->guest.width+gx)*4,4);
    }
    return ch_vk_target_import_color(f->eyes[eye],f->pixels[eye],(size_t)width*height*4);
}
VkResult ch_vk_scene_import(ChVkScene *s,const Nv2aSurface *guest,const void *rgba,size_t bytes,int initial) {
    struct SceneSurface *f; VkResult r=get(s,guest,&f); if(r!=VK_SUCCESS) return r;
    if(!rgba || (initial!=0 && initial!=1) || (uint64_t)guest->width*guest->height>SIZE_MAX/4 ||
        bytes!=(size_t)guest->width*guest->height*4) return VK_ERROR_INITIALIZATION_FAILED;
    if(!screen(guest)) return VK_SUCCESS;
    if(!f->seed) { f->seed=malloc(bytes); if(!f->seed) return VK_ERROR_OUT_OF_HOST_MEMORY; }
    memcpy(f->seed,rgba,bytes);
    if(!initial) f->cpu_owned=1;
    for(unsigned eye=0;eye<2;eye++) if(f->eyes[eye]) { r=seed_eye(s,f,eye); if(r!=VK_SUCCESS) return r; }
    return VK_SUCCESS;
}
static VkResult eyes(ChVkScene *s,struct SceneSurface *f) {
    size_t bytes=(size_t)s->config.eye_width*s->config.eye_height*4;
    const float blank[4]={0};
    for(unsigned eye=0;eye<2;eye++) if(!f->eyes[eye]) {
        f->pixels[eye]=malloc(bytes); if(!f->pixels[eye]) return VK_ERROR_OUT_OF_HOST_MEMORY;
        VkResult r=ch_vk_target_create(s->config.physical,s->config.device,s->config.queue,s->config.family,
            s->config.eye_width,s->config.eye_height,&f->eyes[eye]);
        if(r==VK_SUCCESS) r=ch_vk_target_clear_read(f->eyes[eye],blank,f->pixels[eye],bytes);
        if(r==VK_SUCCESS) r=seed_eye(s,f,eye);
        if(r!=VK_SUCCESS) return r;
    }
    return VK_SUCCESS;
}
VkResult ch_vk_scene_clear(ChVkScene *s,const Nv2aSurface *guest,const Nv2aRenderState *state,uint32_t flags,uint32_t argb,uint32_t zs) {
    struct SceneSurface *f; VkResult r=get(s,guest,&f); if(r!=VK_SUCCESS) return r;
    if(!s->immersive || !screen(guest)) { f->cpu_owned=0; return VK_SUCCESS; }
    r=eyes(s,f); if(r!=VK_SUCCESS) return r;
    for(unsigned eye=0;eye<2;eye++) {
        r=ch_vk_nv2a_clear_panel_read(f->eyes[eye],guest,state,flags,argb,zs,f->pixels[eye],(size_t)s->config.eye_width*s->config.eye_height*4);
        if(r!=VK_SUCCESS) return r;
    }
    f->cpu_owned=0; return VK_SUCCESS;
}
static int full(const Nv2aBatch *b,const Nv2aSurface *guest) {
    float x0=b->vertices[0].x,x1=x0,y0=b->vertices[0].y,y1=y0;
    for(unsigned i=1;i<b->count;i++) {
        x0=fminf(x0,b->vertices[i].x); x1=fmaxf(x1,b->vertices[i].x);
        y0=fminf(y0,b->vertices[i].y); y1=fmaxf(y1,b->vertices[i].y);
    }
    return x1-x0>=.9f*guest->width && y1-y0>=.9f*guest->height;
}
struct SampleContext { ChVkScene *scene; unsigned eye; uint32_t *argb; };
static VkResult resolve_sample(void *context,const Nv2aTexture *tx,ChVkSampleImage *image) {
    struct SampleContext *c=context; ChVkScene *s=c->scene;
    struct SceneSurface *source=find(s,tx->offset);
    if(!source) {
        /* An interior alias of a known stereo target cannot safely use flat
         * guest decoding; sub-image reinterpretation needs explicit mapping. */
        for(unsigned i=0;i<s->count;i++) {
            const Nv2aSurface *g=&s->surfaces[i].guest;
            if(screen(g) && !s->surfaces[i].cpu_owned && tx->offset>g->color_va && (uint64_t)tx->offset<
                (uint64_t)g->color_va+(uint64_t)(g->height-1)*g->pitch+(uint64_t)g->width*g->bytes_per_pixel)
                return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        return VK_NOT_READY;
    }
    if(!screen(&source->guest) || source->cpu_owned) return VK_NOT_READY;
    const Nv2aSurface *g=&source->guest;
    if(!source->eyes[c->eye] || !source->pixels[c->eye] || tx->width!=g->width || tx->height!=g->height ||
        tx->pitch!=g->pitch || (g->bytes_per_pixel==4?tx->color!=0x12:tx->color!=0x11))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    size_t count=(size_t)s->config.eye_width*s->config.eye_height;
    if(!c->argb) { c->argb=malloc(count*4); if(!c->argb) return VK_ERROR_OUT_OF_HOST_MEMORY; }
    const unsigned char *rgba=source->pixels[c->eye];
    for(size_t i=0;i<count;i++) c->argb[i]=((uint32_t)rgba[i*4+3]<<24)|((uint32_t)rgba[i*4]<<16)|((uint32_t)rgba[i*4+1]<<8)|rgba[i*4+2];
    *image=(ChVkSampleImage){.argb=c->argb,.width=s->config.eye_width,.height=s->config.eye_height};
    return VK_SUCCESS;
}
static VkResult draw_host(ChVkScene *s,ChVkTarget *target,const Nv2aSurface *guest,const Nv2aBatch *batch,
    const ChVkEye *view,unsigned eye,int coverage,void *pixels,size_t bytes) {
    struct SampleContext context={.scene=s,.eye=eye};
    VkResult result=ch_vk_nv2a_draw_host_read(target,guest,batch,s->config.decode,view,coverage,resolve_sample,&context,pixels,bytes);
    free(context.argb); return result;
}
VkResult ch_vk_scene_draw(ChVkScene *s,const Nv2aSurface *guest,const Nv2aBatch *batch) {
    struct SceneSurface *f; VkResult r=get(s,guest,&f); if(r!=VK_SUCCESS) return r;
    if(!batch || batch->raw || !batch->vertices || !batch->count || !batch->state) return VK_ERROR_INITIALIZATION_FAILED;
    if(batch->zpass_out) return VK_SUCCESS; /* Reference query pass is guest-only. */
    int is3d=batch->vertices[0].rhw!=1 && batch->vertices[0].rhw>0 && (f->projection_ok || s->previous_ok);
    if(is3d && screen(guest)) f->world_batches++;
    if(!s->immersive || !screen(guest)) { f->cpu_owned=0; return VK_SUCCESS; }
    Nv2aBatch b=*batch; b.zpass_out=NULL;
    if(!is3d && guest->color_va==s->shown && !full(batch,guest)) {
        size_t bytes=(size_t)s->config.hud_width*s->config.hud_height*4;
        if(!s->hud) {
            s->hud_pixels=malloc(bytes); if(!s->hud_pixels) return VK_ERROR_OUT_OF_HOST_MEMORY;
            r=ch_vk_target_create(s->config.physical,s->config.device,s->config.queue,s->config.family,s->config.hud_width,s->config.hud_height,&s->hud);
            if(r!=VK_SUCCESS) return r;
        }
        if(!s->hud_used) { const float blank[4]={0}; r=ch_vk_target_clear_read(s->hud,blank,s->hud_pixels,bytes); if(r!=VK_SUCCESS) return r; }
        /* Reference HUD FBO has color only: depth/stencil tests have no
         * attachments, so they cannot reject otherwise visible HUD pixels. */
        Nv2aRenderState hud_state=*b.state;
        hud_state.depth_test_enable=hud_state.depth_write=hud_state.stencil_enable=0;
        b.state=&hud_state;
        r=draw_host(s,s->hud,guest,&b,NULL,0,1,s->hud_pixels,bytes);
        if(r==VK_SUCCESS) { s->hud_used=1; f->cpu_owned=0; } return r;
    }
    r=eyes(s,f); if(r!=VK_SUCCESS) return r;
    for(unsigned eye=0;eye<2;eye++) {
        size_t bytes=(size_t)s->config.eye_width*s->config.eye_height*4;
        if(is3d) {
            ChVkEye view={.game_projection=f->projection_ok?f->projection:s->previous_projection};
            memcpy(view.gl_eye_vp,s->view.vp[eye],sizeof view.gl_eye_vp);
            r=draw_host(s,f->eyes[eye],guest,&b,&view,eye,0,f->pixels[eye],bytes);
        } else r=draw_host(s,f->eyes[eye],guest,&b,NULL,eye,0,f->pixels[eye],bytes);
        if(r!=VK_SUCCESS) return r;
    }
    f->cpu_owned=0; return VK_SUCCESS;
}
VkResult ch_vk_scene_end(ChVkScene *s,uint32_t presented,ChVkSceneFrame *out) {
    if(!s || !out || !s->active) return VK_ERROR_INITIALIZATION_FAILED;
    memset(out,0,sizeof(*out)); struct SceneSurface *f=find(s,presented); uint32_t best=0,total=0;
    for(unsigned i=0;i<s->count;i++) {
        struct SceneSurface *q=&s->surfaces[i]; total+=q->world_batches;
        if(screen(&q->guest) && q->projection_ok && q->world_batches>best) {
            best=q->world_batches; s->previous_projection=q->projection; s->previous_ok=1;
        }
    }
    if(s->immersive && total && f && !f->cpu_owned && f->eyes[0] && f->eyes[1]) {
        out->immersive=1; out->view=s->view; out->projection=s->previous_projection; /* Reference frame/HUD uses dominant scene projection. */
        out->guest_width=f->guest.width; out->guest_height=f->guest.height;
        out->eyes[0]=f->eyes[0]; out->eyes[1]=f->eyes[1]; out->hud=s->hud_used; out->hud_target=s->hud_used?s->hud:NULL;
    }
    out->world_batches=total; s->previous_3d=total!=0; s->active=0; return VK_SUCCESS;
}
