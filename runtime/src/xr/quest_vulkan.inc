#include "../gpu/vulkan/vk_device_features.h"
#include "../gpu/vulkan/vk_state.h"
/* Native Vulkan presentation for the shared Quest host. Included after the
 * common input/menu/view math; no session lifecycle or input polling here. */
static ChVkXrQuad *s_vk_screen,*s_vk_menu;
static ChVkTarget *s_vk_menu_target;
static uint32_t s_vk_screen_w,s_vk_screen_h;
static uint64_t s_vk_screen_sequence;
static int s_vk_menu_dirty;
static int s_vk_split;            /* the game on its own VkDevice (vk_split_init) */
static ChVkXrBridge s_vk_bridge;
static pthread_mutex_t s_vk_game_queue=PTHREAD_MUTEX_INITIALIZER;
static struct {
    ChVkXrSwapchain *chain;
    ChVkTarget *target;
    uint32_t w,h;
    void *pixels;
} s_vk_eye[2];
/* The game's picture drawn into each eye's image, behind the gun: SteamVR
 * shows OpenXR quad layers above the app's projection layers, so a quad
 * screen hid the gun. VC3_XR_SCREEN_QUAD=1 brings the quad back. */
static uint64_t s_vk_scr_sequence;
static float s_vk_scr_aspect=4.0f/3.0f;
static int s_vk_scr_eyes=-1;
static XrCompositionLayerProjection s_vk_overlay_layer;
static XrCompositionLayerProjectionView s_vk_overlay_views[2];
static int s_vk_overlay_have_last;
static uint32_t s_vk_overlay_frame;
static void vk_overlay_skip_frame(void) {
    s_vk_overlay_have_last=0;s_vk_overlay_frame++;
}

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
static VkResult vk_publish(void *context,const Nv2aSurface *surface,ChVkTarget *target)
{
    s_tid_exec=(pid_t)syscall(SYS_gettid);
    pace_game_frame();
    {   /* VC3_XR_DUMP: the 600th frame handed to the headset, as published */
        static unsigned n,at;
        if(!at) at=ch_env("XR_DUMP_FRAME")?(unsigned)atoi(ch_env("XR_DUMP_FRAME")):600;
        if(ch_env("XR_DUMP") && ++n==at) {
            uint32_t w,h;ch_vk_target_size(target,&w,&h);
            unsigned char *px=malloc((size_t)w*h*4);
            if(px && ch_vk_target_read_color(target,px,(VkDeviceSize)w*h*4)==VK_SUCCESS) {
                for(size_t i=0;i<(size_t)w*h;i++) px[i*4+3]=255;
                dump_ppm("published.ppm",px,(int)w,(int)h);
            }
            free(px);
        }
    }
    return ch_vk_gpu_mailbox_publish(context,surface,target);
}
/* Immersive (vk_immersive.h): the next frame's head view, at each flip. */
static int vk_imm_view(void *context,Vc3VrView *view)
{
    (void)context;
    return vc3_vr_view_latch(view);
}
/* a slot: the eyes side by side at up to VK_EYE_MAX each, the HUD (up to
 * 1280x960) after them, then the lens picture (up to 512x512) */
#define VK_EYE_MAX 2560
#define VK_BRIDGE_CAPACITY ((VkDeviceSize)2*VK_EYE_MAX*VK_EYE_MAX*4+(VkDeviceSize)1280*960*4+(VkDeviceSize)512*512*4+512)
/* Each eye's picture: the panel's native size (s_native_w; 2160 on the
 * Steam Frame, where SteamVR recommends 1728), with foveated shading
 * (vk_split_init) keeping E.S. on its vblank there. The panel's Resolution
 * per eye (vr_eye=n) or VC3_IMM_EYE=n draws n x n instead (up to VK_EYE_MAX). */
static void vk_imm_eye_size(uint32_t *w,uint32_t *h)
{
    const char *e=ch_env("IMM_EYE");
    int n=e?atoi(e):s_vr.eye;
    if(n>=256 && n<=VK_EYE_MAX) {*w=*h=(uint32_t)n;return;}
    uint32_t nw=s_native_w>=256?s_native_w:1440,nh=s_native_h>=256?s_native_h:nw;
    *w=nw;*h=nh;
    if(*w>VK_EYE_MAX) *w=VK_EYE_MAX;
    if(*h>VK_EYE_MAX) *h=VK_EYE_MAX;
}
static void vk_dump_target(const char *name,ChVkTarget *target,int opaque)
{
    uint32_t w,h;ch_vk_target_size(target,&w,&h);
    unsigned char *px=malloc((size_t)w*h*4);
    if(px && ch_vk_target_read_color(target,px,(VkDeviceSize)w*h*4)==VK_SUCCESS) {
        if(!opaque) {   /* the alpha too, as grey: name.alpha.ppm */
            unsigned char *a=malloc((size_t)w*h*4);char an[64];
            if(a) {
                for(size_t i=0;i<(size_t)w*h;i++) a[i*4]=a[i*4+1]=a[i*4+2]=px[i*4+3],a[i*4+3]=255;
                snprintf(an,sizeof an,"%s.alpha.ppm",name);dump_ppm(an,a,(int)w,(int)h);free(a);
            }
        }
        if(opaque) for(size_t i=0;i<(size_t)w*h;i++) px[i*4+3]=255;
        dump_ppm(name,px,(int)w,(int)h);
    }
    free(px);
}
static VkResult vk_publish_imm(void *context,const Nv2aSurface *surface,ChVkTarget *eyes,ChVkTarget *hud,
    ChVkTarget *lens,const ChVkImmFrame *info)
{
    s_tid_exec=(pid_t)syscall(SYS_gettid);
    pace_game_frame();
    {   /* VC3_XR_DUMP: the eyes and HUD of the XR_DUMP_FRAME-th immersive frame */
        static unsigned n,at;
        if(!at) at=ch_env("XR_DUMP_FRAME")?(unsigned)atoi(ch_env("XR_DUMP_FRAME")):600;
        if(ch_env("XR_DUMP") && ++n==at) {
            vk_dump_target("eyes.ppm",eyes,1);
            if(hud) vk_dump_target("hud.ppm",hud,0);
            if(lens && info->lens) vk_dump_target("lens.ppm",lens,1);
            fprintf(stderr,"[xr] immersive dump: projection kx %.2f ky %.2f cx %.2f cy %.2f 1/pz %.5g, guest %ux%u\n",
                info->projection.kx,info->projection.ky,info->projection.cx,info->projection.cy,
                info->projection.inv_pz,info->guest_width,info->guest_height);
        }
    }
    return ch_vk_gpu_mailbox_publish_immersive(context,surface,eyes,hud,lens,info);
}
/* The game on a VkDevice of its own (Steam Frame and Quest;
 * VC3_XR_SPLIT_DEVICE=0: off), which immersive VR needs. SteamVR (Steam
 * Frame) puts its per-display-frame waits on the queue the session is
 * given; sharing it, each of the emulated GPU's many waits a
 * frame lasted until the next display frame -- ~30 fps at 120 Hz, worse at
 * 90. Its own device has a queue of its own; frames cross to the headset's
 * device through shared buffers (vk_xr_bridge.h). Falls back to the one
 * device, logged, if any of it is missing. */
