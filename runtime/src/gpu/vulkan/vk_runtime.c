#include "vk_runtime.h"
#include "vk_backend.h"
#include "chihiro/ch_env.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

extern void xbox_FramebufferWindowPresentFrames(const void *const *,int,uint32_t,uint32_t,uint32_t,uint32_t);
static ChVkBackendConfig host_config;
static int has_host_config;
static struct {
    VkInstance instance;
    VkDevice device;
    ChVkBackend *backend;
    uint32_t *argb;
    size_t capacity;
    uint64_t frames;
} runtime;

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
    uint64_t start=first?strtoull(first,NULL,10):0,number=n?strtoull(n,NULL,10):1;
    if(dir && runtime.frames>=start && runtime.frames-start<number) {
        char path[4096];
        if(snprintf(path,sizeof path,"%s/frame-%06llu.ppm",dir,(unsigned long long)runtime.frames)>=(int)sizeof path)
            return VK_ERROR_INITIALIZATION_FAILED;
        FILE *file=fopen(path,"wb");
        if(!file) {
            fprintf(stderr,"[Vulkan] capture %s: %s\n",path,strerror(errno));
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        fprintf(file,"P6\n%u %u\n255\n",surface->width,surface->height);
        int failed=0;
        for(size_t i=0;i<count;i++) if(fwrite(src+4*i,1,3,file)!=3) {failed=1;break;}
        if(fclose(file)) failed=1;
        if(failed) return VK_ERROR_INITIALIZATION_FAILED;
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
    const char *resident=ch_env("VULKAN_RESIDENT_COLOR");
    config.resident_color=!resident || strcmp(resident,"0");
    const char *cache=ch_env("VULKAN_TEXTURE_CACHE");
    config.cache_textures=!cache || strcmp(cache,"0");
    fprintf(stderr,"[Vulkan] resident color targets: %s\n",config.resident_color?"enabled":"disabled");
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
    result=VK_ERROR_INITIALIZATION_FAILED;
    goto fail; /* Quest must use the OpenXR runtime-selected device. */
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
    if(runtime.device) vkDestroyDevice(runtime.device,NULL);
    if(runtime.instance) vkDestroyInstance(runtime.instance,NULL);
    free(runtime.argb);
    memset(&runtime,0,sizeof runtime);
}
