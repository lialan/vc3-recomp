#include "vk_runtime.h"
#include "vk_backend.h"
#include "chihiro/ch_env.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

extern void xbox_FramebufferWindowPresentFrames(const void *const *,int,uint32_t,uint32_t,uint32_t,uint32_t);
static ChVkBackendConfig host_config;
static int has_host_config;
static struct {
    VkInstance instance;
    VkDevice device;
    ChVkBackend *backend;
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

static void fatal(VkResult result,const char *operation)
{
    fprintf(stderr,"[Vulkan] fatal %s: VkResult=%d, frame=%llu\n",operation,result,
            (unsigned long long)runtime.frames);
    fflush(stderr);
    abort();
}

static VkResult present(void *unused,const Nv2aSurface *surface,const void *rgba,size_t bytes)
{
    (void)unused;
    size_t count=(size_t)surface->width*surface->height;
    if(bytes!=count*4) return VK_ERROR_INITIALIZATION_FAILED;
    if(!has_host_config) {
        if(bytes>runtime.capacity) {
            void *p=realloc(runtime.argb,bytes);
            if(!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
            runtime.argb=p;runtime.capacity=bytes;
        }
        const unsigned char *src=rgba;
        for(size_t i=0;i<count;i++)
            runtime.argb[i]=((uint32_t)src[4*i+3]<<24)|((uint32_t)src[4*i]<<16)
                |((uint32_t)src[4*i+1]<<8)|src[4*i+2];
        const void *planes[]={runtime.argb};
        xbox_FramebufferWindowPresentFrames(planes,1,surface->width,surface->height,surface->width*4,4);
    } else if(host_config.present_rgba) {
        VkResult result=host_config.present_rgba(host_config.render_context,surface,rgba,bytes);
        if(result!=VK_SUCCESS) return result;
    }
    const unsigned char *src=rgba;
    const char *dir=ch_env("VULKAN_DUMP_DIR"),*first=ch_env("VULKAN_DUMP_FIRST"),*n=ch_env("VULKAN_DUMP_COUNT");
    const char *step=ch_env("VULKAN_DUMP_EVERY");
    uint64_t start=first?strtoull(first,NULL,10):0,number=n?strtoull(n,NULL,10):1;
    uint64_t every=step?strtoull(step,NULL,10):0;
    if(dir && runtime.frames>=start && (runtime.frames-start<number || (every && (runtime.frames-start)%every==0))) {
        size_t rgb_bytes=count*3;
        if(rgb_bytes>runtime.capture_capacity) {
            void *p=realloc(runtime.capture_rgb,rgb_bytes);
            if(!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
            runtime.capture_rgb=p;runtime.capture_capacity=rgb_bytes;
        }
        for(size_t i=0;i<count;i++) {
            runtime.capture_rgb[3*i]=src[4*i];runtime.capture_rgb[3*i+1]=src[4*i+1];runtime.capture_rgb[3*i+2]=src[4*i+2];
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
    runtime.frames++;
    if(runtime.frames==1 || runtime.frames%60==0)
        fprintf(stderr,"[Vulkan] completed frame %llu: %ux%u, guest=%08X\n",
                (unsigned long long)runtime.frames,surface->width,surface->height,surface->color_va);
    const char *limit=ch_env("VULKAN_FRAME_LIMIT");
    if(limit && strtoull(limit,NULL,10)>0 && runtime.frames>=strtoull(limit,NULL,10)) {
        fprintf(stderr,"[Vulkan] diagnostic frame limit reached: %llu\n",(unsigned long long)runtime.frames);
        fflush(NULL);
        exit(EXIT_SUCCESS);
    }
    return VK_SUCCESS;
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
    config.sample_render_targets=gpu_render_textures && atoi(gpu_render_textures);
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
    }
    fprintf(stderr,"[Vulkan] texture cache budget: %zuMiB\n",
        config.texture_cache_bytes?config.texture_cache_bytes>>20:(size_t)64);
    const char *fixed=ch_env("VULKAN_GPU_FIXED");
    if(fixed && !strcmp(fixed,"0")) config.raw_fixed=0;
    fprintf(stderr,"[Vulkan] raw fixed-function GPU transforms: %s\n",config.raw_fixed?"enabled":"disabled");
    const char *batch=ch_env("VULKAN_BATCH_DRAWS");
    config.batch_draws=!batch || strcmp(batch,"0");
    fprintf(stderr,"[Vulkan] resident command batches: %s\n",config.batch_draws?"enabled":"disabled");
    const char *batch_limit=ch_env("VULKAN_BATCH_LIMIT");
    if(batch_limit) {
        char *end;errno=0;unsigned long value=strtoul(batch_limit,&end,10);
        if(errno || end==batch_limit || *end || value<1 || value>CH_VK_MAX_BATCH_DRAWS)
            return VK_ERROR_INITIALIZATION_FAILED;
        config.batch_draw_limit=(uint32_t)value;
    }
    fprintf(stderr,"[Vulkan] command batch draw limit: %u\n",config.batch_draw_limit?config.batch_draw_limit:CH_VK_MAX_BATCH_DRAWS);
    const char *depth=ch_env("VULKAN_RESIDENT_DEPTH");
    config.resident_depth=depth && !strcmp(depth,"1");
    const char *depth_copy=ch_env("VULKAN_DEPTH_GPU_COPY");
    config.strict_depth_switches=depth_copy && !strcmp(depth_copy,"0");
    const char *clear_skip=ch_env("VULKAN_DEPTH_CLEAR_SKIP");
    config.strict_depth_clears=clear_skip && !strcmp(clear_skip,"0");
    const char *quant=ch_env("VULKAN_QUANTIZED_DEPTH");
    config.quantized_depth=quant && !strcmp(quant,"1");
    const char *packed=ch_env("VULKAN_DEPTH_PACKED");
    config.depth_float_transfers=packed && !strcmp(packed,"0");
    fprintf(stderr,"[Vulkan] resident color targets: %s\n",config.resident_color?"enabled":"disabled");
    fprintf(stderr,"[Vulkan] immutable GPU render-target sampling: %s\n",config.sample_render_targets?"enabled":"disabled");
    fprintf(stderr,"[Vulkan] exact quantized D32 depth residency: %s\n",config.quantized_depth?"enabled":"disabled");
    fprintf(stderr,"[Vulkan] resident D24 depth targets: %s\n",config.resident_depth?"enabled":"disabled");
    config.present_rgba=present;
    if(!config.fatal) config.fatal=fatal;
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
    float priority=1;
    VkDeviceQueueCreateInfo queue_info={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo device_info={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount=1,.pQueueCreateInfos=&queue_info,.pEnabledFeatures=&enabled};
    result=vkCreateDevice(physical,&device_info,NULL,&runtime.device);
    if(result!=VK_SUCCESS) goto fail;
    VkQueue queue;
    vkGetDeviceQueue(runtime.device,family,0,&queue);
    ChVkBackendConfig config={.physical=physical,.device=runtime.device,.queue=queue,.family=family,
        .guest_base=(void *)xbox_GetMemoryOffset(),.guest_bytes=(size_t)0x80000000u+g_xbox_total_ram,
        .decode=nv2a_backend_decode_texture,.precise_queries=enabled.occlusionQueryPrecise,
        .raw_fixed=enabled.shaderClipDistance && enabled.depthClamp,
        .present_rgba=present,.fatal=fatal};
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
    ch_vk_backend_destroy(runtime.backend);
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