static void vk_split_init(VkDevice *device,VkQueue *queue)
{
    const char *e=ch_env("XR_SPLIT_DEVICE");
    if(e && *e=='0') {fprintf(stderr,"[quest Vulkan] one device (XR_SPLIT_DEVICE=0)\n");return;}
    if(!s_vk.external_memory) {fprintf(stderr,"[quest Vulkan] one device: no VK_KHR_external_memory_fd\n");return;}
    VkPhysicalDeviceFeatures f;vkGetPhysicalDeviceFeatures(s_vk.physical,&f);
    VkPhysicalDeviceFeatures want={.occlusionQueryPrecise=f.occlusionQueryPrecise,.fillModeNonSolid=f.fillModeNonSolid,
        .shaderClipDistance=f.shaderClipDistance,.depthClamp=f.depthClamp};
    ch_vk_sampler_anisotropy_enable(s_vk.physical,&want);
    float priority=1;
    VkDeviceQueueCreateInfo q={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=s_vk.family,
        .queueCount=1,.pQueuePriorities=&priority};
    const char *ext[8]={VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};uint32_t ext_count=1;
    ChVkShaderFeatures shader_features;
    if(!ch_vk_shader_features(s_vk.physical,&shader_features,ext,&ext_count,8)) s_vk.raw_fixed=0;
    /* VC3_XR_GAME_PRIORITY=low|medium|high: the game queue's system-wide
     * priority (VK_EXT_global_priority), so the headset's per-display-frame
     * work can be scheduled ahead of the game's rendering. */
    VkDeviceQueueGlobalPriorityCreateInfoEXT gp={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_EXT};
    {
        const char *lv=ch_env("XR_GAME_PRIORITY");
        if(lv && !strcmp(lv,"low")) gp.globalPriority=VK_QUEUE_GLOBAL_PRIORITY_LOW_EXT;
        else if(lv && !strcmp(lv,"medium")) gp.globalPriority=VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_EXT;
        else if(lv && !strcmp(lv,"high")) gp.globalPriority=VK_QUEUE_GLOBAL_PRIORITY_HIGH_EXT;
        if(gp.globalPriority) {
            uint32_t n=0;int have=0;vkEnumerateDeviceExtensionProperties(s_vk.physical,NULL,&n,NULL);
            VkExtensionProperties *p=n?calloc(n,sizeof(*p)):NULL;
            if(p && vkEnumerateDeviceExtensionProperties(s_vk.physical,NULL,&n,p)==VK_SUCCESS)
                for(uint32_t i=0;i<n;i++) if(!strcmp(p[i].extensionName,VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME)) have=1;
            free(p);
            if(have) {ext[ext_count++]=VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME;q.pNext=&gp;}
            else {fprintf(stderr,"[quest Vulkan] game priority %s: not offered\n",lv);gp.globalPriority=0;}
        }
    }
    /* Foveated shading on the eye images (vk_target.c): full rate within
     * `inner` degrees of each eye's view axis, 2x1 to `outer`, 2x2 to `far`,
     * 4x4 beyond -- native resolution for well under half the shading.
     * VC3_XR_FOVEATION=inner,outer,far (default 30,45,60); 0: off. At
     * 2160 per eye on the Steam Frame E.S. went from 70 of 239 game frames
     * late to 0. */
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR fsr={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
        .pipelineFragmentShadingRate=VK_TRUE,.attachmentFragmentShadingRate=VK_TRUE};
    float fov_inner=30,fov_outer=45,fov_far=60;uint32_t fov_tw=0,fov_th=0;
    {
        const char *fv=ch_env("XR_FOVEATION");
        if(fv && sscanf(fv,"%f,%f,%f",&fov_inner,&fov_outer,&fov_far)<1) fov_inner=0;
        if(fv && atof(fv)<=0) fov_inner=0;
        VkPhysicalDeviceFragmentShadingRateFeaturesKHR have={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
        VkPhysicalDeviceFeatures2 f2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&have};
        VkPhysicalDeviceFragmentShadingRatePropertiesKHR fp={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&fp};
        int ext_ok=ch_vk_device_has_extension(s_vk.physical,VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)
            && ch_vk_device_has_extension(s_vk.physical,VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
        if(fov_inner>0 && ext_ok) {
            vkGetPhysicalDeviceFeatures2(s_vk.physical,&f2);
            vkGetPhysicalDeviceProperties2(s_vk.physical,&p2);
        }
        if(fov_inner>0 && ext_ok && have.pipelineFragmentShadingRate && have.attachmentFragmentShadingRate && ext_count+2<=8) {
            ext[ext_count++]=VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME;
            ext[ext_count++]=VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME;
            fsr.pNext=shader_features.ubo_layout.pNext;shader_features.ubo_layout.pNext=&fsr;
            fov_tw=fp.minFragmentShadingRateAttachmentTexelSize.width;fov_th=fp.minFragmentShadingRateAttachmentTexelSize.height;
            if(fov_tw<16 && 16<=fp.maxFragmentShadingRateAttachmentTexelSize.width) fov_tw=16;
            if(fov_th<16 && 16<=fp.maxFragmentShadingRateAttachmentTexelSize.height) fov_th=16;
        } else if(fov_inner>0) {
            fprintf(stderr,"[quest Vulkan] foveated shading not offered\n");fov_inner=0;
        }
    }
    VkDeviceCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&shader_features.ubo_layout,.queueCreateInfoCount=1,.pQueueCreateInfos=&q,
        .pEnabledFeatures=&want,.enabledExtensionCount=ext_count,.ppEnabledExtensionNames=ext};
    VkDevice game;VkResult r=vkCreateDevice(s_vk.physical,&dc,NULL,&game);
    if(r!=VK_SUCCESS && gp.globalPriority) {
        fprintf(stderr,"[quest Vulkan] game priority refused (%d): the default\n",(int)r);
        q.pNext=NULL;dc.enabledExtensionCount=--ext_count;gp.globalPriority=0;
        r=vkCreateDevice(s_vk.physical,&dc,NULL,&game);
    }
    if(r==VK_SUCCESS && fov_inner>0) {
        void ch_vk_target_set_foveation(VkDevice,PFN_vkCreateRenderPass2KHR,uint32_t,uint32_t,float,float,float);
        PFN_vkCreateRenderPass2KHR pass2=(PFN_vkCreateRenderPass2KHR)vkGetDeviceProcAddr(game,"vkCreateRenderPass2KHR");
        if(pass2) {
            ch_vk_target_set_foveation(game,pass2,fov_tw,fov_th,fov_inner,fov_outer,fov_far);
            fprintf(stderr,"[quest Vulkan] foveated shading: full rate within %.0f deg, 2x1 to %.0f, 2x2 to %.0f, 4x4 beyond (rate texel %ux%u)\n",
                fov_inner,fov_outer,fov_far,fov_tw,fov_th);
        }
    }
    if(r==VK_SUCCESS && gp.globalPriority) fprintf(stderr,"[quest Vulkan] game queue priority %s\n",ch_env("XR_GAME_PRIORITY"));
    if(r!=VK_SUCCESS) {fprintf(stderr,"[quest Vulkan] one device: game device %d\n",(int)r);return;}
    r=ch_vk_xr_bridge_create(s_vk.physical,s_vk.device,game,VK_BRIDGE_CAPACITY,&s_vk_bridge);
    if(r!=VK_SUCCESS) {
        fprintf(stderr,"[quest Vulkan] one device: shared buffers %d\n",(int)r);
        vkDestroyDevice(game,NULL);return;
    }
    ch_vk_gpu_mailbox_set_buffers(s_vk_frames,s_vk_bridge.game_buffer,VK_BRIDGE_CAPACITY);
    *device=game;vkGetDeviceQueue(game,s_vk.family,0,queue);s_vk_split=1;
    fprintf(stderr,"[quest Vulkan] the game on its own VkDevice; frames to the headset's through shared buffers\n");
}
uint32_t xbox_VblankCount(void);
uint64_t xbox_VblankLastNs(void);
uint64_t xbox_HostNowNs(void);
/* The next emulated vblank after the latest one (60 Hz), 0 when the vblank
 * clock is not running. */
