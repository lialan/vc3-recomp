#include "vk_xr_quad.h"
#include <stdlib.h>
#include <math.h>
struct ChVkXrQuad { ChVkXrSwapchain *swapchain; XrCompositionLayerQuad layer; int ready,pending; };
XrResult ch_vk_xr_quad_pose(ChVkXrQuad *q,const XrPosef *pose,XrExtent2Df size) {
    if(!q || !pose || !isfinite(size.width) || !isfinite(size.height) || size.width<=0 || size.height<=0)
        return XR_ERROR_VALIDATION_FAILURE;
    if(q->pending) return XR_ERROR_CALL_ORDER_INVALID;
    const float values[]={pose->orientation.x,pose->orientation.y,pose->orientation.z,pose->orientation.w,
        pose->position.x,pose->position.y,pose->position.z};
    for(unsigned i=0;i<7;i++) if(!isfinite(values[i])) return XR_ERROR_POSE_INVALID;
    float norm=0; for(unsigned i=0;i<4;i++) norm+=values[i]*values[i];
    if(fabsf(norm-1)>.001f) return XR_ERROR_POSE_INVALID;
    q->layer.pose=*pose; q->layer.size=size; return XR_SUCCESS;
}
XrResult ch_vk_xr_quad_create(XrInstance instance,XrSession session,PFN_xrGetInstanceProcAddr get_proc,
    VkDevice device,uint32_t width,uint32_t height,XrSpace space,const XrPosef *pose,XrExtent2Df size,
    XrCompositionLayerFlags flags,ChVkXrQuad **out) {
    if(!out) return XR_ERROR_VALIDATION_FAILURE;
    *out=NULL;
    if(!space || width>INT32_MAX || height>INT32_MAX) return XR_ERROR_VALIDATION_FAILURE;
    ChVkXrQuad *q=calloc(1,sizeof(*q)); if(!q) return XR_ERROR_OUT_OF_MEMORY;
    q->layer=(XrCompositionLayerQuad){.type=XR_TYPE_COMPOSITION_LAYER_QUAD,.space=space,
        .layerFlags=flags,.eyeVisibility=XR_EYE_VISIBILITY_BOTH,
        .subImage={.imageRect={{0,0},{(int32_t)width,(int32_t)height}}}};
    XrResult r=ch_vk_xr_quad_pose(q,pose,size);
    if(r==XR_SUCCESS) r=ch_vk_xr_swapchain_create(instance,session,get_proc,device,width,height,&q->swapchain);
    if(r!=XR_SUCCESS) { free(q); return r; }
    q->layer.subImage.swapchain=ch_vk_xr_swapchain_handle(q->swapchain); *out=q; return XR_SUCCESS;
}
XrResult ch_vk_xr_quad_update(ChVkXrQuad *q,ChVkTarget *target,XrDuration timeout,VkResult *vk_result) {
    if(!q || !target || !vk_result || timeout<0) return XR_ERROR_VALIDATION_FAILURE;
    q->pending=1; q->ready=0;
    uint32_t index; VkImage image;
    XrResult r=ch_vk_xr_swapchain_wait(q->swapchain,timeout,&index,&image);
    if(r!=XR_SUCCESS) return r;
    r=ch_vk_xr_swapchain_copy_release(q->swapchain,target,NULL,0,vk_result);
    if(r==XR_SUCCESS) { q->pending=0; q->ready=1; }
    return r;
}
const XrCompositionLayerBaseHeader *ch_vk_xr_quad_layer(const ChVkXrQuad *q) {
    return q && q->ready && !q->pending?(const XrCompositionLayerBaseHeader *)&q->layer:NULL;
}
XrResult ch_vk_xr_quad_destroy(ChVkXrQuad *q) {
    if(!q) return XR_SUCCESS;
    XrResult r=ch_vk_xr_swapchain_destroy(q->swapchain);
    if(r==XR_SUCCESS) free(q); return r;
}

XrResult ch_vk_xr_quad_update_buffer(ChVkXrQuad *q,VkQueue queue,uint32_t family,VkBuffer buffer,XrDuration timeout,VkResult *vk_result) {
    if(!q || !buffer || !vk_result || timeout<0) return XR_ERROR_VALIDATION_FAILURE;
    q->pending=1;q->ready=0;
    uint32_t index;VkImage image;
    XrResult r=ch_vk_xr_swapchain_wait(q->swapchain,timeout,&index,&image);
    if(r!=XR_SUCCESS) return r;
    r=ch_vk_xr_swapchain_buffer_release(q->swapchain,queue,family,buffer,0,0,vk_result);
    if(r==XR_SUCCESS) {q->pending=0;q->ready=1;}
    return r;
}
XrResult ch_vk_xr_quad_update_texture(ChVkXrQuad *q,ChVkTexture *texture,XrDuration timeout,VkResult *vk_result) {
    if(!q || !texture || !vk_result || timeout<0) return XR_ERROR_VALIDATION_FAILURE;
    q->pending=1;q->ready=0;
    uint32_t index;VkImage image;
    XrResult r=ch_vk_xr_swapchain_wait(q->swapchain,timeout,&index,&image);
    if(r!=XR_SUCCESS) return r;
    r=ch_vk_xr_swapchain_texture_release(q->swapchain,texture,vk_result);
    if(r==XR_SUCCESS) {q->pending=0;q->ready=1;}
    return r;
}
