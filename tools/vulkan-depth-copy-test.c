#include "vk_target.h"
#include <stdio.h>
static int depth_copy_mode(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family,int busy) {
    ChVkTarget *a=NULL,*b=NULL,*wrong=NULL;int failed=1;
    unsigned char rgba[64],stencil[16];float depth[16];
    const float blue[]={0,0,1,1},green[]={0,1,0,1};
#define COPY_REQUIRE(x) do {if(!(x)) {fprintf(stderr,"depth copy failure line%d: %s\n",__LINE__,#x);goto done;}}while(0)
    COPY_REQUIRE(ch_vk_target_create_depth(gpu,device,queue,family,4,4,VK_FORMAT_D32_SFLOAT_S8_UINT,&a)==VK_SUCCESS);
    COPY_REQUIRE(ch_vk_target_create_depth(gpu,device,queue,family,4,4,VK_FORMAT_D32_SFLOAT_S8_UINT,&b)==VK_SUCCESS);
    COPY_REQUIRE(ch_vk_target_create(gpu,device,queue,family,3,4,&wrong)==VK_SUCCESS);
    COPY_REQUIRE(ch_vk_target_clear_read(wrong,green,rgba,sizeof rgba)==VK_SUCCESS);
    COPY_REQUIRE(ch_vk_target_batching(a,1)==VK_SUCCESS && ch_vk_target_batching(b,1)==VK_SUCCESS);
    COPY_REQUIRE(ch_vk_target_enable_queries(b)==VK_SUCCESS);
    COPY_REQUIRE(ch_vk_target_clear_read(a,blue,rgba,sizeof rgba)==VK_SUCCESS);
    COPY_REQUIRE(ch_vk_target_clear_read(b,green,rgba,sizeof rgba)==VK_SUCCESS);
    for(unsigned i=0;i<16;i++) {depth[i]=.6f+i/64.f;stencil[i]=(unsigned char)(i+1);}
    COPY_REQUIRE(ch_vk_target_write_depth_stencil(a,depth,stencil,16)==VK_SUCCESS);
    ch_vk_target_defer_color_readback(a,1);ch_vk_target_defer_color_readback(b,1);
    ChVkColorVertex v[]={{{-1,-1,.25f,1},{1,0,0,1}},{{1,-1,.25f,1},{1,0,0,1}},
        {{1,1,.25f,1},{1,0,0,1}},{{-1,1,.25f,1},{1,0,0,1}}};
    const uint16_t indices[]={0,1,2,0,2,3};
    Nv2aRenderState color={.color_mask=0x01010101};
    COPY_REQUIRE(ch_vk_target_draw_state_read(b,NULL,v,4,indices,6,&color,rgba,sizeof rgba)==VK_SUCCESS);
    v[1].xy[0]=v[2].xy[0]=0;
    Nv2aRenderState ds={.depth_test_enable=1,.depth_write=1,.depth_func=0x207,
        .stencil_enable=1,.stencil_func=0x207,.stencil_ref=0x7b,.stencil_func_mask=255,.stencil_write_mask=255,
        .stencil_fail=0x1e00,.stencil_zfail=0x1e00,.stencil_zpass=0x1e01};
    COPY_REQUIRE(ch_vk_target_draw_state_read(a,NULL,v,4,indices,6,&ds,rgba,sizeof rgba)==VK_SUCCESS);
    COPY_REQUIRE(ch_vk_target_pending(a) && ch_vk_target_pending(b));
    uint32_t unsupported[16]={0};
    COPY_REQUIRE(ch_vk_target_write_z24s8(a,unsupported,15)==VK_ERROR_INITIALIZATION_FAILED && ch_vk_target_pending(a));
    COPY_REQUIRE(ch_vk_target_copy_depth_stencil(a,wrong)==VK_ERROR_INITIALIZATION_FAILED && ch_vk_target_pending(a));
    if(!busy) COPY_REQUIRE(ch_vk_target_finish(b)==VK_SUCCESS);
    COPY_REQUIRE(ch_vk_target_copy_depth_stencil(a,b)==VK_SUCCESS);
    ChVkTargetResourceStats stats;ch_vk_target_resource_stats(a,&stats);
    COPY_REQUIRE(stats.combined_depth_copies==(busy?0:1));
    COPY_REQUIRE(!ch_vk_target_pending(a) && !ch_vk_target_pending(b));
    COPY_REQUIRE(vkGetFenceStatus(device,ch_vk_target_completion_fence(a))==VK_SUCCESS &&
        vkGetFenceStatus(device,ch_vk_target_completion_fence(b))==VK_SUCCESS);
    COPY_REQUIRE(ch_vk_target_read_depth_stencil(b,depth,stencil,16)==VK_SUCCESS);
    for(unsigned i=0;i<16;i++) {
        COPY_REQUIRE(depth[i]==(i%4<2?.25f:.6f+i/64.f));
        COPY_REQUIRE(stencil[i]==(i%4<2?0x7b:i+1));
    }
    COPY_REQUIRE(ch_vk_target_read_color(a,rgba,sizeof rgba)==VK_SUCCESS);
    for(unsigned i=0;i<64;i++) COPY_REQUIRE(rgba[i]==(i%4>=2?255:0));
    COPY_REQUIRE(ch_vk_target_read_color(b,rgba,sizeof rgba)==VK_SUCCESS);
    for(unsigned i=0;i<64;i++) COPY_REQUIRE(rgba[i]==((i%4==0 || i%4==3)?255:0));
    v[1].xy[0]=v[2].xy[0]=1;
    ds.color_mask=0x01010101;ds.depth_func=ds.stencil_func=0x202;ds.depth_write=ds.stencil_write_mask=0;
    uint32_t samples=0;
    COPY_REQUIRE(ch_vk_target_draw_count_read(b,NULL,v,4,indices,6,&ds,NULL,&samples,rgba,sizeof rgba)==VK_SUCCESS && samples==8);
    COPY_REQUIRE(ch_vk_target_copy_depth_stencil(b,b)==VK_SUCCESS);
    puts("PASS: exact GPU depth/stencil copy completes pending source/destination, preserves both colors, restores depth/stencil tests and rejects extent mismatch");failed=0;
done:
    ch_vk_target_destroy(wrong);ch_vk_target_destroy(a);ch_vk_target_destroy(b);return failed;
#undef COPY_REQUIRE
}
int ch_test_depth_copy(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    return depth_copy_mode(gpu,device,queue,family,0) || depth_copy_mode(gpu,device,queue,family,1);
}