static uint64_t vk_frame_due(void)
{
    uint64_t last=xbox_VblankLastNs(),now=xbox_HostNowNs();
    if(!last || now<last || now-last>40000000ull) return 0;
    return last+16666667ull;
}
static int vk_host_init(void)
{
    const char *profile=ch_env("VULKAN_PROFILE");
    s_vk_profile.enabled=profile && atoi(profile)!=0;
    VkResult result=ch_vk_gpu_mailbox_create(&s_vk_frames);
    if(result!=VK_SUCCESS) return 0;
    /* The display takes a game frame every second 120 Hz frame. With only
     * the newest waiting, one finished a little after its display frame
     * was replaced by the next and the old one held for four: judder. Two
     * waiting, taken in order, keep every frame on screen twice; a full
     * queue drops its oldest. VC3_XR_FRAME_QUEUE=1: newest only. */
    {const char *q=ch_env("XR_FRAME_QUEUE");ch_vk_gpu_mailbox_set_queue(s_vk_frames,q && atoi(q)==1?1:2);}
    /* VC3_XR_PRESENT_PACING=1 (with flips paced to the vblank): a frame is
     * shown from the vblank its flip completes at, as the console shows it.
     * Opt-in: against the headset's own 120 Hz loop it delayed frames by a
     * whole display frame (E.S. activation: 22-25 late frames a window,
     * 4-13 without). */
    {
        const char *f=getenv("RECOMP_FLIP_PACING"),*p=ch_env("XR_PRESENT_PACING");
        if(f && *f && strcmp(f,"0") && p && *p=='1')
            ch_vk_gpu_mailbox_set_pacing(s_vk_frames,vk_frame_due,xbox_HostNowNs);
    }
    VkDevice game_device=s_vk.device; VkQueue game_queue=s_vk.queue;
    void *game_lock=&s_vk_queue;
    vk_split_init(&game_device,&game_queue);
    if(s_vk_split) game_lock=&s_vk_game_queue;
    ChVkBackendConfig config={.physical=s_vk.physical,.device=game_device,
        .queue=game_queue,.family=s_vk.family,.precise_queries=s_vk.precise_queries,.raw_fixed=s_vk.raw_fixed,
        .lock_queue=vk_queue_lock,.unlock_queue=vk_queue_unlock,.queue_context=game_lock,
        .render_context=s_vk_frames,.present_target=vk_publish};
    /* Immersive needs the shared buffers (the eyes cross to the headset's
     * device in them) and the GPU transforms. */
    if(s_vk_split && s_vk.raw_fixed) {
        config.immersive_view=vk_imm_view;config.present_immersive=vk_publish_imm;
        vk_imm_eye_size(&config.immersive_eye_width,&config.immersive_eye_height);
        const ChihiroProfile *p=chihiro_profile();
        config.immersive_lens_k=p->vr.lens_k;
        config.immersive_program_projection=(VrGameProj){p->vr.program_proj.kx,p->vr.program_proj.ky,
            p->vr.program_proj.cx,p->vr.program_proj.cy,p->vr.program_proj.inv_pz};
    }
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
static int vk_hud_in_overlay(void);
static int vk_overlay_init(void)
{
    XrViewConfigurationView views[2]={{XR_TYPE_VIEW_CONFIGURATION_VIEW},{XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t count=0;
    if(XR_FAILED(xrEnumerateViewConfigurationViews(s_inst,s_sys,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,2,&count,views)) || count!=2) return 0;
    pthread_mutex_lock(&s_vk_queue);
    for(unsigned e=0;e<2;e++) {
        const char *sc=ch_env("OVERLAY_SCALE");
        /* 0.75 by default: the overlay holds the gun, laser and dot (the HUD
         * is a quad of its own); SteamVR blends it over the whole view at
         * 120 Hz, and 0.75 showed 93% of game frames exactly twice with the
         * headset on against 89% at full size. */
        float scale=sc && atof(sc)>.2 && atof(sc)<=1 ? (float)atof(sc) : vk_hud_in_overlay()?1:.75f;
        unsigned width=(unsigned)((s_native_w?s_native_w:views[e].recommendedImageRectWidth)*scale);
        unsigned height=(unsigned)((s_native_h?s_native_h:views[e].recommendedImageRectHeight)*scale);
        s_vk_eye[e].w=width;s_vk_eye[e].h=height;
        s_vk_eye[e].pixels=malloc((size_t)width*height*4);
        if(!s_vk_eye[e].pixels) abort();
        vk_check(ch_vk_target_create(s_vk.physical,s_vk.device,s_vk.queue,s_vk.family,
            width,height,&s_vk_eye[e].target),"overlay target");
        vk_check(ch_vk_target_batching(s_vk_eye[e].target,1),"overlay command batches");
        ch_vk_target_defer_color_readback(s_vk_eye[e].target,1);
        vk_xr_check(ch_vk_xr_swapchain_create(s_inst,s_sess,xrGetInstanceProcAddr,s_vk.device,
            width,height,&s_vk_eye[e].chain),"overlay swapchain");
    }
    pthread_mutex_unlock(&s_vk_queue);
    return 1;
}
/* VC3_XR_FAKE_GUN=1 with VC3_XR_DUMP: once the views are known, a gun held
 * in front of the left eye, aimed straight ahead, drawn into that eye's
 * overlay and written out; VC3_XR_FAKE_MENU=1 also paints the panel. */
static int vk_screen_in_eyes(void);
static void vk_hud_draw(const Frame *,ChVkTarget *,const float [16],void *,size_t);
static int s_vk_hud_valid;
static int vk_world_update(const Frame *);
static void vk_screen_draw(ChVkTarget *,const float [16],void *,size_t);
static ChVkTexture *s_vk_scr_tex;
static ChVkTarget *s_vk_scr_target;
static uint32_t s_vk_scr_w,s_vk_scr_h;
static int vk_screen_update(const Frame *);
static void vk_test_dump(const Frame *f)
{
    static int done;
    static unsigned frames;
    if(done || !ch_env("XR_DUMP") || !s_views_ok || s_overlay_ok<=0 || ++frames<(ch_env("XR_DUMP_AFTER")?(unsigned)atoi(ch_env("XR_DUMP_AFTER")):60u)) return;
    /* XR_DUMP_ZOOMED=k: wait for an immersive frame whose game lens is
     * longer than k pixels (the death and continue close-up) and its HUD */
    if(ch_env("XR_DUMP_ZOOMED")) {
        if(!f || !f->vr.imm || !(fabsf(f->vr.gp[0])>atof(ch_env("XR_DUMP_ZOOMED")))) return;
        vk_world_update(f);   /* headless, SteamVR never asks for a frame: the HUD from here */
        if(!s_vk_hud_valid) return;
    }
    /* headless, SteamVR never asks for a frame (shouldRender): the screen from here */
    if(vk_screen_in_eyes() && f && !f->vr.imm) vk_screen_update(f);
    done=1;
    if(ch_env("XR_FAKE_MENU") && *ch_env("XR_FAKE_MENU")=='1') {s_menu.open=0;menu_toggle("test");menu_paint();}
    if(!ch_env("XR_FAKE_GUN") || (*ch_env("XR_FAKE_GUN")!='1' && *ch_env("XR_FAKE_GUN")!='2')) return;
    head_pose(s_anchor_p,&s_anchor_yaw);screen_fill_apply();   /* the screen ahead, as after a recentre */
    const XrView *v=&s_views[0];
    /* XR_FAKE_GUN=2: the eye on the sight line, as when aiming through it */
    const int aiming=ch_env("XR_FAKE_GUN")[0]=='2';
    const float offset[3]={aiming?0:0.03f,aiming?-qo_sight_height(s_vr.sight):-0.035f,-0.32f},ahead[3]={0,0,-2.6f};
    float o[3],a[3],vp[16],eye[3]={v->pose.position.x,v->pose.position.y,v->pose.position.z};
    qrot(v->pose.orientation,offset,o);qrot(v->pose.orientation,ahead,a);
    QoFrame g={.gun_valid=1,.gun_q={v->pose.orientation.x,v->pose.orientation.y,v->pose.orientation.z,v->pose.orientation.w},
        .sight=s_vr.sight,.show_gun=1,.beam=s_vr.laser==LASER_ON,.dot=1,
        .weapon=chihiro_profile()->vr.weapon,.scope_scale=qo_scope_scale(),
        /* the rifle's lens as the last overlay frame showed it */
        .lens_tex=s_qo.lens_tex,.lens_fresh=s_qo.lens_fresh,.lens_cross=s_qo.lens_cross,
        .lens_crop={s_qo.lens_crop[0],s_qo.lens_crop[1],s_qo.lens_crop[2]}};
    if(g.weapon==CH_VR_RIFLE) fprintf(stderr,"[xr] test overlay: lens %s, crop at %.4f %.4f half %.3f, crosshair %d, scope scale %.2f\n",
        g.lens_fresh && g.lens_tex?"with a picture":"dark",g.lens_crop[0],g.lens_crop[1],g.lens_crop[2],g.lens_cross,g.scope_scale);
    /* the rifle's laser as rifle_along draws it: on unless Off, no dot, far out along the barrel */
    const float reach=g.weapon==CH_VR_RIFLE?RIFLE_LASER_LEN/2.6f:1.0f;
    if(g.weapon==CH_VR_RIFLE) {g.beam=s_vr.laser!=LASER_OFF;g.dot=0;}
    for(int k=0;k<3;k++){g.gun_p[k]=eye[k]+o[k];g.end[k]=g.gun_p[k]+a[k]*reach;}
    size_t bytes=(size_t)s_vk_eye[0].w*s_vk_eye[0].h*4;
    static const float transparent[4]={0,0,0,0};
    eye_viewproj(v,vp);
    vk_check(ch_vk_target_clear_read(s_vk_eye[0].target,transparent,s_vk_eye[0].pixels,bytes),"test overlay clear");
    if(s_vk_scr_tex) vk_screen_draw(s_vk_eye[0].target,vp,s_vk_eye[0].pixels,bytes);
    if(f && f->vr.imm) vk_hud_draw(f,s_vk_eye[0].target,vp,s_vk_eye[0].pixels,bytes);
    vk_check(ch_vk_overlay_read(s_vk_eye[0].target,&g,vp,eye,qo_sight_height(g.sight),s_vk_eye[0].pixels,bytes),"test overlay draw");
    vk_check(ch_vk_target_read_color(s_vk_eye[0].target,s_vk_eye[0].pixels,bytes),"test overlay read");
    dump_ppm("overlay.ppm",s_vk_eye[0].pixels,(int)s_vk_eye[0].w,(int)s_vk_eye[0].h);
}
static int vk_screen_in_eyes(void)
{
    if(s_vk_scr_eyes<0) {
        const char *e=ch_env("XR_SCREEN_QUAD");
        s_vk_scr_eyes=s_vk_split && !(e && *e=='1');
        fprintf(stderr,"[xr] game screen drawn %s\n",s_vk_scr_eyes?"into the eye images":"as a quad layer");
    }
    return s_vk_scr_eyes;
}
/* A new frame from the slot's shared buffer: returns whether it changed. */
static int vk_screen_update(const Frame *f)
{
    if(s_vk_scr_w!=(uint32_t)f->w || s_vk_scr_h!=(uint32_t)f->h) {
        ch_vk_texture_destroy(s_vk_scr_tex);s_vk_scr_tex=NULL;
        ch_vk_target_destroy(s_vk_scr_target);s_vk_scr_target=NULL;
        vk_initialized_target((uint32_t)f->w,(uint32_t)f->h,&s_vk_scr_target);
        vk_check(ch_vk_target_snapshot_texture(s_vk_scr_target,1,&s_vk_scr_tex),"screen texture");
        const VkSamplerCreateInfo linear={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter=VK_FILTER_LINEAR,.minFilter=VK_FILTER_LINEAR,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        vk_check(ch_vk_texture_set_sampler(s_vk_scr_tex,&linear),"screen sampler");
        s_vk_scr_w=(uint32_t)f->w;s_vk_scr_h=(uint32_t)f->h;s_vk_scr_sequence=0;
    }
    if(f->lw>0 && f->lh>0) s_vk_scr_aspect=(float)f->lw/(float)f->lh;
    if(s_vk_scr_sequence==f->gpu.sequence) return 0;
    vk_check(ch_vk_target_import_buffer(s_vk_scr_target,s_vk_bridge.xr_buffer[f->gpu.slot],
        s_vk_bridge.capacity),"screen import");
    vk_check(ch_vk_target_refresh_texture(s_vk_scr_target,s_vk_scr_tex),"screen refresh");
    s_vk_scr_sequence=f->gpu.sequence;
    return 1;
}
/* The screen where the quad layer would hang (s_dist ahead of the anchor,
 * s_height tall), opaque, into an eye target in LOCAL space. */
static void vk_screen_draw(ChVkTarget *target,const float vp[16],void *pixels,size_t bytes)
{
    const float right[3]={1,0,0},ahead[3]={0,0,-s_dist};
    float r[3],c[3],hw=s_height*s_vk_scr_aspect*.5f,hh=s_height*.5f;
    yaw_rot(s_anchor_yaw,right,r);yaw_rot(s_anchor_yaw,ahead,c);
    for(int k=0;k<3;k++) c[k]+=s_anchor_p[k];
    static const float corner[6][4]={{-1,1,0,0},{1,1,1,0},{-1,-1,0,1},{1,1,1,0},{1,-1,1,1},{-1,-1,0,1}};
    ChVkColorVertex v[6]={0};
    static const uint16_t idx[6]={0,1,2,3,4,5};
    for(int i=0;i<6;i++) {
        float p[3],clip[4];
        for(int k=0;k<3;k++) p[k]=c[k]+r[k]*hw*corner[i][0]+(k==1?hh*corner[i][1]:0);
        for(int k=0;k<4;k++) clip[k]=vp[k]*p[0]+vp[4+k]*p[1]+vp[8+k]*p[2]+vp[12+k];
        v[i].xy[0]=clip[0];v[i].xy[1]=-clip[1];v[i].xy[2]=(clip[2]+clip[3])*.5f;v[i].xy[3]=clip[3];
        v[i].xy[4]=corner[i][2];v[i].xy[5]=corner[i][3];
        v[i].rgba[0]=v[i].rgba[1]=v[i].rgba[2]=0;v[i].rgba[3]=1;
    }
    /* opaque first (the game's alpha means nothing here), then the picture */
    const Nv2aRenderState opaque={.color_mask=0x01010101},picture={.color_mask=0x00010101};
    vk_check(ch_vk_target_draw_state_read(target,NULL,v,6,idx,6,&opaque,pixels,bytes),"screen backing");
    for(int i=0;i<6;i++) v[i].rgba[0]=v[i].rgba[1]=v[i].rgba[2]=1;
    vk_check(ch_vk_target_draw_texture_read(target,NULL,v,6,idx,6,&picture,s_vk_scr_tex,pixels,bytes),"screen draw");
}
/* ── Immersive frames: the eyes as a projection layer, drawn with the pose
 * they were drawn from (the compositor turns them to where the head is),
 * the HUD on its panel in the overlay, under the gun. */
static ChVkXrSwapchain *s_vk_world;
static uint32_t s_vk_world_w,s_vk_world_h;
static float s_vk_world_scale=1;   /* the copied frame's dynamic resolution (ChVkImmFrame.scale) */
static int s_vk_world_shown;       /* the last frame submitted showed the world layer */
static uint64_t s_vk_world_sequence;
static XrCompositionLayerProjection s_vk_world_layer;
static XrCompositionLayerProjectionView s_vk_world_views[2];
static ChVkTarget *s_vk_hud_target;
static ChVkXrQuad *s_vk_hud_quad;
/* VC3_XR_HUD_OVERLAY=1: the HUD drawn into the overlay (sampled from a
 * texture), as before the quad -- the overlay then full size. */
static int vk_hud_in_overlay(void)
{
    static int on=-1;
    if(on<0) on=ch_env("XR_HUD_OVERLAY") && *ch_env("XR_HUD_OVERLAY")=='1';
    return on;
}
static uint32_t s_vk_hud_tw,s_vk_hud_th;
static ChVkTarget *s_vk_lens_target;   /* the rifle's lens picture, as s_vk_hud_tex */
static ChVkTexture *s_vk_lens_tex;
static uint32_t s_vk_lens_tw,s_vk_lens_th;
static VrLensAim s_vk_lens_aim;        /* what it was drawn with */
/* The lens at the display's rate: each overlay frame shows the part of the
 * newest picture the rifle points at now (vr_lens_crop: the picture was
 * drawn wider than the lens shows, with the aim the game had then), with a
 * crosshair at the lens's centre -- the game's reticle is not in a picture
 * with an aim. Every 10 s the log says how often the turn since the picture
 * went past its edge (clamped); VC3_XR_LENS_LOG=1: each overlay frame's crop
 * too (the first 600). */
static void vk_lens_crop(void)
{
    static unsigned frames,clamped,lines;
    static float most;
    static uint64_t since;
    static int log=-1;
    if(log<0) log=ch_env("XR_LENS_LOG") && *ch_env("XR_LENS_LOG")=='1';
    /* the rifle aimed the picture (no reticle in it): the lens follows it */
    const int known=s_qo.lens_fresh && s_vk_lens_aim.guard>=1 && s_vk_lens_aim.follow;
    const int c=vr_lens_crop(&s_vk_lens_aim,s_rifle_aim_ok?s_rifle_yaw:NAN,s_rifle_aim_ok?s_rifle_pitch:NAN,s_qo.lens_crop);
    s_qo.lens_cross=known;
    if(!known) return;
    const float du=s_qo.lens_crop[0]-.5f,dv=s_qo.lens_crop[1]-.5f,off=sqrtf(du*du+dv*dv);
    frames++;clamped+=c;if(off>most) most=off;
    if(log && lines<600) {
        lines++;
        fprintf(stderr,"[rifle lens] picture yaw %.4f pitch %.4f deg (focal %.2f, guard %.2f), rifle yaw %.4f pitch %.4f deg"
            " -> crop at %.4f %.4f (half %.3f)%s\n",s_vk_lens_aim.yaw*57.29578f,s_vk_lens_aim.pitch*57.29578f,
            s_vk_lens_aim.focal,s_vk_lens_aim.guard,s_rifle_yaw*57.29578f,s_rifle_pitch*57.29578f,
            s_qo.lens_crop[0],s_qo.lens_crop[1],s_qo.lens_crop[2],c?" clamped":"");
    }
    const uint64_t now=pace_now_us();
    if(!since) since=now;
    if(now-since>=10000000) {
        fprintf(stderr,"[rifle lens] %u overlay frames with a picture: %u clamped (%.1f%%), largest shift %.3f of the picture"
            " (the band: %.3f)\n",frames,clamped,frames?100.0*clamped/frames:0.0,most,.5f-s_qo.lens_crop[2]);
        since=now;frames=clamped=0;most=0;
    }
}
static ChVkTexture *s_vk_hud_tex;
static uint32_t s_vk_hud_w,s_vk_hud_h;
static int s_vk_hud_valid;
/* VC3_XR_LOOP_LOG: where the headset loop's work goes (summed, us) */
static uint64_t s_vk_seg[12];   /* world, clear, HUD, gun, swapchain copy, settle; world: acquire, copy, HUD import, HUD refresh; overlay: acquire, copy */
#define SEG(i,stmt) do {uint64_t seg_t=pace_now_us();stmt;s_vk_seg[i]+=pace_now_us()-seg_t;} while(0)
/* The headset loop's own GPU work (the world and HUD copies, the overlay
 * exports): recorded into a ring of command buffers, submitted to the
 * session's queue and never waited for there -- SteamVR gates that queue
 * once a display frame, so each wait cost up to one (8-27 ms a loop with
 * the headset on: SteamVR throttled the loop down to 20 Hz). A fence is
 * waited for only when its command buffer comes round again (long done),
 * and the world copy's before its source goes back to the game. */
#define VK_HOST_CMDS 8
static struct {
    VkCommandPool pool;VkCommandBuffer cmd[VK_HOST_CMDS];VkFence fence[VK_HOST_CMDS];
    int busy[VK_HOST_CMDS];unsigned next;int world;   /* world: the last world copy's slot, -1 none */
} s_vk_host={.world=-1};
static void vk_host_wait(unsigned i)
{
    if(!s_vk_host.busy[i]) return;
    vk_check(vkWaitForFences(s_vk.device,1,&s_vk_host.fence[i],VK_TRUE,5000000000ULL),"host command wait");
    s_vk_host.busy[i]=0;
}
static VkCommandBuffer vk_host_begin(void)
{
    if(!s_vk_host.pool) {
        VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=s_vk.family};
        vk_check(vkCreateCommandPool(s_vk.device,&pi,NULL,&s_vk_host.pool),"host command pool");
        VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool=s_vk_host.pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=VK_HOST_CMDS};
        vk_check(vkAllocateCommandBuffers(s_vk.device,&ai,s_vk_host.cmd),"host command buffers");
        for(unsigned i=0;i<VK_HOST_CMDS;i++) {
            VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            vk_check(vkCreateFence(s_vk.device,&fi,NULL,&s_vk_host.fence[i]),"host command fence");
        }
    }
    unsigned i=s_vk_host.next%VK_HOST_CMDS;
    vk_host_wait(i);
    vk_check(vkResetFences(s_vk.device,1,&s_vk_host.fence[i]),"host fence reset");
    vk_check(vkResetCommandBuffer(s_vk_host.cmd[i],0),"host command reset");
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk_check(vkBeginCommandBuffer(s_vk_host.cmd[i],&begin),"host command begin");
    return s_vk_host.cmd[i];
}
static unsigned vk_host_submit(void)
{
    unsigned i=s_vk_host.next%VK_HOST_CMDS;
    vk_check(vkEndCommandBuffer(s_vk_host.cmd[i]),"host command end");
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&s_vk_host.cmd[i]};
    vk_check(vkQueueSubmit(s_vk.queue,1,&submit,s_vk_host.fence[i]),"host command submit");
    s_vk_host.busy[i]=1;s_vk_host.next++;
    return i;
}
/* Before take_frame can give a shared buffer back to the game. */
static void vk_host_world_settle(void)
{
    if(s_vk_host.world>=0) vk_host_wait((unsigned)s_vk_host.world);
    s_vk_host.world=-1;
}
/* A new immersive frame into the world swapchain and the HUD texture;
 * returns whether anything changed. */
static int vk_world_update(const Frame *f)
{
    VkResult result=VK_SUCCESS;
    const uint32_t w=(uint32_t)f->w,h=(uint32_t)f->h;
    if(s_vk_world_w!=w || s_vk_world_h!=h) {
        if(s_vk_world) vk_xr_check(ch_vk_xr_swapchain_destroy(s_vk_world),"old world swapchain");
        s_vk_world=NULL;
        vk_xr_check(ch_vk_xr_swapchain_create(s_inst,s_sess,xrGetInstanceProcAddr,s_vk.device,w,h,&s_vk_world),"world swapchain");
        s_vk_world_w=w;s_vk_world_h=h;s_vk_world_sequence=0;
        fprintf(stderr,"[xr] immersive: eyes %ux%u, side by side\n",w/2,h);
    }
    if(s_vk_world_sequence==f->gpu.sequence) return 0;
    uint32_t index;VkImage image;
    (void)result;
    SEG(6,vk_xr_check(ch_vk_xr_swapchain_wait(s_vk_world,XR_INFINITE_DURATION,&index,&image),"world acquire"));
    VkCommandBuffer cmd=vk_host_begin();
    SEG(7,vk_xr_check(ch_vk_xr_swapchain_record_buffer(s_vk_world,cmd,s_vk_bridge.xr_buffer[f->gpu.slot],0),"world copy"));
    s_vk_world_scale=f->gpu.imm.scale>0 && f->gpu.imm.scale<=1?f->gpu.imm.scale:1;
    s_drawn_pct=(int)(s_vk_world_scale*100+.5f);
    /* The HUD: a quad layer of its own at the game's 640x480, between the
     * world and the overlay (SteamVR blends it only where it is; drawn into
     * the full-view overlay it made that layer full size, and SteamVR
     * blends the overlay over the whole view at 120 Hz). */
    s_vk_hud_valid=f->gpu.imm.hud;
    if(s_vk_hud_valid && vk_hud_in_overlay()) {
        const uint32_t hw=f->gpu.surface.width,hh=f->gpu.surface.height;
        if(s_vk_hud_tw!=hw || s_vk_hud_th!=hh) {
            ch_vk_texture_destroy(s_vk_hud_tex);s_vk_hud_tex=NULL;
            ch_vk_target_destroy(s_vk_hud_target);s_vk_hud_target=NULL;
            vk_initialized_target(hw,hh,&s_vk_hud_target);
            vk_check(ch_vk_target_snapshot_texture(s_vk_hud_target,1,&s_vk_hud_tex),"HUD texture");
            const VkSamplerCreateInfo linear={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .magFilter=VK_FILTER_LINEAR,.minFilter=VK_FILTER_LINEAR,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,
                .addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
            vk_check(ch_vk_texture_set_sampler(s_vk_hud_tex,&linear),"HUD sampler");
            s_vk_hud_tw=hw;s_vk_hud_th=hh;
        }
        SEG(8,vk_check(ch_vk_texture_record_from_buffer(s_vk_hud_tex,cmd,s_vk_bridge.xr_buffer[f->gpu.slot],
            f->gpu.hud_offset),"HUD copy"));
    } else if(s_vk_hud_valid) {
        const uint32_t hw=f->gpu.surface.width,hh=f->gpu.surface.height;
        if(s_vk_hud_quad && (s_vk_hud_w!=hw || s_vk_hud_h!=hh)) {
            vk_xr_check(ch_vk_xr_quad_destroy(s_vk_hud_quad),"old HUD quad");s_vk_hud_quad=NULL;
        }
        if(!s_vk_hud_quad) {
            const XrPosef pose={.orientation={0,0,0,1},.position={0,0,-5}};
            vk_xr_check(ch_vk_xr_quad_create(s_inst,s_sess,xrGetInstanceProcAddr,s_vk.device,hw,hh,s_local,&pose,
                (XrExtent2Df){1,1},XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT|
                XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT,&s_vk_hud_quad),"HUD quad");
            s_vk_hud_w=hw;s_vk_hud_h=hh;
        }
        SEG(8,vk_xr_check(ch_vk_xr_quad_record_buffer(s_vk_hud_quad,cmd,s_vk_bridge.xr_buffer[f->gpu.slot],
            f->gpu.hud_offset),"HUD copy"));
    }
    /* The rifle's lens picture: the same way, into the overlay's texture. It
     * stays as the last frame left it on display frames that bring none new;
     * lens_fresh follows the newest game frame's lens. */
    s_qo.lens_fresh=f->gpu.imm.lens && f->gpu.imm.lens_w && f->gpu.imm.lens_h;
    if(s_qo.lens_fresh) {
        const uint32_t lw=f->gpu.imm.lens_w,lh=f->gpu.imm.lens_h;
        if(s_vk_lens_tw!=lw || s_vk_lens_th!=lh || !s_vk_lens_tex) {
            ch_vk_texture_destroy(s_vk_lens_tex);s_vk_lens_tex=NULL;s_qo.lens_tex=NULL;
            ch_vk_target_destroy(s_vk_lens_target);s_vk_lens_target=NULL;
            vk_initialized_target(lw,lh,&s_vk_lens_target);
            vk_check(ch_vk_target_snapshot_texture(s_vk_lens_target,1,&s_vk_lens_tex),"lens texture");
            const VkSamplerCreateInfo linear={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .magFilter=VK_FILTER_LINEAR,.minFilter=VK_FILTER_LINEAR,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,
                .addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
            vk_check(ch_vk_texture_set_sampler(s_vk_lens_tex,&linear),"lens sampler");
            s_vk_lens_tw=lw;s_vk_lens_th=lh;s_qo.lens_tex=s_vk_lens_tex;
        }
        vk_check(ch_vk_texture_record_from_buffer(s_vk_lens_tex,cmd,s_vk_bridge.xr_buffer[f->gpu.slot],
            f->gpu.imm.lens_offset),"lens copy");
        s_vk_lens_aim=f->gpu.imm.lens_aim;
    }
    s_vk_host.world=(int)vk_host_submit();
    SEG(9,vk_xr_check(ch_vk_xr_swapchain_release_submitted(s_vk_world),"world release"));
    if(s_vk_hud_valid && !vk_hud_in_overlay()) vk_xr_check(ch_vk_xr_quad_release_submitted(s_vk_hud_quad),"HUD release");
    s_vk_world_sequence=f->gpu.sequence;
    return 1;
}
static const XrCompositionLayerBaseHeader *vk_world_layer(const Frame *f)
{
    if(!s_vk_world) return NULL;
    for(int e=0;e<2;e++) {
        XrCompositionLayerProjectionView *v=&s_vk_world_views[e];
        memset(v,0,sizeof *v);
        v->type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        view_to_local(&f->vr.view,e,&v->pose);
        v->fov.angleLeft=f->vr.view.fov[e][0];v->fov.angleRight=f->vr.view.fov[e][1];
        v->fov.angleUp=f->vr.view.fov[e][2];v->fov.angleDown=f->vr.view.fov[e][3];
        v->subImage.swapchain=ch_vk_xr_swapchain_handle(s_vk_world);
        /* dynamic resolution: each eye's picture fills scale of its half,
         * the left eye's from the top-left, the right eye's from the top-right */
        uint32_t ew=(uint32_t)(s_vk_world_w/2*s_vk_world_scale+.5f),eh=(uint32_t)(s_vk_world_h*s_vk_world_scale+.5f);
        v->subImage.imageRect.offset.x=(int32_t)(e?s_vk_world_w-ew:0);
        v->subImage.imageRect.extent.width=(int32_t)ew;
        v->subImage.imageRect.extent.height=(int32_t)eh;
    }
    s_vk_world_layer=(XrCompositionLayerProjection){.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION,
        .space=s_local,.viewCount=2,.views=s_vk_world_views};
    return (const XrCompositionLayerBaseHeader *)&s_vk_world_layer;
}
/* The HUD on its panel -- the game's view at HUD_DIST from
 * the anchor -- into an overlay eye target. */
static void vk_hud_draw(const Frame *f,ChVkTarget *target,const float vp[16],void *pixels,size_t bytes)
{
    float pc[3],pw,ph,c[3],r[3];
    const float right[3]={1,0,0};
    if(!s_vk_hud_valid || !s_vk_hud_tex || !hud_panel(f,pc,&pw,&ph)) return;
    yaw_rot(s_anchor_yaw,pc,c);yaw_rot(s_anchor_yaw,right,r);
    for(int k=0;k<3;k++) c[k]+=s_anchor_p[k];
    static const float corner[6][4]={{-1,1,0,0},{1,1,1,0},{-1,-1,0,1},{1,1,1,0},{1,-1,1,1},{-1,-1,0,1}};
    static const uint16_t idx[6]={0,1,2,3,4,5};
    ChVkColorVertex v[6]={0};
    for(int i=0;i<6;i++) {
        float p[3],clip[4];
        for(int k=0;k<3;k++) p[k]=c[k]+r[k]*pw*.5f*corner[i][0]+(k==1?ph*.5f*corner[i][1]:0);
        for(int k=0;k<4;k++) clip[k]=vp[k]*p[0]+vp[4+k]*p[1]+vp[8+k]*p[2]+vp[12+k];
        v[i].xy[0]=clip[0];v[i].xy[1]=-clip[1];v[i].xy[2]=(clip[2]+clip[3])*.5f;v[i].xy[3]=clip[3];
        v[i].xy[4]=corner[i][2];v[i].xy[5]=corner[i][3];
        v[i].rgba[0]=v[i].rgba[1]=v[i].rgba[2]=v[i].rgba[3]=1;
    }
    /* Straight alpha: the HUD's empty pixels hold colour (grey) at alpha 0. */
    const Nv2aRenderState over={.color_mask=0x01010101,.blend_enable=1,.blend_src=0x302,.blend_dst=0x303,.blend_eq=0x8006};
    vk_check(ch_vk_target_draw_texture_read(target,NULL,v,6,idx,6,&over,s_vk_hud_tex,pixels,bytes),"HUD draw");
}
/* "Show stats": HOTD4's panel - the game's frame rate, CPU (the process,
 * and the game thread per frame), the GPU, memory - painted twice a second
 * and shown head-locked in the top right, about 1 m out (its outer corner
 * about 22 and 14 degrees off centre). */
static ChVkTarget *s_vk_stats_target;
static ChVkXrQuad *s_vk_stats;
static int s_vk_stats_dirty,s_vk_stats_shown;
static void vk_stats_sample(void)
{
    static uint64_t since,frames0,samples;
    static double cpu0=-1;
    static char shown[LG_STATS_LINES][LG_STATS_LINE];
    static unsigned char pixels[XM_STATS_W*XM_STATS_H*4];
    uint64_t now=pace_now_us();
    if(!s_vr.show_stats) {since=0;s_vk_stats_shown=0;return;}
    if(!since) {since=now;frames0=s_frames_in;return;}
    if(now-since<500000) return;
    double elapsed=(double)(now-since);
    uint64_t frames=s_frames_in-frames0;
    LgStats stats={0};
    stats.fps=(int)(frames*1e6/elapsed+.5);
    stats.frame_ms=frames?(float)(elapsed/1000.0/frames):-1;
    if(s_vk_world_shown) {   /* the game's 3D: each eye's picture as drawn now */
        stats.eye_w=(int)(s_vk_world_w/2*s_vk_world_scale+.5f);
        stats.eye_h=(int)(s_vk_world_h*s_vk_world_scale+.5f);
        stats.scale_pct=(int)(s_vk_world_scale*100+.5f);
    } else {stats.eye_w=(int)s_vk_eye[0].w;stats.eye_h=(int)s_vk_eye[0].h;}   /* the screen, in the overlay */
    struct timespec ts;
    if(s_guest_clock_ok && clock_gettime(s_guest_clock,&ts)==0) {   /* the game thread's CPU time per frame */
        double cpu=ts.tv_sec*1e3+ts.tv_nsec/1e6;
        if(cpu0>=0 && frames) lg_stats_set_game_ms((float)((cpu-cpu0)/frames));
        cpu0=cpu;
    }
    lg_stats_sample(&stats);
    since=now;frames0=s_frames_in;
    char lines[LG_STATS_LINES][LG_STATS_LINE];
    lg_stats_format(&stats,lines);
    if(memcmp(lines,shown,sizeof lines)) {
        const char *rows[LG_STATS_LINES];
        for(int i=0;i<LG_STATS_LINES;i++) rows[i]=lines[i];
        xm_paint_lines(pixels,rows,LG_STATS_LINES);
        if(!s_vk_stats_target) vk_initialized_target(XM_STATS_W,XM_STATS_H,&s_vk_stats_target);
        vk_check(ch_vk_target_import_color(s_vk_stats_target,pixels,sizeof pixels),"stats upload");
        memcpy(shown,lines,sizeof lines);s_vk_stats_dirty=1;
        static int dumped;
        if(!dumped && samples>=4 && ch_env("XR_DUMP")) {dumped=1;dump_ppm("stats.ppm",pixels,XM_STATS_W,XM_STATS_H);}
    }
    if(samples++%10==0)     /* every 5 s: the same numbers in the log */
        fprintf(stderr,"[stats] %s | %s | %s | %s\n",lines[0],lines[1],lines[2],lines[3]);
    s_vk_stats_shown=1;
}
static const XrCompositionLayerBaseHeader *vk_stats_layer(void)
{
    VkResult result=VK_SUCCESS;
    if(!s_vr.show_stats || !s_vk_stats_shown || !s_vk_stats_target || !s_views_ok) return NULL;
    const float hw=.143f,hh=hw*XM_STATS_H/XM_STATS_W,at[3]={.41f-hw,.2567f-hh,-1};
    const XrQuaternionf q=s_views[0].pose.orientation;
    float off[3];
    qrot(q,at,off);
    XrPosef pose={.orientation=q,.position={
        .5f*(s_views[0].pose.position.x+s_views[1].pose.position.x)+off[0],
        .5f*(s_views[0].pose.position.y+s_views[1].pose.position.y)+off[1],
        .5f*(s_views[0].pose.position.z+s_views[1].pose.position.z)+off[2]}};
    XrExtent2Df size={hw*2,hh*2};
    if(!s_vk_stats) {
        vk_xr_check(ch_vk_xr_quad_create(s_inst,s_sess,xrGetInstanceProcAddr,s_vk.device,
            XM_STATS_W,XM_STATS_H,s_local,&pose,size,XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT|
            XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT,&s_vk_stats),"stats swapchain");
        s_vk_stats_dirty=1;
    }
    vk_xr_check(ch_vk_xr_quad_pose(s_vk_stats,&pose,size),"stats pose");
    if(s_vk_stats_dirty) {
        vk_xr_check(ch_vk_xr_quad_update(s_vk_stats,s_vk_stats_target,XR_INFINITE_DURATION,&result),"stats copy");
        vk_check(result,"stats copy completion");s_vk_stats_dirty=0;
    }
    return ch_vk_xr_quad_layer(s_vk_stats);
}
/* The gun, its beam and dot into an eye's overlay. Input the overlay
 * refuses (VK_ERROR_INITIALIZATION_FAILED, checked before each part is
 * drawn) costs that eye the rest of its gun for the frame, logged with what
 * it was given: it aborted the game with the settings panel open
 * (2026-11-03). Other failures stop. */
static void vk_overlay_draw(unsigned e,const float vp[16],const float eye[3],size_t bytes)
{
    s_qo.weapon=chihiro_profile()->vr.weapon;
    s_qo.scope_scale=qo_scope_scale();
    VkResult r=ch_vk_overlay_read(s_vk_eye[e].target,&s_qo,vp,eye,qo_sight_height(s_vr.sight),s_vk_eye[e].pixels,bytes);
    if(r!=VK_ERROR_INITIALIZATION_FAILED) {vk_check(r,"overlay draw");return;}
    static unsigned refused;
    if(refused++<8) {
        const float *q=s_qo.gun_q;
        fprintf(stderr,"[quest Vulkan] overlay draw refused (%u): eye %u |q|^2 %.6f kick %.3f gun %.3f %.3f %.3f "
            "end %.3f %.3f %.3f eye %.3f %.3f %.3f beam %d dot %d flash %d sight %d panel %d\n",
            refused,e,q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3],s_qo.kick,s_qo.gun_p[0],s_qo.gun_p[1],s_qo.gun_p[2],
            s_qo.end[0],s_qo.end[1],s_qo.end[2],eye[0],eye[1],eye[2],s_qo.beam,s_qo.dot,s_qo.flash,s_qo.sight,s_menu.open);
    }
}
/* The HUD spread toward the edges of the view, leaving the middle clear
 * (VC3_XR_HUD_SPREAD=0: one quad, as the game draws it). The picture goes in
 * pieces, each a quad layer of the one HUD swapchain (its subImage): the
 * corner groups -- score, magazine/weapon/lives, player 2's -- move out by
 * HUD_SPREAD_X of the panel's half width and HUD_SPREAD_Y of its half height
 * (times VC3_XR_HUD_SPREAD)
 * (at the usual lens their outer edges ~38 degrees to the side, ~26 up and
 * down), the gauge and credits down, the middle stays. The pieces tile the
 * picture: nothing is shown twice. The pieces are the profile's vr.hud_tiles
 * (rectangles in the game's 640x480 HUD, y down, cut round its layout; dx, dy:
 * which way the piece moves); none: the HUD is one quad. */
#define HUD_SPREAD_X .85f
#define HUD_SPREAD_Y .55f
static XrCompositionLayerQuad s_vk_hud_piece[10];
/* The pieces of the HUD's quad layer (base, posed at hp, pw x ph metres) into
 * layers; 0 when it is not to be spread (then the caller shows it whole). */
static unsigned vk_hud_pieces(const XrCompositionLayerBaseHeader *base,const XrPosef *hp,float pw,float ph,
    const XrCompositionLayerBaseHeader **layers)
{
    static float spread=-1;   /* VC3_XR_HUD_SPREAD=k: k times as far out (1 by default; 0: off) */
    if(spread<0) spread=ch_env("XR_HUD_SPREAD")?(float)atof(ch_env("XR_HUD_SPREAD")):1.f;
    const uint32_t tw=s_vk_hud_tw,th=s_vk_hud_th;
    const int pieces=chihiro_profile()->vr.hud_tile_count<10?chihiro_profile()->vr.hud_tile_count:10;
    if(!(spread>0) || !tw || tw*3!=th*4 || pieces<=0) return 0;   /* the game's 4:3 HUD only */
    const float right_unit[3]={1,0,0};float r[3];yaw_rot(s_anchor_yaw,right_unit,r);
    const float sx=(float)tw/640.f,sy=(float)th/480.f;
    const typeof(chihiro_profile()->vr.hud_tiles[0]) *k_hud_pieces=chihiro_profile()->vr.hud_tiles;
    for(int i=0;i<pieces;i++) {
        XrCompositionLayerQuad q=*(const XrCompositionLayerQuad *)base;
        const int32_t x0=(int32_t)(k_hud_pieces[i].x0*sx+.5f),y0=(int32_t)(k_hud_pieces[i].y0*sy+.5f);
        const int32_t x1=(int32_t)(k_hud_pieces[i].x1*sx+.5f),y1=(int32_t)(k_hud_pieces[i].y1*sy+.5f);
        q.subImage.imageRect=(XrRect2Di){{x0,y0},{x1-x0,y1-y0}};
        q.size=(XrExtent2Df){(float)(x1-x0)/(float)tw*pw,(float)(y1-y0)/(float)th*ph};
        /* the piece's centre from the panel's, metres: right, up */
        const float cx=((float)(x0+x1)*.5f/(float)tw-.5f)*pw+k_hud_pieces[i].dx*spread*HUD_SPREAD_X*pw*.5f;
        const float cy=(.5f-(float)(y0+y1)*.5f/(float)th)*ph-k_hud_pieces[i].dy*spread*HUD_SPREAD_Y*ph*.5f;
        q.pose=*hp;
        q.pose.position.x+=r[0]*cx;q.pose.position.y+=cy;q.pose.position.z+=r[2]*cx;
        s_vk_hud_piece[i]=q;
        layers[i]=(const XrCompositionLayerBaseHeader *)&s_vk_hud_piece[i];
    }
    return (unsigned)pieces;
}
static unsigned vk_present(const Frame *f,const XrCompositionLayerBaseHeader **layers,XrDuration display_period)
{
    unsigned count=0;
    VkResult result=VK_SUCCESS;
    XrPosef pose={.orientation={0,0,0,1}};
    float forward[3]={0,0,-s_dist},center[3];
    yaw_rot(s_anchor_yaw,forward,center);
    pose.orientation.y=sinf(s_anchor_yaw*.5f);pose.orientation.w=cosf(s_anchor_yaw*.5f);
    pose.position=(XrVector3f){center[0]+s_anchor_p[0],center[1]+s_anchor_p[1],center[2]+s_anchor_p[2]};
    int screen_new=0;
    const int imm=f && f->vr.imm;
    s_vk_world_shown=0;
    if(imm) {
        SEG(0,screen_new=vk_world_update(f));
        const XrCompositionLayerBaseHeader *world=vk_world_layer(f);
        if(world) {layers[count++]=world;s_vk_world_shown=1;}
        float pc[3],pw,ph;
        if(s_vk_hud_valid && s_vk_hud_quad && !vk_hud_in_overlay() && hud_panel(f,pc,&pw,&ph)) {
            float c[3];yaw_rot(s_anchor_yaw,pc,c);
            XrPosef hp={.orientation={0,sinf(s_anchor_yaw*.5f),0,cosf(s_anchor_yaw*.5f)},
                .position={c[0]+s_anchor_p[0],c[1]+s_anchor_p[1],c[2]+s_anchor_p[2]}};
            vk_xr_check(ch_vk_xr_quad_pose(s_vk_hud_quad,&hp,(XrExtent2Df){pw,ph}),"HUD pose");
            const XrCompositionLayerBaseHeader *hud=ch_vk_xr_quad_layer(s_vk_hud_quad);
            if(hud) {
                unsigned n=vk_hud_pieces(hud,&hp,pw,ph,layers+count);
                if(n) count+=n; else layers[count++]=hud;
            }
        }
    } else if(f && vk_screen_in_eyes()) screen_new=vk_screen_update(f);
    else if(f) {
        XrExtent2Df size={s_height*(float)f->lw/f->lh,s_height};
        if(s_vk_screen_w!=(uint32_t)f->w || s_vk_screen_h!=(uint32_t)f->h) {
            vk_xr_check(ch_vk_xr_quad_destroy(s_vk_screen),"old screen");s_vk_screen=NULL;
            vk_xr_check(ch_vk_xr_quad_create(s_inst,s_sess,xrGetInstanceProcAddr,s_vk.device,
                f->w,f->h,s_local,&pose,size,0,&s_vk_screen),"screen swapchain");
            s_vk_screen_w=f->w;s_vk_screen_h=f->h;s_vk_screen_sequence=0;
        }
        vk_xr_check(ch_vk_xr_quad_pose(s_vk_screen,&pose,size),"screen pose");
        if(s_vk_screen_sequence!=f->gpu.sequence) {
            if(s_vk_split)   /* the game's frame, finished, in its slot's shared buffer */
                vk_xr_check(ch_vk_xr_quad_update_buffer(s_vk_screen,s_vk.queue,s_vk.family,
                    s_vk_bridge.xr_buffer[f->gpu.slot],XR_INFINITE_DURATION,&result),"shared screen copy");
            else
                vk_xr_check(ch_vk_xr_quad_update_texture(s_vk_screen,f->gpu.image,XR_INFINITE_DURATION,&result),"GPU screen copy");
            vk_check(result,"screen copy completion");
            s_vk_screen_sequence=f->gpu.sequence;
        }
        const XrCompositionLayerBaseHeader *layer=ch_vk_xr_quad_layer(s_vk_screen);
        if(layer) layers[count++]=layer;
    }
    /* Match the GL host's high-refresh overlay cadence. Reuse both the image
     * and its render pose between draws; the compositor handles reprojection. */
    static int overlay_full=-1;
    if(overlay_full<0) overlay_full=ch_env("OVERLAY_FULLRATE") && *ch_env("OVERLAY_FULLRATE")=='1';
    s_vk_overlay_frame++;
    static int eye_test=-1,swap_eyes=-1;
    if(eye_test<0) eye_test=ch_env("XR_EYE_TEST") && *ch_env("XR_EYE_TEST")=='1';
    if(swap_eyes<0) swap_eyes=ch_env("XR_SWAP_EYES") && *ch_env("XR_SWAP_EYES")=='1';
    const int screen=!imm && s_vk_scr_tex && vk_screen_in_eyes();
    const int hud_shown=imm && s_vk_hud_valid && vk_hud_in_overlay();
    int overlay_want=s_overlay_ok>0 && s_views_ok && (s_qo.gun_valid || eye_test || screen || hud_shown);
    /* the rifle's lens with a picture follows the aim every display frame (vk_lens_crop) */
    const int lens_live=s_qo.gun_valid && s_qo.lens_fresh && chihiro_profile()->vr.weapon==CH_VR_RIFLE;
    int overlay_reuse=overlay_want && !screen_new && overlay_reuse_frame(s_vk_overlay_have_last,overlay_full || lens_live,
        s_menu.open,s_vk_overlay_frame,display_period);
    if(!overlay_want) s_vk_overlay_have_last=0;
    /* Diagnostics for the eye mapping: EYE_TEST tints the left eye's overlay
     * red and the right eye's blue; SWAP_EYES draws each eye's overlay from
     * the other view (the image still goes to the same layer slot). */
    if(overlay_want && !overlay_reuse) {
        VkCommandBuffer ocmd=VK_NULL_HANDLE;int recorded[2]={0,0};
        if(s_qo.gun_valid && chihiro_profile()->vr.weapon==CH_VR_RIFLE) vk_lens_crop();
        for(unsigned e=0;e<2;e++) {
            const XrView *v=&s_views[swap_eyes?1-e:e];
            float vp[16],eye[3]={v->pose.position.x,v->pose.position.y,v->pose.position.z};
            uint32_t index;VkImage image;
            size_t bytes=(size_t)s_vk_eye[e].w*s_vk_eye[e].h*4;
            static const float transparent[4]={0,0,0,0},tint[2][4]={{.35f,0,0,.35f},{0,0,.35f,.35f}};
            eye_viewproj(v,vp);
            /* the first clear initialises the target (synchronous); then
             * clears join the batch, not waited for */
            static int cleared[2];
            if(!cleared[e]) {
                vk_check(ch_vk_target_clear_read(s_vk_eye[e].target,transparent,s_vk_eye[e].pixels,bytes),"overlay first clear");
                cleared[e]=1;
            }
            SEG(1,vk_check(ch_vk_target_clear_aspects_read(s_vk_eye[e].target,1,eye_test?tint[e]:transparent,0,0,s_vk_eye[e].pixels,bytes),"overlay clear"));
            if(screen) vk_screen_draw(s_vk_eye[e].target,vp,s_vk_eye[e].pixels,bytes);
            if(hud_shown) SEG(2,vk_hud_draw(f,s_vk_eye[e].target,vp,s_vk_eye[e].pixels,bytes));
            if(s_qo.gun_valid) SEG(3,vk_overlay_draw(e,vp,eye,bytes));
            SEG(10,vk_xr_check(ch_vk_xr_swapchain_wait(s_vk_eye[e].chain,XR_INFINITE_DURATION,&index,&image),"overlay acquire"));
            if(!ocmd) ocmd=vk_host_begin();
            VkResult exported;
            SEG(11,exported=ch_vk_target_record_export(s_vk_eye[e].target,ocmd,image));
            if(exported==VK_SUCCESS) recorded[e]=1;
            else if(exported==VK_NOT_READY) {   /* the waiting export */
                vk_xr_check(ch_vk_xr_swapchain_copy_release(s_vk_eye[e].chain,s_vk_eye[e].target,NULL,0,&result),"overlay copy");
                vk_check(result,"overlay copy completion");
            } else vk_check(exported,"overlay export");
            s_vk_overlay_views[e]=(XrCompositionLayerProjectionView){
                .type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,.pose=v->pose,.fov=v->fov,
                .subImage={.swapchain=ch_vk_xr_swapchain_handle(s_vk_eye[e].chain),
                    .imageRect={.extent={(int32_t)s_vk_eye[e].w,(int32_t)s_vk_eye[e].h}}}};
        }
        if(ocmd) {
            vk_host_submit();
            for(unsigned e=0;e<2;e++) if(recorded[e])
                vk_xr_check(ch_vk_xr_swapchain_release_submitted(s_vk_eye[e].chain),"overlay release");
        }
        s_vk_overlay_layer=(XrCompositionLayerProjection){.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION,
            .layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,.space=s_local,
            .viewCount=2,.views=s_vk_overlay_views};
        s_vk_overlay_have_last=1;
    }
    if(overlay_want) layers[count++]=(const XrCompositionLayerBaseHeader *)&s_vk_overlay_layer;
    {
        const XrCompositionLayerBaseHeader *stats=vk_stats_layer();
        if(stats) layers[count++]=stats;
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
