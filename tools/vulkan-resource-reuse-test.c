/* Real GPU lifetime checks for retained draw resources, with a same-device
 * synchronous allocation control. This is not a combat/FPS benchmark. */
#include "gpu/vulkan/vk_target.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double milliseconds(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec*1000.0+t.tv_nsec/1e6;
}
static int draw(ChVkTarget *target,ChVkColorVertex *vertices,uint32_t count,uint32_t color) {
    const uint16_t indices[]={0,1,2,0,2,3};
    const float black[]={0,0,0,0};unsigned char pixels[16*16*4];
    Nv2aRenderState state={.color_mask=0x01010101,.blend_enable=1,
        .blend_src=0x8001,.blend_dst=0,.blend_eq=0x8006,.blend_color=color};
    VkResult r=ch_vk_target_draw_state_read(target,black,vertices,count,indices,6,&state,pixels,sizeof pixels);
    if(r!=VK_SUCCESS) {fprintf(stderr,"resource draw failed: %d\n",r);return 1;}
    unsigned char expected[]={color>>16,color>>8,color,color>>24};
    for(unsigned i=0;i<sizeof pixels;i++) if(pixels[i]!=expected[i%4]) {
        fprintf(stderr,"resource draw byte %u: %u expected %u\n",i,pixels[i],expected[i%4]);return 1;
    }
    return 0;
}
static int run(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family,int reuse) {
    ChVkTarget *target=NULL;int failed=1;
    ChVkColorVertex *vertices=calloc(8192,sizeof *vertices);
    if(!vertices) return 1;
    const float xy[4][2]={{-1,-1},{1,-1},{1,1},{-1,1}};
    for(unsigned i=0;i<4;i++) {
        vertices[i].xy[0]=xy[i][0];vertices[i].xy[1]=xy[i][1];vertices[i].xy[3]=1;
        for(unsigned c=0;c<4;c++) vertices[i].rgba[c]=1;
    }
    setenv("CHIHIRO_VULKAN_REUSE_RESOURCES",reuse?"1":"0",1);
    if(ch_vk_target_create(gpu,device,queue,family,16,16,&target)!=VK_SUCCESS) goto done;
    double start=milliseconds();
    for(unsigned i=0;i<128;i++) if(draw(target,vertices,4,i%2?0xff00ff00:0xffff0000)) goto done;
    double elapsed=milliseconds()-start;
    ChVkTargetResourceStats stats;ch_vk_target_resource_stats(target,&stats);
    if(stats.pipeline_builds!=(reuse?2u:128u) || stats.geometry_allocations!=(reuse?1u:128u) ||
        stats.pipeline_hits!=(reuse?126u:0u)) {
        fprintf(stderr,"resource retention mismatch: reuse%d builds%llu allocations%llu hits%llu\n",reuse,
            (unsigned long long)stats.pipeline_builds,(unsigned long long)stats.geometry_allocations,
            (unsigned long long)stats.pipeline_hits);goto done;
    }
    /* Grow beyond the original mapping, then immediately return to small
     * draws. The retained buffer must not shrink/reallocate or retain stale data. */
    if(draw(target,vertices,8192,0xff00ff00) || draw(target,vertices,4,0xffff0000)) goto done;
    ch_vk_target_resource_stats(target,&stats);
    if(stats.geometry_allocations!=(reuse?2u:130u) ||
        (reuse && stats.geometry_capacity<8192*sizeof *vertices+12)) goto done;
    /* More than the bounded pipeline working set: exercise eviction and
     * rebuilding an old state after its resources have been destroyed. */
    for(unsigned i=1;i<=40;i++) if(draw(target,vertices,4,0xff000000|(i*5<<16))) goto done;
    if(draw(target,vertices,4,0xffff0000)) goto done;
    ch_vk_target_resource_stats(target,&stats);
    if(!stats.pipeline_evictions || (reuse && stats.pipeline_builds!=43)) goto done;
    printf("PASS: retained-resource control=%d: 128 alternating-state draws %.2fms, pipelines %u geometry allocations %u; growth/reuse and LRU eviction preserve every RGBA byte\n",
        !reuse,elapsed,reuse?2:128,reuse?1:128);
    failed=0;
done:
    ch_vk_target_destroy(target);free(vertices);return failed;
}
int ch_test_resource_reuse(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    const char *value=getenv("CHIHIRO_VULKAN_REUSE_RESOURCES");char *old=value?strdup(value):NULL;
    if(value && !old) return 1;
    int failed=run(gpu,device,queue,family,0);
    if(!failed) failed=run(gpu,device,queue,family,1);
    if(old) {setenv("CHIHIRO_VULKAN_REUSE_RESOURCES",old,1);free(old);}
    else unsetenv("CHIHIRO_VULKAN_REUSE_RESOURCES");
    return failed;
}
