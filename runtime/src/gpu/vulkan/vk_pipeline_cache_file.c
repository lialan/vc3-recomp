#include "vk_pipeline_cache_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#define CACHE_LIMIT (64u*1024u*1024u)
static const unsigned char magic[8]={'C','H','V','K','P','C',0,1};
static uint64_t read64(const unsigned char *p) {
    uint64_t n=0;for(unsigned i=0;i<8;i++) n|=(uint64_t)p[i]<<(8*i);return n;
}
#if !defined(_WIN32)
static void write64(unsigned char *p,uint64_t n) {
    for(unsigned i=0;i<8;i++) p[i]=(unsigned char)(n>>(8*i));
}
#endif
static uint64_t checksum(const unsigned char *p,size_t n) {
    uint64_t h=UINT64_C(14695981039346656037);
    for(size_t i=0;i<n;i++) h=(h^p[i])*UINT64_C(1099511628211);
    return h;
}
static void *read_cache(VkPhysicalDevice physical,const char *path,size_t *size) {
    *size=0;
    if(!path || !*path) return NULL;
    FILE *file=fopen(path,"rb");if(!file) return NULL;
    unsigned char envelope[24];void *blob=NULL;
    if(fread(envelope,1,sizeof envelope,file)!=sizeof envelope || memcmp(envelope,magic,8)) goto done;
    uint64_t bytes=read64(envelope+8);
    if(bytes<sizeof(VkPipelineCacheHeaderVersionOne) || bytes>CACHE_LIMIT) goto done;
    blob=malloc((size_t)bytes);if(!blob) goto done;
    if(fread(blob,1,(size_t)bytes,file)!=(size_t)bytes || fgetc(file)!=EOF || ferror(file) ||
       checksum(blob,(size_t)bytes)!=read64(envelope+16)) goto invalid;
    VkPipelineCacheHeaderVersionOne header;memcpy(&header,blob,sizeof header);
    VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(physical,&properties);
    if(header.headerSize!=sizeof header || header.headerVersion!=VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
       header.vendorID!=properties.vendorID || header.deviceID!=properties.deviceID ||
       memcmp(header.pipelineCacheUUID,properties.pipelineCacheUUID,VK_UUID_SIZE)) goto invalid;
    *size=(size_t)bytes;goto done;
invalid:
    free(blob);blob=NULL;
done:
    fclose(file);return blob;
}
VkResult ch_vk_pipeline_cache_file_load(VkPhysicalDevice physical,VkDevice device,
    const char *path,VkPipelineCache *cache,size_t *loaded) {
    if(loaded) *loaded=0;
    if(!physical || !device || !cache) return VK_ERROR_INITIALIZATION_FAILED;
    *cache=VK_NULL_HANDLE;
    size_t size=0;void *blob=read_cache(physical,path,&size);
    VkPipelineCacheCreateInfo info={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize=size,.pInitialData=blob};
    VkResult result=vkCreatePipelineCache(device,&info,NULL,cache);
    /* A valid old cache is only an optimization. Retry without it on a driver
     * rejection, including revisions sharing a UUID but rejecting the payload. */
    if(result!=VK_SUCCESS && size) {
        *cache=VK_NULL_HANDLE;info.initialDataSize=0;info.pInitialData=NULL;size=0;
        result=vkCreatePipelineCache(device,&info,NULL,cache);
    }
    free(blob);if(result==VK_SUCCESS && loaded) *loaded=size;return result;
}
int ch_vk_pipeline_cache_file_save(VkDevice device,VkPipelineCache cache,const char *path) {
#if defined(_WIN32)
    (void)device;(void)cache;(void)path;return 0;
#else
    if(!device || !cache || !path || !*path) return 0;
    size_t size=0;
    if(vkGetPipelineCacheData(device,cache,&size,NULL)!=VK_SUCCESS || size<32 || size>CACHE_LIMIT) return 0;
    void *blob=malloc(size);if(!blob) return 0;
    if(vkGetPipelineCacheData(device,cache,&size,blob)!=VK_SUCCESS || size<sizeof(VkPipelineCacheHeaderVersionOne)) {free(blob);return 0;}
    unsigned char envelope[24];memcpy(envelope,magic,8);
    write64(envelope+8,size);write64(envelope+16,checksum(blob,size));
    size_t length=strlen(path);
    if(length>SIZE_MAX-16) {free(blob);return 0;}
    char *pending=malloc(length+16);if(!pending) {free(blob);return 0;}
    snprintf(pending,length+16,"%s.tmp.XXXXXX",path);
    int fd=mkstemp(pending),ok=0;
    if(fd>=0) {
        FILE *file=fdopen(fd,"wb");
        if(file) {
            ok=fwrite(envelope,1,sizeof envelope,file)==sizeof envelope && fwrite(blob,1,size,file)==size;
            if(fclose(file)) ok=0;
            if(ok && rename(pending,path)) ok=0;
        } else close(fd);
        if(!ok) unlink(pending);
    }
    free(pending);free(blob);return ok;
#endif
}
