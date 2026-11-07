#include "vk_runtime.h"
#ifdef CH_VK_SPIRV_OPT
#include "vk_spirv_specialize.h"
#endif
#include "vk_backend.h"
#include "vk_target.h"
#include "vk_fixed.h"
#include "vk_device_features.h"
#include "vk_state.h"
#include "vk_pipeline_cache_file.h"
#include "vk_interp.h"
#include "vk_gpu_mailbox.h"
#include "chihiro/ch_env.h"
#include "chihiro/chihiro_profile.h"
#include "host/vr_view.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <time.h>

extern void xbox_FramebufferWindowPresentFrames(const void *const *,int,uint32_t,uint32_t,uint32_t,uint32_t);
static ChVkBackendConfig host_config;
static int has_host_config;
static struct {
    VkInstance instance;
    VkDevice device;
    VkPhysicalDevice physical;VkQueue queue;uint32_t family;   /* the desktop device (interpolation) */
    ChVkBackend *backend;
    VkPipelineCache pipeline_cache;
    VkDevice cache_device;
    char *cache_path;
    uint64_t publish_ns;
    ChGuestWriteWatch *write_watch;
    uint32_t *argb;
    size_t capacity;
    unsigned char *capture_rgb;
    size_t capture_capacity;
    uint64_t frames;
} runtime;
static _Atomic(ChGuestWriteWatch *) fault_watch;
#if !defined(_WIN32)
static W32MemoryObserver write_observer;
static void write_begin(void *context,void *p,size_t n) {ch_guest_write_watch_begin(context,p,n);}
static void write_end(void *context,void *p,size_t n) {ch_guest_write_watch_end(context,p,n);}
static void write_invalidate(void *context,void *p,size_t n) {ch_guest_write_watch_invalidate(context,p,n);}
#endif
int ch_vk_runtime_write_fault(uintptr_t address,void *context)
{
    ChGuestWriteWatch *watch=atomic_load_explicit(&fault_watch,memory_order_acquire);
    return watch && ch_guest_write_watch_fault(watch,address,ch_guest_write_fault_is_write(context));
}

uint32_t xbox_VblankCount(void);
uint64_t xbox_VblankLastNs(void);
uint64_t xbox_HostNowNs(void);
static void fatal(VkResult result,const char *operation)
{
    fprintf(stderr,"[Vulkan] fatal %s: VkResult=%d, frame=%llu\n",operation,result,
            (unsigned long long)runtime.frames);
    fflush(stderr);
    abort();
}

static void save_pipeline_cache(void)
{
    if(runtime.pipeline_cache && runtime.cache_path)
        fprintf(stderr,"[Vulkan pipeline cache] save=%s\n",
            ch_vk_pipeline_cache_file_save(runtime.cache_device,runtime.pipeline_cache,runtime.cache_path)?"ok":"skipped");
}

/* The periodic save, off the render thread. Copying the cache out of the
 * driver, checksumming it and writing it (78 MB for HOTD3's chapter 1) took
 * 100-380 ms; on the render thread every save was a visible hitch, every
 * ~10 s while the cache grew. The cache is internally synchronized (not
 * created EXTERNALLY_SYNCHRONIZED), so reading it here while pipelines are
 * created elsewhere is allowed, and the file is written to a temporary name
 * and renamed, so a save at exit racing one here leaves a whole file. */
#if !defined(_WIN32)
#include <pthread.h>
static struct {
    pthread_mutex_t lock;pthread_cond_t wake;pthread_t thread;
    int started,request,stop;
    size_t saved;   /* the size last saved; the saver's own */
} saver={PTHREAD_MUTEX_INITIALIZER,PTHREAD_COND_INITIALIZER};
static void *saver_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&saver.lock);
    for(;;) {
        while(!saver.request && !saver.stop) pthread_cond_wait(&saver.wake,&saver.lock);
        if(saver.stop) break;
        saver.request=0;
        pthread_mutex_unlock(&saver.lock);
        size_t size=0;
        if(vkGetPipelineCacheData(runtime.cache_device,runtime.pipeline_cache,&size,NULL)==VK_SUCCESS && size>saver.saved) {
            save_pipeline_cache();saver.saved=size;
        }
        pthread_mutex_lock(&saver.lock);
    }
    pthread_mutex_unlock(&saver.lock);
    return NULL;
}
static void saver_request(void)
{
    pthread_mutex_lock(&saver.lock);
    if(!saver.started && !saver.stop) saver.started=pthread_create(&saver.thread,NULL,saver_main,NULL)==0;
    saver.request=1;
    pthread_cond_signal(&saver.wake);
    pthread_mutex_unlock(&saver.lock);
}
/* Before the cache is saved a last time and destroyed. */
static void saver_stop(void)
{
    pthread_mutex_lock(&saver.lock);
    int started=saver.started;
    saver.stop=1;
    pthread_cond_signal(&saver.wake);
    pthread_mutex_unlock(&saver.lock);
    if(started) pthread_join(saver.thread,NULL);
}
#endif

static void frame_completed(const Nv2aSurface *surface)
{
    runtime.frames++;
    if(ch_env("VULKAN_INTERVAL_LOG")) {
        static uint64_t previous;
        struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
        uint64_t now=(uint64_t)t.tv_sec*1000000000u+t.tv_nsec;
        /* vblanks since the last frame (1: none missed) and how long after
         * its vblank this frame was handed over (latency, not rate). */
        static uint32_t last_vblank;
        uint32_t vblank=xbox_VblankCount();uint64_t vblank_ns=xbox_VblankLastNs(),host=xbox_HostNowNs();
        if(previous) fprintf(stderr,"[Vulkan present intervals] frame=%llu interval_ms=%.3f vblanks=%u after_vblank_ms=%.3f publish_ms=%.3f\n",
            (unsigned long long)runtime.frames,(now-previous)/1e6,vblank-last_vblank,
            vblank_ns && host>vblank_ns?(host-vblank_ns)/1e6:-1.0,runtime.publish_ns/1e6);
        previous=now;last_vblank=vblank;
    }
    /* The cache was saved only on a clean exit, and a game closed from its
     * launcher (Steam) never has one: every session compiled every pipeline
     * again, mid-play. Every ~10 s, when the cache has grown, it is saved
     * (by the saver thread above, not this one). */
    if(runtime.pipeline_cache && runtime.cache_path && runtime.frames%600==0) {
#if !defined(_WIN32)
        saver_request();
#else
        static size_t saved;size_t size=0;
        if(vkGetPipelineCacheData(runtime.cache_device,runtime.pipeline_cache,&size,NULL)==VK_SUCCESS && size>saved) {
            save_pipeline_cache();saved=size;
        }
#endif
    }
    if(runtime.frames==1 || runtime.frames%60==0)
        fprintf(stderr,"[Vulkan] completed frame %llu: %ux%u, guest=%08X\n",
                (unsigned long long)runtime.frames,surface->width,surface->height,surface->color_va);
    const char *limit=ch_env("VULKAN_FRAME_LIMIT");
    if(limit && strtoull(limit,NULL,10)>0 && runtime.frames>=strtoull(limit,NULL,10)) {
        fprintf(stderr,"[Vulkan] diagnostic frame limit reached: %llu\n",(unsigned long long)runtime.frames);
        save_pipeline_cache();
        fflush(NULL);
        /* Not exit(): its teardown unmapped guest memory under the guest
         * threads still running, and the run often ended in a segfault. */
        _exit(EXIT_SUCCESS);
    }
}

