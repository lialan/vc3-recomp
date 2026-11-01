#include "vk_xr_screen.h"
#include <stdlib.h>
struct ChVkXrScreen {
    XrInstance instance; XrSession session; XrSpace space;
    PFN_xrGetInstanceProcAddr get_proc;
    VkPhysicalDevice physical; VkDevice device; VkQueue queue; uint32_t family;
    ChVkFrameMailbox *mailbox; ChVkCpuFrame pending;
    ChVkTarget *target; ChVkXrQuad *quad;
    uint32_t width,height; uint64_t sequence;
    int updating,failed;
};
XrResult ch_vk_xr_screen_create(XrInstance instance,XrSession session,XrSpace space,
    PFN_xrGetInstanceProcAddr get_proc,VkPhysicalDevice physical,VkDevice device,VkQueue queue,
    uint32_t family,ChVkFrameMailbox *mailbox,ChVkXrScreen **out) {
    if(!out) return XR_ERROR_VALIDATION_FAILURE;
    *out=NULL;
    if(!instance || !session || !space || !get_proc || !physical || !device || !queue || !mailbox)
        return XR_ERROR_VALIDATION_FAILURE;
    ChVkXrScreen *s=calloc(1,sizeof(*s)); if(!s) return XR_ERROR_OUT_OF_MEMORY;
    s->instance=instance; s->session=session; s->space=space; s->get_proc=get_proc;
    s->physical=physical; s->device=device; s->queue=queue; s->family=family; s->mailbox=mailbox;
    *out=s; return XR_SUCCESS;
}
XrResult ch_vk_xr_screen_destroy(ChVkXrScreen *s) {
    if(!s) return XR_SUCCESS;
    if(s->updating) return XR_ERROR_CALL_ORDER_INVALID;
    XrResult r=ch_vk_xr_quad_destroy(s->quad); if(r!=XR_SUCCESS) return r;
    ch_vk_target_destroy(s->target); ch_vk_cpu_frame_release(&s->pending); free(s); return XR_SUCCESS;
}
const XrCompositionLayerBaseHeader *ch_vk_xr_screen_layer(const ChVkXrScreen *s) {
    return s && !s->failed?ch_vk_xr_quad_layer(s->quad):NULL;
}
XrResult ch_vk_xr_screen_update(ChVkXrScreen *s,const XrPosef *pose,XrExtent2Df size,
    XrDuration timeout,VkResult *vk_result,uint64_t *sequence) {
    if(!s || !pose || !vk_result || !sequence || timeout<0) return XR_ERROR_VALIDATION_FAILURE;
    *vk_result=VK_SUCCESS; *sequence=s->sequence;
    if(s->failed) return XR_ERROR_CALL_ORDER_INVALID;
    XrResult r=XR_SUCCESS;
    if(!s->updating) {
        *vk_result=ch_vk_frame_mailbox_take(s->mailbox,&s->pending);
        if(*vk_result==VK_NOT_READY) {
            *vk_result=VK_SUCCESS;
            return s->quad?ch_vk_xr_quad_pose(s->quad,pose,size):XR_SUCCESS;
        }
        if(*vk_result!=VK_SUCCESS) { s->failed=1; return XR_ERROR_RUNTIME_FAILURE; }
        if(s->pending.immersive) { r=XR_ERROR_LAYER_INVALID; goto fail; } /* Host must dispatch stereo packets explicitly. */
        uint32_t width=s->pending.surface.width,height=s->pending.surface.height;
        if(width!=s->width || height!=s->height) {
            /* No image is acquired here; prior end-frame must have completed. */
            r=ch_vk_xr_quad_destroy(s->quad);
            if(r!=XR_SUCCESS) goto fail;
            s->quad=NULL; ch_vk_target_destroy(s->target); s->target=NULL;
            r=ch_vk_xr_quad_create(s->instance,s->session,s->get_proc,s->device,width,height,
                s->space,pose,size,0,&s->quad);
            if(r!=XR_SUCCESS) goto fail;
            *vk_result=ch_vk_target_create(s->physical,s->device,s->queue,s->family,width,height,&s->target);
            if(*vk_result!=VK_SUCCESS) { r=XR_ERROR_GRAPHICS_DEVICE_INVALID; goto fail; }
            const float blank[4]={0};
            void *scratch=malloc(s->pending.bytes);
            if(!scratch) { *vk_result=VK_ERROR_OUT_OF_HOST_MEMORY; r=XR_ERROR_OUT_OF_MEMORY; goto fail; }
            *vk_result=ch_vk_target_clear_read(s->target,blank,scratch,s->pending.bytes);
            free(scratch);
            if(*vk_result!=VK_SUCCESS) { r=XR_ERROR_GRAPHICS_DEVICE_INVALID; goto fail; }
            s->width=width; s->height=height;

        }
        r=ch_vk_xr_quad_pose(s->quad,pose,size); if(r!=XR_SUCCESS) goto fail;
        *vk_result=ch_vk_target_import_color(s->target,s->pending.rgba,s->pending.bytes);
        if(*vk_result!=VK_SUCCESS) { r=XR_ERROR_GRAPHICS_DEVICE_INVALID; goto fail; }
        s->updating=1;
    }
    r=ch_vk_xr_quad_update(s->quad,s->target,timeout,vk_result);
    if(r==XR_TIMEOUT_EXPIRED) return r;
    if(r!=XR_SUCCESS) goto fail;
    s->sequence=s->pending.sequence; *sequence=s->sequence;
    ch_vk_cpu_frame_release(&s->pending); s->updating=0; return XR_SUCCESS;
fail:
    s->failed=1; return r;
}
