/* Native Vulkan presentation for the shared Quest host. Included after the
 * common input/menu/view math; no session lifecycle or input polling here. */
static ChVkXrQuad *s_vk_screen,*s_vk_menu;
static ChVkTarget *s_vk_screen_target,*s_vk_menu_target;
static uint32_t s_vk_screen_w,s_vk_screen_h;
static uint64_t s_vk_screen_sequence;
static int s_vk_menu_dirty;
static struct {
    ChVkXrSwapchain *chain;
    ChVkTarget *target;
    uint32_t w,h;
    void *pixels;
} s_vk_eye[2];
static XrCompositionLayerProjection s_vk_overlay_layer;
static XrCompositionLayerProjectionView s_vk_overlay_views[2];

static struct {
    int enabled;
    uint64_t acquired,guest_wait,guest_hold,guest_calls,host_begin,host_hold,host_calls,last;
} s_vk_profile;
static void vk_queue_lock(void *context) {
    uint64_t start=s_vk_profile.enabled?pace_now_us():0;
    pthread_mutex_lock(context);
    if(s_vk_profile.enabled) {
        s_vk_profile.acquired=pace_now_us();
        s_vk_profile.guest_wait+=s_vk_profile.acquired-start;
    }
}
static void vk_queue_unlock(void *context) {
    if(s_vk_profile.enabled) {
        s_vk_profile.guest_hold+=pace_now_us()-s_vk_profile.acquired;
        s_vk_profile.guest_calls++;
    }
    pthread_mutex_unlock(context);
}
static void vk_profile_host_end(void) {
    if(!s_vk_profile.enabled) return;
    uint64_t now=pace_now_us();
    s_vk_profile.host_hold+=now-s_vk_profile.host_begin;s_vk_profile.host_calls++;
    if(!s_vk_profile.last) s_vk_profile.last=now;
    if(now-s_vk_profile.last<1000000) return;
    fprintf(stderr,"[Vulkan queue] %.2fs: host %llu calls hold %.1fms; guest %llu calls hold %.1fms wait %.1fms\n",
        (now-s_vk_profile.last)/1e6,(unsigned long long)s_vk_profile.host_calls,s_vk_profile.host_hold/1000.,
        (unsigned long long)s_vk_profile.guest_calls,s_vk_profile.guest_hold/1000.,s_vk_profile.guest_wait/1000.);
    s_vk_profile.last=now;s_vk_profile.guest_wait=s_vk_profile.guest_hold=s_vk_profile.guest_calls=0;
    s_vk_profile.host_hold=s_vk_profile.host_calls=0;
}
static void vk_check(VkResult result,const char *operation)
{
    if(result==VK_SUCCESS) return;
    fprintf(stderr,"[quest Vulkan] %s: VkResult=%d\n",operation,result);
    abort();
}
static void vk_xr_check(XrResult result,const char *operation)
{
    if(result==XR_SUCCESS) return;
    fprintf(stderr,"[quest Vulkan] %s: XrResult=%d\n",operation,result);
    abort();
}
static VkResult vk_publish(void *context,const Nv2aSurface *surface,const void *rgba,size_t bytes)
{
    s_tid_exec=(pid_t)syscall(SYS_gettid);
    pace_game_frame();
    return ch_vk_frame_mailbox_publish(context,surface,rgba,bytes);
}
static int vk_host_init(void)
{
    const char *profile=ch_env("VULKAN_PROFILE");
    s_vk_profile.enabled=profile && atoi(profile)!=0;
    VkResult result=ch_vk_frame_mailbox_create(&s_vk_frames);
    if(result!=VK_SUCCESS) return 0;
    ChVkBackendConfig config={.physical=s_vk.physical,.device=s_vk.device,
        .queue=s_vk.queue,.family=s_vk.family,.precise_queries=s_vk.precise_queries,.raw_fixed=s_vk.raw_fixed,
        .lock_queue=vk_queue_lock,.unlock_queue=vk_queue_unlock,.queue_context=&s_vk_queue,
        .render_context=s_vk_frames,.present_rgba=vk_publish};
    return ch_vk_runtime_configure(&config);
}
static void vk_initialized_target(uint32_t width,uint32_t height,ChVkTarget **target)
{
    vk_check(ch_vk_target_create(s_vk.physical,s_vk.device,s_vk.queue,s_vk.family,
        width,height,target),"presentation target");
    size_t bytes=(size_t)width*height*4;
    void *scratch=malloc(bytes);
    if(!scratch) abort();
    const float clear[4]={0,0,0,0};
    vk_check(ch_vk_target_clear_read(*target,clear,scratch,bytes),"presentation target initialization");
    free(scratch);
}
static void vk_menu_upload(const void *rgba)
{
    pthread_mutex_lock(&s_vk_queue);
    if(!s_vk_menu_target)
        vk_initialized_target(XM_W,XM_H,&s_vk_menu_target);
    vk_check(ch_vk_target_import_color(s_vk_menu_target,rgba,(size_t)XM_W*XM_H*4),"menu upload");
    s_vk_menu_dirty=1;
    pthread_mutex_unlock(&s_vk_queue);
}
static int vk_overlay_init(void)
{
    XrViewConfigurationView views[2]={{XR_TYPE_VIEW_CONFIGURATION_VIEW},{XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t count=0;
    if(XR_FAILED(xrEnumerateViewConfigurationViews(s_inst,s_sys,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,2,&count,views)) || count!=2) return 0;
    pthread_mutex_lock(&s_vk_queue);
    for(unsigned e=0;e<2;e++) {
        const char *sc=ch_env("OVERLAY_SCALE");
        float scale=sc && atof(sc)>.2 && atof(sc)<=1 ? (float)atof(sc) : 1;
        unsigned width=(unsigned)((s_native_w?s_native_w:views[e].recommendedImageRectWidth)*scale);
        unsigned height=(unsigned)((s_native_h?s_native_h:views[e].recommendedImageRectHeight)*scale);
        s_vk_eye[e].w=width;s_vk_eye[e].h=height;
        s_vk_eye[e].pixels=malloc((size_t)width*height*4);
        if(!s_vk_eye[e].pixels) abort();
        vk_check(ch_vk_target_create(s_vk.physical,s_vk.device,s_vk.queue,s_vk.family,
            width,height,&s_vk_eye[e].target),"overlay target");
        vk_xr_check(ch_vk_xr_swapchain_create(s_inst,s_sess,xrGetInstanceProcAddr,s_vk.device,
            width,height,&s_vk_eye[e].chain),"overlay swapchain");
    }
    pthread_mutex_unlock(&s_vk_queue);
    return 1;
}
static unsigned vk_present(const Frame *f,const XrCompositionLayerBaseHeader **layers)
{
    unsigned count=0;
    VkResult result=VK_SUCCESS;
    XrPosef pose={.orientation={0,0,0,1}};
    float forward[3]={0,0,-s_dist},center[3];
    yaw_rot(s_anchor_yaw,forward,center);
    pose.orientation.y=sinf(s_anchor_yaw*.5f);pose.orientation.w=cosf(s_anchor_yaw*.5f);
    pose.position=(XrVector3f){center[0]+s_anchor_p[0],center[1]+s_anchor_p[1],center[2]+s_anchor_p[2]};
    if(f) {
        XrExtent2Df size={s_height*(float)f->lw/f->lh,s_height};
        if(s_vk_screen_w!=(uint32_t)f->w || s_vk_screen_h!=(uint32_t)f->h) {
            vk_xr_check(ch_vk_xr_quad_destroy(s_vk_screen),"old screen");s_vk_screen=NULL;
            ch_vk_target_destroy(s_vk_screen_target);s_vk_screen_target=NULL;
            vk_initialized_target(f->w,f->h,&s_vk_screen_target);
            vk_xr_check(ch_vk_xr_quad_create(s_inst,s_sess,xrGetInstanceProcAddr,s_vk.device,
                f->w,f->h,s_local,&pose,size,0,&s_vk_screen),"screen swapchain");
            s_vk_screen_w=f->w;s_vk_screen_h=f->h;s_vk_screen_sequence=0;
        }
        vk_xr_check(ch_vk_xr_quad_pose(s_vk_screen,&pose,size),"screen pose");
        if(s_vk_screen_sequence!=f->cpu.sequence) {
            vk_check(ch_vk_target_import_color(s_vk_screen_target,f->cpu.rgba,f->cpu.bytes),"screen upload");
            vk_xr_check(ch_vk_xr_quad_update(s_vk_screen,s_vk_screen_target,XR_INFINITE_DURATION,&result),"screen copy");
            vk_check(result,"screen copy completion");
            s_vk_screen_sequence=f->cpu.sequence;
        }
        const XrCompositionLayerBaseHeader *layer=ch_vk_xr_quad_layer(s_vk_screen);
        if(layer) layers[count++]=layer;
    }
    if(s_overlay_ok>0 && s_views_ok && s_qo.gun_valid) {
        for(unsigned e=0;e<2;e++) {
            float vp[16],eye[3]={s_views[e].pose.position.x,s_views[e].pose.position.y,s_views[e].pose.position.z};
            uint32_t index;VkImage image;
            size_t bytes=(size_t)s_vk_eye[e].w*s_vk_eye[e].h*4;
            static const float transparent[4]={0,0,0,0};
            eye_viewproj(&s_views[e],vp);
            vk_check(ch_vk_target_clear_read(s_vk_eye[e].target,transparent,s_vk_eye[e].pixels,bytes),"overlay clear");
            vk_check(ch_vk_overlay_read(s_vk_eye[e].target,&s_qo,vp,eye,qo_sight_height(s_vr.sight),
                s_vk_eye[e].pixels,bytes),"overlay draw");
            vk_xr_check(ch_vk_xr_swapchain_wait(s_vk_eye[e].chain,XR_INFINITE_DURATION,&index,&image),"overlay acquire");
            vk_xr_check(ch_vk_xr_swapchain_copy_release(s_vk_eye[e].chain,s_vk_eye[e].target,NULL,0,&result),"overlay copy");
            vk_check(result,"overlay copy completion");
            s_vk_overlay_views[e]=(XrCompositionLayerProjectionView){
                .type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,.pose=s_views[e].pose,.fov=s_views[e].fov,
                .subImage={.swapchain=ch_vk_xr_swapchain_handle(s_vk_eye[e].chain),
                    .imageRect={.extent={(int32_t)s_vk_eye[e].w,(int32_t)s_vk_eye[e].h}}}};
        }
        s_vk_overlay_layer=(XrCompositionLayerProjection){.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION,
            .layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,.space=s_local,
            .viewCount=2,.views=s_vk_overlay_views};
        layers[count++]=(const XrCompositionLayerBaseHeader *)&s_vk_overlay_layer;
    }
    if(s_menu.open && s_vk_menu_target) {
        float yaw=atan2f(s_menu.n[0],s_menu.n[2]);
        pose=(XrPosef){.orientation={0,sinf(yaw*.5f),0,cosf(yaw*.5f)},
            .position={s_menu.c[0],s_menu.c[1],s_menu.c[2]}};
        XrExtent2Df size={s_menu.hw*2,s_menu.hh*2};
        if(!s_vk_menu) {
            vk_xr_check(ch_vk_xr_quad_create(s_inst,s_sess,xrGetInstanceProcAddr,s_vk.device,
                XM_W,XM_H,s_local,&pose,size,XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT|
                XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT,&s_vk_menu),"menu swapchain");
            s_vk_menu_dirty=1;
        }
        vk_xr_check(ch_vk_xr_quad_pose(s_vk_menu,&pose,size),"menu pose");
        if(s_vk_menu_dirty) {
            vk_xr_check(ch_vk_xr_quad_update(s_vk_menu,s_vk_menu_target,XR_INFINITE_DURATION,&result),"menu copy");
            vk_check(result,"menu copy completion");s_vk_menu_dirty=0;
        }
        const XrCompositionLayerBaseHeader *layer=ch_vk_xr_quad_layer(s_vk_menu);
        if(layer) layers[count++]=layer;
    }
    return count;
}