static int capture_due(void)
{
    const char *dir=ch_env("VULKAN_DUMP_DIR"),*first=ch_env("VULKAN_DUMP_FIRST"),*n=ch_env("VULKAN_DUMP_COUNT");
    const char *step=ch_env("VULKAN_DUMP_EVERY");
    uint64_t start=first?strtoull(first,NULL,10):0,number=n?strtoull(n,NULL,10):1;
    uint64_t every=step?strtoull(step,NULL,10):0;
    return dir && runtime.frames>=start && (runtime.frames-start<number || (every && (runtime.frames-start)%every==0));
}
static VkResult capture_pixels(const Nv2aSurface *surface,const void *rgba,int bgra)
{
    if(!capture_due()) return VK_SUCCESS;
    size_t count=(size_t)surface->width*surface->height;
    const unsigned char *src=rgba;
    const char *dir=ch_env("VULKAN_DUMP_DIR");
    {
        size_t rgb_bytes=count*3;
        if(rgb_bytes>runtime.capture_capacity) {
            void *p=realloc(runtime.capture_rgb,rgb_bytes);
            if(!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
            runtime.capture_rgb=p;runtime.capture_capacity=rgb_bytes;
        }
        for(size_t i=0;i<count;i++) {
            runtime.capture_rgb[3*i]=src[4*i+(bgra?2:0)];runtime.capture_rgb[3*i+1]=src[4*i+1];runtime.capture_rgb[3*i+2]=src[4*i+(bgra?0:2)];
        }
        char path[4096];
        if(snprintf(path,sizeof path,"%s/frame-%06llu.ppm",dir,(unsigned long long)runtime.frames)>=(int)sizeof path)
            return VK_ERROR_INITIALIZATION_FAILED;
        char pending[4096];
        if(snprintf(pending,sizeof pending,"%s.tmp",path)>=(int)sizeof pending) return VK_ERROR_INITIALIZATION_FAILED;
        FILE *file=fopen(pending,"wb");
        if(!file) {
            fprintf(stderr,"[Vulkan] capture %s: %s\n",path,strerror(errno));
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        fprintf(file,"P6\n%u %u\n255\n",surface->width,surface->height);
        int failed=fwrite(runtime.capture_rgb,1,rgb_bytes,file)!=rgb_bytes;
        if(fclose(file)) failed=1;
        if(failed) {remove(pending);return VK_ERROR_INITIALIZATION_FAILED;}
        /* A watchdog may interrupt diagnostic runs: expose only complete PPMs. */
        if(rename(pending,path)) {remove(pending);return VK_ERROR_INITIALIZATION_FAILED;}
        fprintf(stderr,"[Vulkan] captured %s\n",path);
    }
    return VK_SUCCESS;
}

/* With each frame dump, the game's lens picture (vr_view.h; Silent Scope:
 * its 256x256 scope target) as lens-<frame>.ppm, to check it with no
 * headset. Nothing when the game has none (VC3); a failed dump is logged,
 * not fatal. */
static void capture_lens(void)
{
    const uint32_t va=vc3_vr_lens_surface();
    if(!va || !capture_due()) return;
    ChVkTarget *target=runtime.backend?ch_vk_backend_target(runtime.backend,va):NULL;
    static int found,missing;
    if(!target) {
        if(!missing++) fprintf(stderr,"[Vulkan] lens surface %08X: no target\n",va);
        return;
    }
    uint32_t w,h;ch_vk_target_size(target,&w,&h);
    if(!found++) fprintf(stderr,"[Vulkan] lens surface %08X: target %ux%u\n",va,w,h);
    size_t bytes=(size_t)w*h*4;
    unsigned char *pixels=malloc(bytes);
    ChVkTexture *copy=NULL;
    VkResult r=pixels?ch_vk_target_snapshot_texture(target,1,&copy):VK_ERROR_OUT_OF_HOST_MEMORY;
    if(r==VK_SUCCESS) r=ch_vk_texture_read_rgba(copy,pixels,bytes);
    if(r==VK_SUCCESS) {
        char path[4096];
        snprintf(path,sizeof path,"%s/lens-%06llu.ppm",ch_env("VULKAN_DUMP_DIR"),(unsigned long long)runtime.frames);
        FILE *f=fopen(path,"wb");
        if(f) {
            fprintf(f,"P6\n%u %u\n255\n",w,h);
            for(size_t i=0;i<(size_t)w*h;i++) fwrite(pixels+4*i,1,3,f);
            fclose(f);
        }
    } else fprintf(stderr,"[Vulkan] lens surface %08X: capture failed (VkResult=%d)\n",va,r);
    free(pixels);ch_vk_texture_destroy(copy);
}

/* GPU-native hosts own the image handoff. Keep frame accounting and cache
 * shutdown identical to the CPU presenter without materializing pixels. */
static VkResult present_target(void *unused,const Nv2aSurface *surface,ChVkTarget *target)
{
    (void)unused;
    VkResult r=host_config.present_target(host_config.render_context,surface,target);
    if(r!=VK_SUCCESS && r!=VK_NOT_READY) return r;
    /* Explicit screenshot diagnostics are the only presentation readback in
     * the GPU-host path. The normal path never allocates CPU frame pixels. */
    if(capture_due()) {
        ChVkTexture *copy=NULL;
        VkResult captured=ch_vk_target_snapshot_texture(target,1,&copy);
        size_t bytes=(size_t)surface->width*surface->height*4;
        void *pixels=captured==VK_SUCCESS?malloc(bytes):NULL;
        if(captured==VK_SUCCESS && !pixels) captured=VK_ERROR_OUT_OF_HOST_MEMORY;
        if(captured==VK_SUCCESS) captured=ch_vk_texture_read_rgba(copy,pixels,bytes);
        if(captured==VK_SUCCESS) captured=capture_pixels(surface,pixels,0);
        free(pixels);ch_vk_texture_destroy(copy);
        if(captured!=VK_SUCCESS) return captured;
    }
    frame_completed(surface);
    return r;
}

/* Immersive frames keep the same frame accounting (intervals, frame limit). */
static VkResult present_immersive(void *unused,const Nv2aSurface *surface,ChVkTarget *eyes,ChVkTarget *hud,ChVkTarget *lens,
    const ChVkImmFrame *frame)
{
    (void)unused;
    struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
    VkResult r=host_config.present_immersive(host_config.render_context,surface,eyes,hud,lens,frame);
    clock_gettime(CLOCK_MONOTONIC,&b);
    runtime.publish_ns=(uint64_t)(b.tv_sec-a.tv_sec)*1000000000u+(uint64_t)(b.tv_nsec-a.tv_nsec);
    if(r!=VK_SUCCESS && r!=VK_NOT_READY) return r;
    frame_completed(surface);
    return r;
}

/* CHIHIRO_IMMERSIVE_TEST (<prefix>_ too; host/vr_view_desktop.c: yaw,pitch,
 * fov,sway): the immersive view on the desktop, to check it with no headset.
 * As on the headset (quest_vulkan.inc), each frame goes through the GPU
 * mailbox -- here into host-visible buffers of this device -- and the game's
 * culling is widened (quest_host.c imm_cull: 140 degrees). With each frame
 * dump (CHIHIRO_VULKAN_DUMP_*) the frame's eyes, HUD and lens picture are
 * written as eyes-/hud-/lens-<frame>.ppm, from the mailbox's buffer. The
 * window keeps its last flat picture. Unset: off. Each eye 1024x1024, or
 * <prefix>_IMMERSIVE_TEST_SIZE=WxH. */
static struct {
    ChVkGpuMailbox *mailbox;VkDevice device;
    VkBuffer buffers[CH_VK_GPU_MAILBOX_SLOTS];VkDeviceMemory memory[CH_VK_GPU_MAILBOX_SLOTS];
    unsigned char *mapped[CH_VK_GPU_MAILBOX_SLOTS];
} imm_test;
static int imm_test_view(void *unused,Vc3VrView *view)
{
    (void)unused;
    const int on=vc3_vr_view_latch(view);
    if(chihiro_profile()->vr.cull) chihiro_profile()->vr.cull(on?70.0f*3.14159265f/180.0f:0.0f);
    return on;
}
static void dump_rgba(const char *kind,const unsigned char *rgba,uint32_t w,uint32_t h)
{
    char path[4096];
    snprintf(path,sizeof path,"%s/%s-%06llu.ppm",ch_env("VULKAN_DUMP_DIR"),kind,(unsigned long long)runtime.frames);
    FILE *f=fopen(path,"wb");
    if(!f) {fprintf(stderr,"[Vulkan] capture %s: %s\n",path,strerror(errno));return;}
    fprintf(f,"P6\n%u %u\n255\n",w,h);
    for(size_t i=0;i<(size_t)w*h;i++) fwrite(rgba+4*i,1,3,f);
    fclose(f);
}
static VkResult imm_test_present(void *unused,const Nv2aSurface *surface,ChVkTarget *eyes,ChVkTarget *hud,ChVkTarget *lens,
    const ChVkImmFrame *frame)
{
    (void)unused;
    ChVkGpuFrame f;memset(&f,0,sizeof f);
    VkResult r=ch_vk_gpu_mailbox_publish_immersive(imm_test.mailbox,surface,eyes,hud,lens,frame);
    if(r==VK_SUCCESS) r=ch_vk_gpu_mailbox_take(imm_test.mailbox,&f);
    if(r==VK_SUCCESS) r=ch_vk_gpu_frame_wait(&f);
    if(r==VK_SUCCESS && capture_due()) {
        const unsigned char *p=imm_test.mapped[f.slot];
        dump_rgba("eyes",p,f.imm.eye_width*2,f.imm.eye_height);
        if(f.imm.hud) dump_rgba("hud",p+f.hud_offset,f.surface.width,f.surface.height);
        if(f.imm.lens) dump_rgba("lens",p+f.imm.lens_offset,f.imm.lens_w,f.imm.lens_h);
        fprintf(stderr,"[Vulkan] immersive test frame %llu: eyes %ux%u, HUD %s, lens %s (%ux%u at %llu), shot meets the scene %s %.4f\n",
            (unsigned long long)runtime.frames,f.imm.eye_width,f.imm.eye_height,f.imm.hud?"yes":"no",
            f.imm.lens?"fresh":"0",f.imm.lens_w,f.imm.lens_h,(unsigned long long)f.imm.lens_offset,
            f.imm.aim_hit>0?"at":"nowhere",f.imm.aim_hit);
    }
    ch_vk_gpu_frame_release(&f);
    if(r!=VK_SUCCESS) return r;
    frame_completed(surface);
    return VK_SUCCESS;
}
static VkResult imm_test_setup(ChVkBackendConfig *config)
{
    uint32_t w=1024,h=1024;
    const char *size=ch_env("IMMERSIVE_TEST_SIZE");
    if(size && (sscanf(size,"%ux%u",&w,&h)!=2 || w<256 || h<256 || w>4096 || h>4096)) return VK_ERROR_INITIALIZATION_FAILED;
    const VkDeviceSize capacity=(VkDeviceSize)w*2*h*4+(VkDeviceSize)1280*960*4+(VkDeviceSize)1024*1024*4+512;
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(config->physical,&memory);
    const VkMemoryPropertyFlags want=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkResult r=ch_vk_gpu_mailbox_create(&imm_test.mailbox);
    imm_test.device=config->device;
    for(unsigned i=0;r==VK_SUCCESS && i<CH_VK_GPU_MAILBOX_SLOTS;i++) {
        VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=capacity,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        r=vkCreateBuffer(config->device,&bi,NULL,&imm_test.buffers[i]);
        if(r!=VK_SUCCESS) break;
        VkMemoryRequirements need;vkGetBufferMemoryRequirements(config->device,imm_test.buffers[i],&need);
        uint32_t type=0;
        while(type<memory.memoryTypeCount && (!(need.memoryTypeBits&(1u<<type)) || (memory.memoryTypes[type].propertyFlags&want)!=want)) type++;
        if(type==memory.memoryTypeCount) {r=VK_ERROR_FEATURE_NOT_PRESENT;break;}
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=need.size,.memoryTypeIndex=type};
        r=vkAllocateMemory(config->device,&ai,NULL,&imm_test.memory[i]);
        if(r==VK_SUCCESS) r=vkBindBufferMemory(config->device,imm_test.buffers[i],imm_test.memory[i],0);
        if(r==VK_SUCCESS) r=vkMapMemory(config->device,imm_test.memory[i],0,VK_WHOLE_SIZE,0,(void **)&imm_test.mapped[i]);
    }
    if(r!=VK_SUCCESS) return r;
    ch_vk_gpu_mailbox_set_buffers(imm_test.mailbox,imm_test.buffers,capacity);
    config->immersive_view=imm_test_view;config->present_immersive=imm_test_present;
    config->immersive_eye_width=w;config->immersive_eye_height=h;
    const ChihiroProfile *p=chihiro_profile();
    config->immersive_lens_k=p->vr.lens_k;
    config->immersive_program_projection=(VrGameProj){p->vr.program_proj.kx,p->vr.program_proj.ky,
        p->vr.program_proj.cx,p->vr.program_proj.cy,p->vr.program_proj.inv_pz};
    fprintf(stderr,"[Vulkan] immersive test view: each eye %ux%u, lens focal %.1f\n",w,h,config->immersive_lens_k);
    return VK_SUCCESS;
}
static void imm_test_stop(void)
{
    if(ch_vk_gpu_mailbox_destroy(imm_test.mailbox)!=VK_SUCCESS) return;
    for(unsigned i=0;i<CH_VK_GPU_MAILBOX_SLOTS;i++) {
        if(imm_test.buffers[i]) vkDestroyBuffer(imm_test.device,imm_test.buffers[i],NULL);
        if(imm_test.memory[i]) vkFreeMemory(imm_test.device,imm_test.memory[i],NULL);
    }
    memset(&imm_test,0,sizeof imm_test);
}

/* The display's 4:3 / 16:9 setting (host/display_settings.c): 16:9 draws
 * the game's 4:3 picture unchanged with the view continuing to the sides
 * (vk_wide.h). The window takes each frame's shape at its height; at 16:9 a
 * frame without a wide picture (a non-screen surface, the first frame) is
 * put in the middle of a 16:9 one, so the window keeps its shape. */
extern int vc3_wide_get(void);
extern void xbox_FramebufferWindowSetPicture(uint32_t,uint32_t);
static int wide_view(void *unused) {(void)unused;return vc3_wide_get();}
static const void *pillarbox(const Nv2aSurface *surface,const void *rgba,int bgra,uint32_t *width)
{
    static unsigned char *wide;static size_t capacity;
    uint32_t w=surface->width,h=surface->height,tw=((h*16+8)/9+1)&~1u;
    *width=w;
    if(has_host_config || !vc3_wide_get() || (uint64_t)w*9>=(uint64_t)h*16) return rgba;
    size_t need=(size_t)tw*h*4;
    if(need>capacity) {void *p=realloc(wide,need);if(!p) return rgba;wide=p;capacity=need;}
    (void)bgra;
    const uint32_t black=0xff000000u;   /* opaque black in RGBA and BGRA byte order alike */
    uint32_t off=(tw-w)/2;
    for(uint32_t y=0;y<h;y++) {
        uint32_t *row=(uint32_t *)(wide+(size_t)y*tw*4);
        for(uint32_t x=0;x<off;x++) row[x]=black;
        memcpy(row+off,(const unsigned char *)rgba+(size_t)y*w*4,(size_t)w*4);
        for(uint32_t x=off+w;x<tw;x++) row[x]=black;
    }
    *width=tw;
    return wide;
}
/* Display frame interpolation (vk_interp.h): a title presenting every
 * other vblank (Silent Scope: 30 fps) is shown at the display's rate (the
 * fps setting, 60 by default) with frames made between its own, as the
 * OpenGL renderer does above 60. Made per flip: the display's rate times
 * the swap's interval over 60, less the real one -- 1 for Silent Scope at
 * 60, none for VC3. A real frame reaches the window one made frame later
 * (~17 ms at 60). Only between consecutive flips of one size: after a gap
 * the frame kept is not the one before. CHIHIRO_VULKAN_INTERP=0: off. */
#define INTERP_MAX 3
extern int vc3_fps_get(void);
extern uint32_t nv2a_pb_exec_swap_interval(void);
static struct {
    ChVkInterp *p;int failed,logged;
    unsigned char *prev,*mid;size_t capacity;
    uint32_t w,h;uint64_t at,presents;
} interp;
static int interp_count(void)
{
    static int off=-1;
    if(off<0) {const char *e=ch_env("VULKAN_INTERP");off=e && !strcmp(e,"0");}
    uint32_t interval=nv2a_pb_exec_swap_interval();
    if(off || interval<1 || interval>4) return 0;
    int n=vc3_fps_get()*(int)interval/60-1;
    return n<0?0:n>INTERP_MAX?INTERP_MAX:n;
}
static int interpolate(const void *rgba,uint32_t w,uint32_t h,const void **planes)
{
    size_t bytes=(size_t)w*h*4;
    int n=interp_count(),made=0;
    interp.presents++;
    if(n<1) return 0;
    if(!interp.p && !interp.failed) {
        VkResult r=runtime.physical?ch_vk_interp_create(runtime.physical,runtime.device,runtime.queue,runtime.family,
            runtime.pipeline_cache,&interp.p):VK_ERROR_INITIALIZATION_FAILED;
        if(r!=VK_SUCCESS) {interp.failed=1;fprintf(stderr,"[Vulkan] frame interpolation unavailable (VkResult=%d): real frames only\n",r);return 0;}
    }
    if(!interp.p) return 0;
    if(bytes*(1+INTERP_MAX)>interp.capacity) {
        void *a=realloc(interp.prev,bytes),*b=realloc(interp.mid,bytes*INTERP_MAX);
        if(a) interp.prev=a;
        if(b) interp.mid=b;
        if(!a || !b) return 0;
        interp.capacity=bytes*(1+INTERP_MAX);interp.at=0;
    }
    if(interp.at && interp.at+1==interp.presents && interp.w==w && interp.h==h) {
        static uint64_t ns,count;
        struct timespec t0,t1;clock_gettime(CLOCK_MONOTONIC,&t0);
        for(int i=0;i<n;i++) {
            unsigned char *out=interp.mid+bytes*(size_t)i;
            VkResult r=ch_vk_interp_make(interp.p,interp.prev,rgba,w,h,(float)(i+1)/(float)(n+1),out);
            if(r!=VK_SUCCESS) {fprintf(stderr,"[Vulkan] frame interpolation failed (VkResult=%d)\n",r);break;}
            planes[made++]=out;
        }
        clock_gettime(CLOCK_MONOTONIC,&t1);
        ns+=(uint64_t)(t1.tv_sec-t0.tv_sec)*1000000000u+(uint64_t)(t1.tv_nsec-t0.tv_nsec);count+=made;
        if(made && !interp.logged++)
            fprintf(stderr,"[Vulkan] frame interpolation: %d made per flip (display %d fps, swap interval %u)\n",
                n,vc3_fps_get(),nv2a_pb_exec_swap_interval());
        if(count && interp.presents%300==0)
            fprintf(stderr,"[Vulkan] frame interpolation: %llu made, %.2f ms each\n",(unsigned long long)count,ns/1e6/count);
        /* CHIHIRO_VULKAN_INTERP_DUMP=dir (diagnostic): every 150th, the
         * earlier frame, the made one and the later, as PPMs. */
        const char *dir=ch_env("VULKAN_INTERP_DUMP");
        if(dir && made && interp.presents%150==0) {
            const unsigned char *img[3]={interp.prev,planes[0],rgba};
            for(int k=0;k<3;k++) {
                char path[512];snprintf(path,sizeof path,"%s/interp-%06llu-%d.ppm",dir,(unsigned long long)interp.presents,k);
                FILE *f=fopen(path,"wb");if(!f) continue;
                fprintf(f,"P6\n%u %u\n255\n",w,h);
                for(size_t i=0;i<(size_t)w*h;i++) fwrite(img[k]+i*4,1,3,f);
                fclose(f);
            }
        }
    }
    memcpy(interp.prev,rgba,bytes);interp.w=w;interp.h=h;interp.at=interp.presents;
    return made;
}
static VkResult present_pixels(void *unused,const Nv2aSurface *surface,const void *rgba,size_t bytes,int bgra)
{
    (void)unused;
    size_t count=(size_t)surface->width*surface->height;
    if(bytes!=count*4) return VK_ERROR_INITIALIZATION_FAILED;
    Nv2aSurface shaped=*surface;
    if(!has_host_config) {
        uint32_t w;
        rgba=pillarbox(surface,rgba,bgra,&w);
        shaped.width=w;count=(size_t)w*surface->height;
        static uint32_t picture_w,picture_h;
        if(w!=picture_w || surface->height!=picture_h) {
            xbox_FramebufferWindowSetPicture(w,surface->height);
            picture_w=w;picture_h=surface->height;
        }
        surface=&shaped;
    }
    if(!has_host_config) {
        /* The frames made between the last one and this (interpolate), then
         * this one: the window spreads them over the flip interval. */
        const void *planes[INTERP_MAX+1];
        int n=interpolate(rgba,surface->width,surface->height,planes);
        planes[n++]=rgba;
        if(!bgra) {
            size_t need=count*4*(size_t)n;
            if(need>runtime.capacity) {
                void *p=realloc(runtime.argb,need);
                if(!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
                runtime.argb=p;runtime.capacity=need;
            }
            for(int k=0;k<n;k++) {
                const unsigned char *src=planes[k];uint32_t *dst=runtime.argb+count*(size_t)k;
                for(size_t i=0;i<count;i++)
                    dst[i]=((uint32_t)src[4*i+3]<<24)|((uint32_t)src[4*i]<<16)|((uint32_t)src[4*i+1]<<8)|src[4*i+2];
                planes[k]=dst;
            }
        }
        xbox_FramebufferWindowPresentFrames(planes,n,surface->width,surface->height,surface->width*4,4);
    } else if(host_config.present_rgba) {
        VkResult result=host_config.present_rgba(host_config.render_context,surface,rgba,bytes);
        if(result!=VK_SUCCESS) return result;
    }
    VkResult capture_result=capture_pixels(surface,rgba,bgra);
    if(capture_result!=VK_SUCCESS) return capture_result;
    capture_lens();
    frame_completed(surface);
    return VK_SUCCESS;
}
static VkResult present(void *context,const Nv2aSurface *surface,const void *pixels,size_t bytes) {
    return present_pixels(context,surface,pixels,bytes,0);
}
static VkResult present_bgra(void *context,const Nv2aSurface *surface,const void *pixels,size_t bytes) {
    return present_pixels(context,surface,pixels,bytes,1);
}


int ch_vk_runtime_configure(const ChVkBackendConfig *config)
{
    if(runtime.backend || runtime.instance || runtime.device) return 0;
    if(config && (!config->physical || !config->device || !config->queue ||
       (!!config->lock_queue != !!config->unlock_queue))) return 0;
    if(config) host_config=*config;
    else memset(&host_config,0,sizeof host_config);
    has_host_config=config!=NULL;
    return 1;
}

static VkResult attach(ChVkBackendConfig config)
{
    config.guest_base=(void *)xbox_GetMemoryOffset();
    config.guest_bytes=(size_t)0x80000000u+g_xbox_total_ram;
    config.decode=nv2a_backend_decode_texture;
    const char *tracking=ch_env("VULKAN_WRITE_WATCH");
#if !defined(_WIN32)
    /* Use exact dirty tracking in ordinary builds as well as benchmarks.
     * The legacy diagnostic watch owns the same fault path; keep it explicit. */
    if(!tracking && !getenv("RECOMP_WATCH")) tracking="1";
#endif
    if(tracking && strcmp(tracking,"0")) {
        if(strcmp(tracking,"1") || getenv("RECOMP_WATCH")) return VK_ERROR_INITIALIZATION_FAILED;
#if !defined(_WIN32)
        void *bases[2];size_t sizes[2];ChGuestWriteView views[2];
        size_t count=xbox_GetContiguousViews(bases,sizes,2);
        if(!count) return VK_ERROR_FEATURE_NOT_PRESENT;
        for(size_t i=0;i<count;i++) views[i]=(ChGuestWriteView){bases[i],sizes[i]};
        runtime.write_watch=ch_guest_write_watch_create(views,count);
        if(!runtime.write_watch) return VK_ERROR_FEATURE_NOT_PRESENT;
        write_observer=(W32MemoryObserver){.context=runtime.write_watch,.begin_write=write_begin,
            .end_write=write_end,.invalidate=write_invalidate};
        atomic_store_explicit(&fault_watch,runtime.write_watch,memory_order_release);
        w32_set_memory_observer(&write_observer);
        config.guest_write_watch=runtime.write_watch;
        fprintf(stderr,"[Vulkan] exact write tracking: %zu contiguous backing view(s), unsupported pages compare exactly\n",count);
#else
        return VK_ERROR_FEATURE_NOT_PRESENT;
#endif
    }
    const char *resident=ch_env("VULKAN_RESIDENT_COLOR");
    config.resident_color=!resident || strcmp(resident,"0");
    const char *cache=ch_env("VULKAN_TEXTURE_CACHE");
    config.cache_textures=!cache || strcmp(cache,"0");
    const char *gpu_render_textures=ch_env("VULKAN_GPU_RENDER_TEXTURES");
    config.sample_render_targets=gpu_render_textures?atoi(gpu_render_textures)!=0:
        config.resident_color && config.cache_textures;
    const char *completion_depth=ch_env("VULKAN_COMPLETION_DEPTH");
    /* Default: as the OpenGL renderer, nothing writes VC3's depth back at
     * completions (no title code reads it); =1 publishes it there again. */
    config.completion_keep_depth=!completion_depth || atoi(completion_depth)==0;
    if(config.completion_keep_depth) fprintf(stderr,"[Vulkan] guest completions keep resident depth on the GPU\n");
    const char *lazy=ch_env("VULKAN_LAZY_COMPLETION");
    /* Default on, as the OpenGL renderer (which has no completion hook at
     * all): VC3 never reads rendered pixels back from the CPU, and waiting
     * for the GPU at each kickoff kept E.S. mode under 60 fps on the Frame.
     * =0 waits and publishes at every completion again. */
    config.lazy_completion=!lazy || atoi(lazy)!=0;
    if(config.lazy_completion) fprintf(stderr,"[Vulkan] lazy guest completion: enabled\n");
    const char *lazy_flip=ch_env("VULKAN_LAZY_FLIP");
    config.lazy_flip=lazy_flip?atoi(lazy_flip)!=0:chihiro_profile()->gpu.lazy_flip;
    if(config.lazy_flip) fprintf(stderr,"[Vulkan] lazy flips: enabled\n");
    const char *entries=ch_env("VULKAN_TEXTURE_CACHE_ENTRIES");
    if(entries) {
        char *end;errno=0;unsigned long value=strtoul(entries,&end,10);
        if(errno || end==entries || *end || value<1 || value>4096)
            return VK_ERROR_INITIALIZATION_FAILED;
        config.texture_cache_entries=(uint32_t)value;
    }
    const char *budget=ch_env("VULKAN_TEXTURE_CACHE_MIB");
    if(budget) {
        char *end;errno=0;unsigned long value=strtoul(budget,&end,10);
        if(errno || end==budget || *end || value<1 || value>1024)
            return VK_ERROR_INITIALIZATION_FAILED;
        config.texture_cache_bytes=(size_t)value<<20;
    } else if(config.immersive_view && !config.texture_cache_bytes) {
        /* Flat draws sample other surfaces' eye images (a snapshot costs
         * both eyes twice over: image and staging); too small a budget pins
         * them and decodes everything else on the CPU every draw. E.S.
         * samples several surfaces' eyes, each pinned for a frame or two:
         * room for twelve besides the game's textures. */
        size_t snapshot=(size_t)config.immersive_eye_width*2*config.immersive_eye_height*4*2;
        config.texture_cache_bytes=((size_t)128<<20)+12*snapshot;
    }
    fprintf(stderr,"[Vulkan] texture cache budget: %zuMiB\n",
        config.texture_cache_bytes?config.texture_cache_bytes>>20:(size_t)64);
    const char *pack=ch_env("VULKAN_GPU_DEPTH_PACK");
    uint32_t queue_count=0;vkGetPhysicalDeviceQueueFamilyProperties(config.physical,&queue_count,NULL);
    VkQueueFamilyProperties *queues=calloc(queue_count,sizeof *queues);
    if(!queues) return VK_ERROR_OUT_OF_HOST_MEMORY;
    vkGetPhysicalDeviceQueueFamilyProperties(config.physical,&queue_count,queues);
    config.gpu_depth_pack=(!pack || atoi(pack)) && config.family<queue_count &&
        (queues[config.family].queueFlags&VK_QUEUE_COMPUTE_BIT);
    const char *decode=ch_env("VULKAN_GPU_TEXTURE_DECODE");
    config.gpu_texture_decode=(!decode || atoi(decode)) && config.family<queue_count &&
        (queues[config.family].queueFlags&VK_QUEUE_COMPUTE_BIT);
    const char *color_pack=ch_env("VULKAN_GPU_COLOR_PACK");
    config.gpu_color_pack=(!color_pack || atoi(color_pack)) && config.family<queue_count &&
        (queues[config.family].queueFlags&VK_QUEUE_COMPUTE_BIT);
    const char *size_eviction=ch_env("VULKAN_SIZE_AWARE_EVICTION");
    config.size_aware_texture_eviction=!size_eviction || atoi(size_eviction)!=0;
    const char *morton_depth=ch_env("VULKAN_GPU_MORTON_DEPTH");
    config.gpu_morton_depth=(!morton_depth || atoi(morton_depth)) && config.gpu_depth_pack;
    const char *morton_color=ch_env("VULKAN_GPU_MORTON_COLOR");
    config.gpu_morton_color=(!morton_color || atoi(morton_color)) && config.gpu_color_pack;
    free(queues);
    fprintf(stderr,"[Vulkan] D24/D32 guest depth GPU conversion: %s\n",config.gpu_depth_pack?"enabled":"disabled");
    const char *fixed=ch_env("VULKAN_GPU_FIXED");
    if(fixed && !strcmp(fixed,"0")) config.raw_fixed=0;
    fprintf(stderr,"[Vulkan] raw fixed/program GPU transforms: %s\n",config.raw_fixed?"enabled":"disabled");
    const char *batch=ch_env("VULKAN_BATCH_DRAWS");
    config.batch_draws=!batch || strcmp(batch,"0");
    fprintf(stderr,"[Vulkan] resident command batches: %s\n",config.batch_draws?"enabled":"disabled");
    const char *batch_limit=ch_env("VULKAN_BATCH_LIMIT");
    if(batch_limit) {
        char *end;errno=0;unsigned long value=strtoul(batch_limit,&end,10);
        if(errno || end==batch_limit || *end || value<1 || value>CH_VK_MAX_BATCH_DRAWS)
            return VK_ERROR_INITIALIZATION_FAILED;
        config.batch_draw_limit=(uint32_t)value;
    } else config.batch_draw_limit=64; /* with the batch ring the GPU starts on a frame's draws while the rest are recorded */
    fprintf(stderr,"[Vulkan] command batch draw limit: %u\n",config.batch_draw_limit?config.batch_draw_limit:CH_VK_DEFAULT_BATCH_DRAWS);
    const char *depth=ch_env("VULKAN_RESIDENT_DEPTH");
    config.resident_depth=depth?!strcmp(depth,"1"):config.gpu_depth_pack;
    const char *depth_copy=ch_env("VULKAN_DEPTH_GPU_COPY");
    config.strict_depth_switches=depth_copy && !strcmp(depth_copy,"0");
    const char *clean_depth=ch_env("VULKAN_REUSE_CLEAN_DEPTH");
    config.reuse_clean_depth=clean_depth && atoi(clean_depth)!=0;
    const char *multiple_depth=ch_env("VULKAN_MULTIPLE_DEPTH");
    config.multiple_depth=multiple_depth?atoi(multiple_depth)!=0:
        config.resident_depth && !config.reuse_clean_depth;
    const char *clear_skip=ch_env("VULKAN_DEPTH_CLEAR_SKIP");
    config.strict_depth_clears=clear_skip && !strcmp(clear_skip,"0");
    const char *quant=ch_env("VULKAN_QUANTIZED_DEPTH");
    config.quantized_depth=quant?!strcmp(quant,"1"):config.resident_depth;
    const char *quant_draw=ch_env("VULKAN_DEPTH_QUANT_DRAW");
    config.depth_quant_draw=quant_draw && !strcmp(quant_draw,"1");
    const char *quant_fragment=ch_env("VULKAN_DEPTH_QUANT_FRAGMENT");
    config.depth_quant_fragment=quant_fragment && !strcmp(quant_fragment,"1");
    const char *quant_inline=ch_env("VULKAN_DEPTH_QUANT_INLINE");
    config.depth_quant_inline=quant_inline && !strcmp(quant_inline,"1");
    if(!quant_fragment && !quant_inline && !quant_draw)
        config.depth_quant_fragment=config.quantized_depth;
    const char *packed=ch_env("VULKAN_DEPTH_PACKED");
    config.depth_float_transfers=packed && !strcmp(packed,"0");
    fprintf(stderr,"[Vulkan] resident color targets: %s\n",config.resident_color?"enabled":"disabled");
    fprintf(stderr,"[Vulkan] immutable GPU render-target sampling: %s\n",config.sample_render_targets?"enabled":"disabled");
    fprintf(stderr,"[Vulkan] exact quantized D32 depth residency: %s\n",config.quantized_depth?(config.depth_quant_fragment?"native-style fragment rounding":config.depth_quant_inline?"same-subpass input draw":config.depth_quant_draw?"depth-only draw":"compute"):"disabled");
    fprintf(stderr,"[Vulkan] resident D24 depth targets: %s\n",config.resident_depth?"enabled":"disabled");
    fprintf(stderr,"[Vulkan] retained dirty depth owners: %s\n",config.multiple_depth?"enabled":"disabled");
    const char *texture_watch=ch_env("VULKAN_TEXTURE_WRITE_WATCH");
    config.strict_texture_checks=texture_watch && !strcmp(texture_watch,"0");
    config.present_rgba=present;
    if(config.present_target) config.present_target=present_target;
    if(config.present_immersive && has_host_config) config.present_immersive=present_immersive;
    if(!config.immersive_lens) config.immersive_lens=vc3_vr_lens_surface;
    if(!config.immersive_lens_aim) config.immersive_lens_aim=chihiro_profile()->vr.lens_aim;
    if(!config.immersive_flat_now) config.immersive_flat_now=chihiro_profile()->vr.flat_now;
    /* The lens picture four times the game's size each way while immersive
     * (on the rifle's much bigger lens the game's 256 looked soft, and the 2D
     * scope is three times that lens: 1024 keeps it as sharp);
     * CHIHIRO_VULKAN_LENS_SCALE=k (1: the game's own) */
    if(!(config.immersive_lens_scale>0)) {
        const char *e=ch_env("VULKAN_LENS_SCALE");
        config.immersive_lens_scale=e && atof(e)>=1 && atof(e)<=4?(float)atof(e):4.0f;
    }
    if(!has_host_config || !host_config.present_rgba) config.present_bgra=present_bgra;
    if(!config.fatal) config.fatal=fatal;
    const char *cache_path=ch_env("VULKAN_PIPELINE_CACHE");
    if(!config.pipeline_cache) {
        const char *save_dir=ch_env("SAVE_DIR");
        if(cache_path && *cache_path) {
            runtime.cache_path=malloc(strlen(cache_path)+1);
            if(!runtime.cache_path) return VK_ERROR_OUT_OF_HOST_MEMORY;
            strcpy(runtime.cache_path,cache_path);
        } else if(!cache_path && save_dir && *save_dir) {
            size_t n=strlen(save_dir)+32;runtime.cache_path=malloc(n);
            if(!runtime.cache_path) return VK_ERROR_OUT_OF_HOST_MEMORY;
            snprintf(runtime.cache_path,n,"%s/vulkan-pipelines.cache",save_dir);
        }
        runtime.cache_device=config.device;
        size_t packaged=0,loaded=0;
        VkResult cache_result=ch_vk_pipeline_cache_packaged_load(config.physical,config.device,
            &runtime.pipeline_cache,&packaged);
        if(cache_result!=VK_SUCCESS) return cache_result;
        if(runtime.cache_path) {
            VkPipelineCache disk=VK_NULL_HANDLE;
            cache_result=ch_vk_pipeline_cache_file_load(config.physical,config.device,runtime.cache_path,&disk,&loaded);
            if(cache_result==VK_SUCCESS && loaded) cache_result=vkMergePipelineCaches(config.device,runtime.pipeline_cache,1,&disk);
            if(disk) vkDestroyPipelineCache(config.device,disk,NULL);
            if(cache_result!=VK_SUCCESS) return cache_result;
        }
        config.pipeline_cache=runtime.pipeline_cache;
#if !defined(_WIN32)
        /* What is on disk already counts as saved: a session that adds no
         * pipeline writes nothing (it rewrote the whole 78 MB at its first
         * check, and at each check after where the driver's size moved). */
        if(loaded) {size_t size=0;if(vkGetPipelineCacheData(config.device,runtime.pipeline_cache,&size,NULL)==VK_SUCCESS) saver.saved=size;}
#endif
        fprintf(stderr,"[Vulkan pipeline cache] packaged_bytes=%zu loaded_bytes=%zu path=%s\n",packaged,loaded,
            runtime.cache_path?runtime.cache_path:"(memory only)");
        /* Pipeline recipes beside the cache: what earlier runs built,
         * precompiled while the game boots (CHIHIRO_VULKAN_PRECOMPILE_THREADS,
         * 4 by default; 0 records only). */
        ch_vk_vertex_program_loop_init(config.physical);
        if(runtime.cache_path) {
            size_t n=strlen(runtime.cache_path);char *recipes=malloc(n+16);
            if(recipes) {
                memcpy(recipes,runtime.cache_path,n+1);
                char *dot=strrchr(recipes,'.'),*slash=strrchr(recipes,'/');
                if(dot && (!slash || dot>slash)) *dot=0;
                strcat(recipes,".recipes");
#ifdef CH_VK_SPIRV_OPT
                {   /* CPU-specialized shaders beside them */
                    char *spec=malloc(strlen(recipes)+16);
                    if(spec) {strcpy(spec,recipes);strcpy(strrchr(spec,'.'),".specialized");
                        ch_vk_spirv_specialize_cache_open(spec);free(spec);}
                }
#endif
                const char *e=getenv("CHIHIRO_VULKAN_PRECOMPILE_THREADS");
                ch_vk_target_recipes_start(config.device,runtime.pipeline_cache,recipes,e?(unsigned)atoi(e):4u);
                free(recipes);
            }
        }
    }
    VkResult result=ch_vk_backend_create(&config,&runtime.backend);
    if(result==VK_SUCCESS) result=ch_vk_backend_attach(runtime.backend);
    return result;
}

int ch_vk_runtime_start(void)
{
    if(runtime.backend || runtime.instance || runtime.device) return 0;
    VkResult result;
    if(has_host_config) {
        result=attach(host_config);
        if(result!=VK_SUCCESS) goto fail;
        fprintf(stderr,"[Vulkan] direct NV2A backend on host-selected Vulkan device; OpenGL excluded from this build\n");
        return 1;
    }
#ifdef __ANDROID__
    const char *offscreen=ch_env("VULKAN_OFFSCREEN");
    if(!offscreen || strcmp(offscreen,"1")) {
        result=VK_ERROR_INITIALIZATION_FAILED;
        goto fail; /* Live Quest XR must use the runtime-selected device. */
    }
#endif
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName="Chihiro direct Vulkan",.apiVersion=VK_API_VERSION_1_0};
    VkInstanceCreateInfo info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
#if defined(__APPLE__)
    /* MoltenVK is a portability implementation: the loader lists it only for
     * an instance that asks for portability devices. (KosmicKrisp is a
     * conformant driver and is listed either way.) */
    const char *instance_extensions[1];
    {
        uint32_t n=0;
        VkExtensionProperties *p=NULL;
        if(vkEnumerateInstanceExtensionProperties(NULL,&n,NULL)==VK_SUCCESS && n && (p=calloc(n,sizeof *p)) &&
           vkEnumerateInstanceExtensionProperties(NULL,&n,p)==VK_SUCCESS)
            for(uint32_t i=0;i<n;i++) if(!strcmp(p[i].extensionName,"VK_KHR_portability_enumeration")) {
                instance_extensions[0]="VK_KHR_portability_enumeration";
                info.enabledExtensionCount=1;info.ppEnabledExtensionNames=instance_extensions;
                info.flags|=0x00000001u;   /* VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR */
                break;
            }
        free(p);
    }
#endif
    result=vkCreateInstance(&info,NULL,&runtime.instance);
    if(result!=VK_SUCCESS) goto fail;
    uint32_t count=0;
    result=vkEnumeratePhysicalDevices(runtime.instance,&count,NULL);
    if(result!=VK_SUCCESS || !count) {result=VK_ERROR_INITIALIZATION_FAILED;goto fail;}
    VkPhysicalDevice *devices=calloc(count,sizeof *devices);
    if(!devices) {result=VK_ERROR_OUT_OF_HOST_MEMORY;goto fail;}
    result=vkEnumeratePhysicalDevices(runtime.instance,&count,devices);
    VkPhysicalDevice physical=VK_NULL_HANDLE;
    uint32_t family=0;
    for(uint32_t d=0;result==VK_SUCCESS && d<count && !physical;d++) {
        uint32_t families=0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[d],&families,NULL);
        VkQueueFamilyProperties *queues=calloc(families,sizeof *queues);
        if(!queues) {result=VK_ERROR_OUT_OF_HOST_MEMORY;break;}
        vkGetPhysicalDeviceQueueFamilyProperties(devices[d],&families,queues);
        for(uint32_t q=0;q<families;q++) if(queues[q].queueCount && (queues[q].queueFlags&VK_QUEUE_GRAPHICS_BIT)) {
            physical=devices[d];family=q;break;
        }
        free(queues);
    }
    free(devices);
    if(result!=VK_SUCCESS || !physical) {if(result==VK_SUCCESS) result=VK_ERROR_INITIALIZATION_FAILED;goto fail;}
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceFeatures supported,enabled={0};
    vkGetPhysicalDeviceProperties(physical,&properties);
    vkGetPhysicalDeviceFeatures(physical,&supported);
    enabled.occlusionQueryPrecise=supported.occlusionQueryPrecise;
    enabled.fillModeNonSolid=supported.fillModeNonSolid;
    enabled.shaderClipDistance=supported.shaderClipDistance;enabled.depthClamp=supported.depthClamp;
    ch_vk_sampler_anisotropy_enable(physical,&enabled);
    float priority=1;
    VkDeviceQueueCreateInfo queue_info={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    const char *device_extensions[5];uint32_t device_extension_count=0;ChVkShaderFeatures shader_features;
    int ubo_layout=ch_vk_shader_features(physical,&shader_features,device_extensions,&device_extension_count,4);
    if(!ubo_layout) {enabled.shaderClipDistance=VK_FALSE;}   /* no GPU transforms (raw_fixed) */
#if defined(__APPLE__)
    {
        /* A portability device (MoltenVK) must be created with its subset
         * extension enabled. */
        uint32_t n=0;
        VkExtensionProperties *p=NULL;
        if(vkEnumerateDeviceExtensionProperties(physical,NULL,&n,NULL)==VK_SUCCESS && n && (p=calloc(n,sizeof *p)) &&
           vkEnumerateDeviceExtensionProperties(physical,NULL,&n,p)==VK_SUCCESS)
            for(uint32_t i=0;i<n;i++) if(!strcmp(p[i].extensionName,"VK_KHR_portability_subset")) {
                device_extensions[device_extension_count++]="VK_KHR_portability_subset";
                break;
            }
        free(p);
    }
#endif
    VkDeviceCreateInfo device_info={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&shader_features.ubo_layout,
        .queueCreateInfoCount=1,.pQueueCreateInfos=&queue_info,.pEnabledFeatures=&enabled,
        .enabledExtensionCount=device_extension_count,.ppEnabledExtensionNames=device_extensions};
    result=vkCreateDevice(physical,&device_info,NULL,&runtime.device);
    if(result!=VK_SUCCESS) goto fail;
    VkQueue queue;
    vkGetDeviceQueue(runtime.device,family,0,&queue);
    runtime.physical=physical;runtime.queue=queue;runtime.family=family;
    ChVkBackendConfig config={.physical=physical,.device=runtime.device,.queue=queue,.family=family,
        .guest_base=(void *)xbox_GetMemoryOffset(),.guest_bytes=(size_t)0x80000000u+g_xbox_total_ram,
        .decode=nv2a_backend_decode_texture,.precise_queries=enabled.occlusionQueryPrecise,
        .raw_fixed=enabled.shaderClipDistance && enabled.depthClamp,
        .present_rgba=present,.fatal=fatal,.wide_view=wide_view};
    if(ch_env("IMMERSIVE_TEST") && config.raw_fixed && (result=imm_test_setup(&config))!=VK_SUCCESS) goto fail;
    result=attach(config);
    if(result!=VK_SUCCESS) goto fail;
    fprintf(stderr,"[Vulkan] direct NV2A backend on %s; OpenGL excluded from this build\n",properties.deviceName);
    return 1;
fail:
    fprintf(stderr,"[Vulkan] initialization failed: VkResult=%d; guest not started\n",result);
    ch_vk_runtime_stop();
    return 0;
}

void ch_vk_runtime_stop(void)
{
    ch_vk_target_recipes_stop();
    ch_vk_backend_destroy(runtime.backend);
    imm_test_stop();
    ch_vk_interp_destroy(interp.p);interp.p=NULL;
    free(interp.prev);free(interp.mid);
#if !defined(_WIN32)
    saver_stop();
#endif
    save_pipeline_cache();
    if(runtime.pipeline_cache) vkDestroyPipelineCache(runtime.cache_device,runtime.pipeline_cache,NULL);
    free(runtime.cache_path);
    if(runtime.write_watch) {
#if !defined(_WIN32)
        w32_set_memory_observer(NULL);
#endif
        atomic_store_explicit(&fault_watch,NULL,memory_order_release);
        if(!ch_guest_write_watch_destroy(runtime.write_watch)) fatal(VK_ERROR_UNKNOWN,"write tracker still has active leases at shutdown");
    }
    if(runtime.device) vkDestroyDevice(runtime.device,NULL);
    if(runtime.instance) vkDestroyInstance(runtime.instance,NULL);
    free(runtime.argb);
    free(runtime.capture_rgb);
    memset(&runtime,0,sizeof runtime);
}
