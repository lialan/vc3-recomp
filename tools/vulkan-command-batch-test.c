/* Real GPU batching/lifetime tests; no headset session or game required. */
#include "vk_nv2a.h"
#include "vk_texture_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static unsigned char memory[128];static unsigned decodes;
static int decode(const Nv2aTexture *t,uint32_t *argb) {
    if(t->width!=1 || t->height!=1 || t->offset>sizeof memory-4) return 0;
    memcpy(argb,memory+t->offset,4);decodes++;return 1;
}
static double now_ms(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000.0+t.tv_nsec/1e6;}
int ch_test_command_batches(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    const char *old_profile=getenv("CHIHIRO_VULKAN_PROFILE");char *saved_profile=old_profile?strdup(old_profile):NULL;
    if(old_profile && !saved_profile) return 1;
    setenv("CHIHIRO_VULKAN_PROFILE","1",1);
    ChVkTarget *t=NULL;ChVkTextureCache *cache=NULL;int failed=1;
    unsigned char pixels[16*16*4];const float black[]={0,0,0,0};
    ChVkColorVertex v[]={{{-1,-1,0,1},{0,0,0,0}},{{1,-1,0,1},{0,0,0,0}},{{1,1,0,1},{0,0,0,0}},{{-1,1,0,1},{0,0,0,0}}};
    const uint16_t indices[]={0,1,2,0,2,3};
    Nv2aRenderState add={.color_mask=0x01010101,.blend_enable=1,.blend_src=1,.blend_dst=1,.blend_eq=0x8006};
#define REQUIRE(x) do {if(!(x)) {fprintf(stderr,"command batch failure line%d: %s\n",__LINE__,#x);goto done;}}while(0)
    for(unsigned batched=0;batched<4;batched++) {
        REQUIRE(ch_vk_target_create(gpu,device,queue,family,16,16,&t)==VK_SUCCESS);
        unsigned limit=batched==1?1:batched==2?64:256,submissions=(514+limit-1)/limit;
        REQUIRE(ch_vk_target_batch_limit(t,257)==VK_ERROR_INITIALIZATION_FAILED);
        REQUIRE(ch_vk_target_batch_limit(t,0)==VK_SUCCESS);
        REQUIRE(ch_vk_target_batch_limit(t,limit)==VK_SUCCESS);
        REQUIRE(ch_vk_target_batching(t,batched)==VK_SUCCESS);
        REQUIRE(ch_vk_target_enable_queries(t)==VK_SUCCESS);
        REQUIRE(ch_vk_target_clear_read(t,black,pixels,sizeof pixels)==VK_SUCCESS);
        ch_vk_target_defer_color_readback(t,1);
        REQUIRE(ch_vk_target_draw_state_read(t,NULL,v,4,indices,6,&add,pixels,sizeof pixels)==VK_SUCCESS);
        REQUIRE(ch_vk_target_finish(t)==VK_SUCCESS);
        REQUIRE(ch_vk_target_batch_limit(t,limit)==VK_ERROR_INITIALIZATION_FAILED);
        REQUIRE(ch_vk_target_clear_read(t,black,pixels,sizeof pixels)==VK_SUCCESS);
        ChVkTargetResourceStats before;ch_vk_target_resource_stats(t,&before);
        ChVkTargetTimingStats timing_before;ch_vk_target_timing_stats(t,&timing_before);
        memset(pixels,0xa5,sizeof pixels);double start=now_ms();
        for(unsigned i=0;i<514;i++) {
            for(unsigned k=0;k<4;k++) {memset(v[k].rgba,0,sizeof v[k].rgba);v[k].rgba[i%3]=1/255.f;v[k].rgba[3]=1/255.f;}
            REQUIRE(ch_vk_target_draw_state_read(t,NULL,v,4,indices,6,&add,pixels,sizeof pixels)==VK_SUCCESS);
            if(batched) REQUIRE(ch_vk_target_pending(t)>0 && ch_vk_target_completion_fence(t)==VK_NULL_HANDLE);
        }
        for(unsigned i=0;i<sizeof pixels;i++) REQUIRE(pixels[i]==0xa5);
        REQUIRE(ch_vk_target_finish(t)==VK_SUCCESS && !ch_vk_target_pending(t));
        double elapsed=now_ms()-start;
        REQUIRE(vkGetFenceStatus(device,ch_vk_target_completion_fence(t))==VK_SUCCESS);
        ChVkTargetResourceStats stats;ch_vk_target_resource_stats(t,&stats);
        REQUIRE(stats.batch_draws-before.batch_draws==(batched?514u:0u) && stats.batch_submissions-before.batch_submissions==(batched?submissions:0u));
        ChVkTargetTimingStats timing;ch_vk_target_timing_stats(t,&timing);
        if(timing.available) {
            REQUIRE(timing.batch_submissions-timing_before.batch_submissions==(batched?submissions:0u));
            REQUIRE(timing.batch_draws-timing_before.batch_draws==(batched?514u:0u));
            REQUIRE(timing.sync_render_submissions-timing_before.sync_render_submissions==(batched?0u:514u));
            if(batched) REQUIRE(timing.batch_record_ns>timing_before.batch_record_ns && timing.batch_finish_ns>timing_before.batch_finish_ns &&
                timing.batch_wait_ns<=timing.batch_finish_ns && timing.batch_retire_ns<=timing.batch_finish_ns);
            puts("PASS: completed GPU timestamp samples cover every synchronous draw or command batch with exact submission/draw counts and host phase totals");
        } else puts("SKIP: queue has no GPU timestamp support");
        REQUIRE(ch_vk_target_read_color(t,pixels,sizeof pixels)==VK_SUCCESS);
        const unsigned char expected[]={172,171,171,255};
        for(unsigned i=0;i<sizeof pixels;i++) REQUIRE(pixels[i]==expected[i%4]);
        printf("PASS: command batch control=%u limit=%u:514 changing-geometry draws %.2fms, batch submissions=%llu, exact additive pixels and pending fence ownership\n",!batched,limit,elapsed,(unsigned long long)(stats.batch_submissions-before.batch_submissions));
        if(!batched) {ch_vk_target_destroy(t);t=NULL;continue;}
        /* A query is an immediate completion boundary even on a resident target. */
        for(unsigned k=0;k<4;k++) for(unsigned c=0;c<4;c++) v[k].rgba[c]=1;
        Nv2aRenderState opaque={.color_mask=0x01010101};uint32_t samples=0;
        REQUIRE(ch_vk_target_draw_state_read(t,NULL,v,4,indices,6,&opaque,pixels,sizeof pixels)==VK_SUCCESS);
        REQUIRE(ch_vk_target_draw_count_read(t,NULL,v,4,indices,6,&opaque,NULL,&samples,pixels,sizeof pixels)==VK_SUCCESS);
        REQUIRE(samples==256 && !ch_vk_target_pending(t));
        REQUIRE(ch_vk_target_draw_state_read(t,NULL,v,4,indices,6,&opaque,pixels,sizeof pixels)==VK_SUCCESS);
        ChVkColorVertex *large=calloc(40000,sizeof *large);REQUIRE(large!=NULL);
        memcpy(large,v,sizeof v);
        for(unsigned k=0;k<4;k++) {large[k].rgba[0]=large[k].rgba[2]=0;large[k].rgba[1]=large[k].rgba[3]=1;}
        VkResult grown=ch_vk_target_draw_state_read(t,NULL,large,40000,indices,6,&opaque,pixels,sizeof pixels);
        free(large);REQUIRE(grown==VK_SUCCESS);
        for(unsigned k=0;k<4;k++) {v[k].rgba[0]=v[k].rgba[3]=1;v[k].rgba[1]=v[k].rgba[2]=0;}
        REQUIRE(ch_vk_target_draw_state_read(t,NULL,v,4,indices,6,&opaque,pixels,sizeof pixels)==VK_SUCCESS);
        REQUIRE(ch_vk_target_read_color(t,pixels,sizeof pixels)==VK_SUCCESS);
        for(unsigned i=0;i<sizeof pixels;i++) REQUIRE(pixels[i]==((i%4==0 || i%4==3)?255:0));
        ch_vk_target_resource_stats(t,&stats);REQUIRE(stats.geometry_allocations==2);
        for(unsigned k=0;k<4;k++) for(unsigned c=0;c<4;c++) v[k].rgba[c]=1;
        /* More states than the LRU capacity must finish before eviction. */
        static const uint32_t sources[]={0,1,0x300,0x301,0x302};
        static const uint32_t destinations[]={0,1,0x300,0x301,0x302,0x303,0x304,0x305};
        opaque.blend_enable=1;opaque.blend_eq=0x8006;
        for(unsigned i=0;i<40;i++) {opaque.blend_src=sources[i/8];opaque.blend_dst=destinations[i%8];REQUIRE(ch_vk_target_draw_state_read(t,NULL,v,4,indices,6,&opaque,pixels,sizeof pixels)==VK_SUCCESS);}
        ch_vk_target_resource_stats(t,&stats);REQUIRE(stats.pipeline_evictions>0);
        const float blue[]={0,0,1,1};
        REQUIRE(ch_vk_target_clear_read(t,blue,pixels,sizeof pixels)==VK_SUCCESS && !ch_vk_target_pending(t));
        REQUIRE(ch_vk_target_read_color(t,pixels,sizeof pixels)==VK_SUCCESS);
        for(unsigned i=0;i<sizeof pixels;i++) REQUIRE(pixels[i]==(i%4>=2?255:0));
        REQUIRE(ch_vk_target_draw_state_read(t,NULL,v,4,indices,6,&opaque,pixels,sizeof pixels)==VK_SUCCESS);
        for(unsigned i=0;i<sizeof pixels;i++) pixels[i]=(i%4==1 || i%4==3)?255:0;
        REQUIRE(ch_vk_target_import_color(t,pixels,sizeof pixels)==VK_SUCCESS && !ch_vk_target_pending(t));
        memset(pixels,0,sizeof pixels);REQUIRE(ch_vk_target_read_color(t,pixels,sizeof pixels)==VK_SUCCESS);
        for(unsigned i=0;i<sizeof pixels;i++) REQUIRE(pixels[i]==((i%4==1 || i%4==3)?255:0));
        float depth[256];unsigned char stencil[256];
        for(unsigned i=0;i<256;i++) {depth[i]=.5f;stencil[i]=(unsigned char)i;}
        REQUIRE(ch_vk_target_draw_state_read(t,NULL,v,4,indices,6,&opaque,pixels,sizeof pixels)==VK_SUCCESS);
        REQUIRE(ch_vk_target_write_depth_stencil(t,depth,stencil,256)==VK_SUCCESS && !ch_vk_target_pending(t));
        opaque.color_mask=0;opaque.depth_test_enable=opaque.depth_write=1;opaque.depth_func=0x207;
        REQUIRE(ch_vk_target_draw_state_read(t,NULL,v,4,indices,6,&opaque,pixels,sizeof pixels)==VK_SUCCESS);
        REQUIRE(ch_vk_target_read_depth_stencil(t,depth,stencil,256)==VK_SUCCESS && !ch_vk_target_pending(t));
        for(unsigned i=0;i<256;i++) REQUIRE(depth[i]==0 && stencil[i]==(unsigned char)i);
        ch_vk_target_destroy(t);t=NULL;
    }
    /* Cache entries stay pinned while descriptors are pending. Slot pressure
     * falls back to owned immutable uploads, which must also survive the pass. */
    REQUIRE(ch_vk_target_create(gpu,device,queue,family,16,16,&t)==VK_SUCCESS);
    REQUIRE(ch_vk_target_batching(t,1)==VK_SUCCESS);
    REQUIRE(ch_vk_target_clear_read(t,black,pixels,sizeof pixels)==VK_SUCCESS);
    ch_vk_target_defer_color_readback(t,1);
    ChVkTextureCacheConfig config={.physical=gpu,.device=device,.queue=queue,.family=family,
        .guest_base=memory,.guest_bytes=sizeof memory,.decode=decode,.max_entries=2,.max_bytes=1024};
    REQUIRE(ch_vk_texture_cache_create(&config,&cache)==VK_SUCCESS);
    Nv2aTexture tx={.offset=16,.width=1,.height=1,.pitch=4,.color=0x12,.filter=0x01010000,.addr_u=3,.addr_v=3};
    Nv2aSurface surface={.width=16,.height=16,.pitch=64,.bytes_per_pixel=4};
    Nv2aVertex vertices[6]={{0,0,0,1,0xffffffff,0,0},{16,0,0,1,0xffffffff,0,0},{16,16,0,1,0xffffffff,0,0},
        {0,0,0,1,0xffffffff,0,0},{16,16,0,1,0xffffffff,0,0},{0,16,0,1,0xffffffff,0,0}};
    Nv2aBatch batch={.vertices=vertices,.count=6,.state=&add,.texture=&tx};decodes=0;
    for(unsigned i=0;i<514;i++) {
        const uint32_t colors[]={0x01010000,0x01000100,0x01000001};memcpy(memory+16,&colors[i%3],4);
        REQUIRE(ch_vk_nv2a_draw_resolved_read(t,&surface,&batch,decode,ch_vk_texture_cache_resolve,cache,pixels,sizeof pixels)==VK_SUCCESS);
        if(i==1) {
            ChVkSampleImage probe={0};Nv2aTexture other=tx;other.offset=32;memcpy(memory+32,&colors[2],4);
            REQUIRE(ch_vk_texture_cache_resolve(cache,&other,&probe)==VK_NOT_READY && !probe.gpu);
        }
    }
    REQUIRE(ch_vk_target_read_color(t,pixels,sizeof pixels)==VK_SUCCESS && !ch_vk_target_pending(t));
    const unsigned char expected[]={172,171,171,255};
    for(unsigned i=0;i<sizeof pixels;i++) REQUIRE(pixels[i]==expected[i%4]);
    ChVkTextureCacheStats stats;ch_vk_texture_cache_stats(cache,&stats);REQUIRE(decodes>stats.uploads);
    ch_vk_target_destroy(t);t=NULL;ch_vk_texture_cache_destroy(cache);cache=NULL;
    puts("PASS: batched texture descriptors/immutable same-VA edits, pinned-budget fallback uploads, query/clear/CPU color/depth boundaries and LRU retirement");failed=0;
done:
    ch_vk_target_destroy(t);ch_vk_texture_cache_destroy(cache);
    if(saved_profile) {setenv("CHIHIRO_VULKAN_PROFILE",saved_profile,1);free(saved_profile);} else unsetenv("CHIHIRO_VULKAN_PROFILE");
    return failed;
#undef REQUIRE
}
