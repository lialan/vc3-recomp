#include "vk_submission.h"
#include <stdlib.h>
#include <string.h>
struct ChVkSubmission { ChVkSubmissionConfig config; ChVkBackend *backend; ChVkScene *scene; ChVkCpuFrame pending; };
static VkResult latch(ChVkSubmission *s,uint32_t shown) {
    Vc3VrView view={0}; VkResult r=s->config.latch_view(s->config.view_context,&view);
    return r==VK_SUCCESS?ch_vk_scene_begin(s->scene,&view,shown):r;
}
static VkResult raw(void *p,const Nv2aSurface *surface,const Nv2aBatch *batch) { return ch_vk_scene_observe(((ChVkSubmission *)p)->scene,surface,batch); }
static VkResult changed(void *p,const Nv2aSurface *surface,const void *rgba,size_t bytes,int initial) { return ch_vk_scene_import(((ChVkSubmission *)p)->scene,surface,rgba,bytes,initial); }
static VkResult clear(void *p,const Nv2aSurface *surface,const Nv2aRenderState *state,uint32_t flags,uint32_t color,uint32_t zs) { return ch_vk_scene_clear(((ChVkSubmission *)p)->scene,surface,state,flags,color,zs); }
static VkResult draw(void *p,const Nv2aSurface *surface,const Nv2aBatch *batch) { return ch_vk_scene_draw(((ChVkSubmission *)p)->scene,surface,batch); }
static VkResult present(void *p,const Nv2aSurface *surface,const void *rgba,size_t bytes) {
    ChVkSubmission *s=p;
    if(s->pending.rgba) return VK_ERROR_INITIALIZATION_FAILED;
    s->pending.rgba=malloc(bytes); if(!s->pending.rgba) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(s->pending.rgba,rgba,bytes); s->pending.bytes=bytes; s->pending.surface=*surface; return VK_SUCCESS;
}
static VkResult snapshot(ChVkTarget *target,ChVkCpuImage *image) {
    ch_vk_target_size(target,&image->width,&image->height);
    if(!image->width || !image->height || (uint64_t)image->width*image->height>SIZE_MAX/4) return VK_ERROR_INITIALIZATION_FAILED;
    image->bytes=(size_t)image->width*image->height*4; image->rgba=malloc(image->bytes);
    if(!image->rgba) return VK_ERROR_OUT_OF_HOST_MEMORY;
    return ch_vk_target_clear_aspects_read(target,0,NULL,0,0,image->rgba,image->bytes);
}
static VkResult flip(void *p,uint32_t shown) {
    ChVkSubmission *s=p; ChVkSceneFrame scene;
    VkResult r=ch_vk_scene_end(s->scene,shown,&scene); if(r!=VK_SUCCESS) return r;
    if(shown) {
        if(!s->pending.rgba || s->pending.surface.color_va!=shown) return VK_ERROR_INITIALIZATION_FAILED;
        s->pending.immersive=scene.immersive;
        if(scene.immersive) {
            s->pending.view=scene.view; s->pending.projection=scene.projection;
            for(unsigned eye=0;eye<2;eye++) { r=snapshot(scene.eyes[eye],&s->pending.eyes[eye]); if(r!=VK_SUCCESS) return r; }
            if(scene.hud) { r=snapshot(scene.hud_target,&s->pending.hud); if(r!=VK_SUCCESS) return r; }
        }
        r=ch_vk_frame_mailbox_publish_frame(s->config.frames,&s->pending);
        ch_vk_cpu_frame_release(&s->pending); if(r!=VK_SUCCESS) return r;
    }
    return latch(s,shown);
}
VkResult ch_vk_submission_create(const ChVkSubmissionConfig *config,ChVkSubmission **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;
    if(!config || !config->frames || !config->latch_view || config->backend.render_context || config->backend.observe_raw ||
       config->backend.guest_color_changed || config->backend.render_clear || config->backend.render_draw || config->backend.render_flip || config->backend.present_rgba)
        return VK_ERROR_INITIALIZATION_FAILED;
    ChVkSubmission *s=calloc(1,sizeof(*s)); if(!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->config=*config;
    ChVkSceneConfig scene={.physical=config->backend.physical,.device=config->backend.device,.queue=config->backend.queue,
        .family=config->backend.family,.eye_width=config->eye_width,.eye_height=config->eye_height,
        .hud_width=config->hud_width,.hud_height=config->hud_height,.decode=config->backend.decode};
    VkResult r=ch_vk_scene_create(&scene,&s->scene); if(r!=VK_SUCCESS) goto fail;
    ChVkBackendConfig backend=config->backend; backend.render_context=s;
    backend.observe_raw=raw; backend.guest_color_changed=changed; backend.render_clear=clear; backend.render_draw=draw;
    backend.render_flip=flip; backend.present_rgba=present;
    r=ch_vk_backend_create(&backend,&s->backend); if(r!=VK_SUCCESS) goto fail;
    r=latch(s,0); if(r!=VK_SUCCESS) goto fail;
    *out=s; return VK_SUCCESS;
fail:
    ch_vk_submission_destroy(s); return r;
}
VkResult ch_vk_submission_attach(ChVkSubmission *s) { return s?ch_vk_backend_attach(s->backend):VK_ERROR_INITIALIZATION_FAILED; }
void ch_vk_submission_destroy(ChVkSubmission *s) {
    if(!s) return;
    ch_vk_backend_destroy(s->backend); ch_vk_scene_destroy(s->scene); ch_vk_cpu_frame_release(&s->pending); free(s);
}
