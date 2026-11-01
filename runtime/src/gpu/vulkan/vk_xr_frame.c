#include "vk_xr_frame.h"
#include <stdlib.h>
#include <string.h>
struct ChVkXrFrame {
    XrSession session; XrSpace space;
    ChVkXrSwapchain *swapchains[2]; uint32_t width[2],height[2];
    XrFrameState state; XrView views[2]; XrViewStateFlags flags;
    int active,failed; uint32_t next_eye;
    PFN_xrWaitFrame wait; PFN_xrBeginFrame begin; PFN_xrLocateViews locate; PFN_xrEndFrame end;
};
XrResult ch_vk_xr_frame_destroy(ChVkXrFrame *f) {
    if(!f) return XR_SUCCESS;
    if(f->active) return XR_ERROR_CALL_ORDER_INVALID;
    for(unsigned i=0;i<2;i++) if(f->swapchains[i]) {
        XrResult r=ch_vk_xr_swapchain_destroy(f->swapchains[i]);
        if(XR_FAILED(r)) return r;
        f->swapchains[i]=NULL;
    }
    free(f); return XR_SUCCESS;
}
XrResult ch_vk_xr_frame_create(XrInstance instance,XrSession session,XrSystemId system,XrSpace space,
    PFN_xrGetInstanceProcAddr get_proc,VkDevice device,ChVkXrFrame **out) {
    if(!out) return XR_ERROR_VALIDATION_FAILURE;
    *out=NULL;
    if(!instance || !session || !system || !space || !get_proc || !device) return XR_ERROR_VALIDATION_FAILURE;
    ChVkXrFrame *f=calloc(1,sizeof(*f)); if(!f) return XR_ERROR_OUT_OF_MEMORY;
    f->session=session; f->space=space; XrResult result;
    PFN_xrEnumerateViewConfigurationViews enumerate=NULL;
#define TRY(x) do { result=(x); if(XR_FAILED(result)) goto fail; } while(0)
#define LOAD(n,p) do { TRY(get_proc(instance,n,(PFN_xrVoidFunction *)&p)); if(!p) { result=XR_ERROR_FUNCTION_UNSUPPORTED; goto fail; } } while(0)
    LOAD("xrWaitFrame",f->wait); LOAD("xrBeginFrame",f->begin);
    LOAD("xrLocateViews",f->locate); LOAD("xrEndFrame",f->end);
    LOAD("xrEnumerateViewConfigurationViews",enumerate);
    uint32_t count=0;
    TRY(enumerate(instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,0,&count,NULL));
    if(count!=2) { result=XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED; goto fail; }
    XrViewConfigurationView config[2]={{.type=XR_TYPE_VIEW_CONFIGURATION_VIEW},{.type=XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    TRY(enumerate(instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,2,&count,config));
    if(count!=2) { result=XR_ERROR_RUNTIME_FAILURE; goto fail; }
    for(unsigned i=0;i<2;i++) {
        f->width[i]=config[i].recommendedImageRectWidth; f->height[i]=config[i].recommendedImageRectHeight;
        TRY(ch_vk_xr_swapchain_create(instance,session,get_proc,device,f->width[i],f->height[i],&f->swapchains[i]));
    }
    *out=f; return XR_SUCCESS;
fail:
    ch_vk_xr_frame_destroy(f); return result;
}
XrResult ch_vk_xr_frame_extent(const ChVkXrFrame *f,uint32_t eye,uint32_t *width,uint32_t *height) {
    if(!f || eye>=2 || !width || !height) return XR_ERROR_VALIDATION_FAILURE;
    *width=f->width[eye]; *height=f->height[eye]; return XR_SUCCESS;
}
XrResult ch_vk_xr_frame_begin(ChVkXrFrame *f,XrFrameState *state,XrView views[2],XrViewStateFlags *flags) {
    if(!f || !state || !views || !flags) return XR_ERROR_VALIDATION_FAILURE;
    if(f->active || f->failed) return XR_ERROR_CALL_ORDER_INVALID;
    XrFrameWaitInfo wait={.type=XR_TYPE_FRAME_WAIT_INFO};
    f->state=(XrFrameState){.type=XR_TYPE_FRAME_STATE};
    XrResult r=f->wait(f->session,&wait,&f->state); if(XR_FAILED(r)) return r;
    XrFrameBeginInfo begin={.type=XR_TYPE_FRAME_BEGIN_INFO};
    r=f->begin(f->session,&begin); if(XR_FAILED(r)) return r;
    f->active=1; f->next_eye=0; f->flags=0;
    for(unsigned i=0;i<2;i++) f->views[i]=(XrView){.type=XR_TYPE_VIEW};
    if(f->state.shouldRender) {
        XrViewLocateInfo locate={.type=XR_TYPE_VIEW_LOCATE_INFO,.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            .displayTime=f->state.predictedDisplayTime,.space=f->space};
        XrViewState view_state={.type=XR_TYPE_VIEW_STATE}; uint32_t count=0;
        r=f->locate(f->session,&locate,&view_state,2,&count,f->views);
        if(XR_FAILED(r) || count!=2) { f->failed=1; return XR_FAILED(r)?r:XR_ERROR_RUNTIME_FAILURE; }
        f->flags=view_state.viewStateFlags;
    }
    *state=f->state; memcpy(views,f->views,sizeof(f->views)); *flags=f->flags; return XR_SUCCESS;
}
static XrResult submit_composition(ChVkXrFrame *f,ChVkTarget *const eyes[2],int world,XrDuration timeout,
    const XrCompositionLayerBaseHeader *const *extra,uint32_t extra_count,VkResult *vk_result) {
    if(!f || !vk_result || timeout<0) return XR_ERROR_VALIDATION_FAILURE;
    if(!f->active || f->failed) return XR_ERROR_CALL_ORDER_INVALID;
    if(extra_count>8 || (extra_count && !extra)) return XR_ERROR_VALIDATION_FAILURE;
    for(uint32_t i=0;i<extra_count;i++) if(!extra[i]) return XR_ERROR_VALIDATION_FAILURE;
    *vk_result=VK_SUCCESS;
    XrViewStateFlags valid=XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT;
    int render=f->state.shouldRender && (f->flags & valid)==valid;
    XrCompositionLayerProjectionView projections[2]={0};
    if(render && world) {
        if(!eyes) return XR_ERROR_VALIDATION_FAILURE;
        for(unsigned i=0;i<2;i++) {
            uint32_t w,h; ch_vk_target_size(eyes[i],&w,&h);
            if(w!=f->width[i] || h!=f->height[i]) return XR_ERROR_VALIDATION_FAILURE;
        }
        while(f->next_eye<2) {
            unsigned i=f->next_eye; uint32_t index; VkImage image;
            XrResult r=ch_vk_xr_swapchain_wait(f->swapchains[i],timeout,&index,&image);
            if(r!=XR_SUCCESS) { if(XR_FAILED(r)) f->failed=1; return r; }
            r=ch_vk_xr_swapchain_copy_release(f->swapchains[i],eyes[i],NULL,0,vk_result);
            if(r!=XR_SUCCESS) { f->failed=1; return r; }
            f->next_eye++;
        }
        for(unsigned i=0;i<2;i++) projections[i]=(XrCompositionLayerProjectionView){
            .type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,.pose=f->views[i].pose,.fov=f->views[i].fov,
            .subImage={.swapchain=ch_vk_xr_swapchain_handle(f->swapchains[i]),.imageRect={{0,0},{(int32_t)f->width[i],(int32_t)f->height[i]}}}};
    }
    XrCompositionLayerProjection projection={.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION,.space=f->space,.viewCount=2,.views=projections};
    const XrCompositionLayerBaseHeader *layers[9];
    unsigned base=world?1:0;
    if(world) layers[0]=(const XrCompositionLayerBaseHeader *)&projection;
    for(uint32_t i=0;i<extra_count;i++) layers[i+base]=extra[i];
    XrFrameEndInfo end={.type=XR_TYPE_FRAME_END_INFO,.displayTime=f->state.predictedDisplayTime,
        .environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE,.layerCount=render?base+extra_count:0,.layers=render?layers:NULL};
    XrResult r=f->end(f->session,&end);
    if(XR_SUCCEEDED(r)) f->active=0; else f->failed=1;
    return r;
}

XrResult ch_vk_xr_frame_submit(ChVkXrFrame *f,ChVkTarget *const eyes[2],XrDuration timeout,VkResult *vk_result) {
    return ch_vk_xr_frame_submit_layers(f,eyes,timeout,NULL,0,vk_result);
}

XrResult ch_vk_xr_frame_submit_layers(ChVkXrFrame *f,ChVkTarget *const eyes[2],XrDuration timeout,
    const XrCompositionLayerBaseHeader *const *extra,uint32_t count,VkResult *result) {
    return submit_composition(f,eyes,1,timeout,extra,count,result);
}
XrResult ch_vk_xr_frame_submit_flat(ChVkXrFrame *f,
    const XrCompositionLayerBaseHeader *const *layers,uint32_t count,VkResult *result) {
    return submit_composition(f,NULL,0,0,layers,count,result);
}
