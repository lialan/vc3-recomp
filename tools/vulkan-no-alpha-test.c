#include "vk_target.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int ch_test_no_alpha(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    const char *old=getenv("CHIHIRO_VULKAN_NOAT");char *saved=old?strdup(old):NULL;
    if(old && !saved) return 1;
    ChVkTarget *target=NULL;int failed=1;
    unsigned char reference[4][64],rgba[64],stencil[16],old_stencil[4][16];
    float depth[16],old_depth[4][16];
    const float black[]={0,0,0,0};
    const uint16_t indices[]={0,1,2,0,2,3};
    ChVkColorVertex v[]={{{-1,-1,0,1},{0}},{{1,-1,0,1},{0}},{{1,1,0,1},{0}},{{-1,1,0,1},{0}}};
    Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=1,.depth_write=1,.depth_func=0x201,
        .stencil_enable=1,.stencil_func=0x207,.stencil_ref=3,.stencil_func_mask=255,.stencil_write_mask=255,
        .stencil_fail=0x1e00,.stencil_zfail=0x1e00,.stencil_zpass=0x1e01,.alpha_func=0x204,.alpha_ref=128};
#define REQUIRE(x) do {if(!(x)) {fprintf(stderr,"alpha-free failure line %d: %s\n",__LINE__,#x);goto done;}}while(0)
    for(unsigned split=0;split<2;split++) {
        setenv("CHIHIRO_VULKAN_NOAT",split?"1":"0",1);
        REQUIRE(ch_vk_target_create(gpu,device,queue,family,4,4,&target)==VK_SUCCESS);
        REQUIRE(ch_vk_target_enable_queries(target)==VK_SUCCESS);
        REQUIRE(ch_vk_target_clear_read(target,black,rgba,sizeof rgba)==VK_SUCCESS);
        for(unsigned step=0;step<4;step++) {
            const float z[]={.75f,.5f,.5f,.25f},alpha[]={.25f,.25f,.75f,0};
            for(unsigned i=0;i<4;i++) {
                v[i].xy[2]=z[step];memset(v[i].rgba,0,sizeof v[i].rgba);
                v[i].rgba[step==0?0:step==2?2:1]=1;v[i].rgba[3]=alpha[step];
            }
            state.alpha_test_enable=step==1 || step==2;
            uint32_t samples=0;
            REQUIRE(ch_vk_target_draw_count_read(target,NULL,v,4,indices,6,&state,NULL,&samples,rgba,sizeof rgba)==VK_SUCCESS);
            REQUIRE(samples==(step==1?0u:16u));
            REQUIRE(ch_vk_target_read_depth_stencil(target,depth,stencil,16)==VK_SUCCESS);
            for(unsigned i=0;i<16;i++) REQUIRE(depth[i]==(step<2?.75f:step==2?.5f:.25f) && stencil[i]==3);
            if(!split) {memcpy(reference[step],rgba,64);memcpy(old_depth[step],depth,sizeof depth);memcpy(old_stencil[step],stencil,16);}
            else REQUIRE(!memcmp(reference[step],rgba,64) && !memcmp(old_depth[step],depth,sizeof depth) && !memcmp(old_stencil[step],stencil,16));
        }
        for(unsigned i=0;i<16;i++) REQUIRE(!rgba[i*4] && rgba[i*4+1]==255 && !rgba[i*4+2] && !rgba[i*4+3]);
        ChVkTargetResourceStats stats;ch_vk_target_resource_stats(target,&stats);
        REQUIRE(stats.pipeline_builds==(split?2u:1u) && stats.no_alpha_pipeline_builds==split && stats.no_alpha_pipeline_hits==(split?1u:0u));
        ch_vk_target_destroy(target);target=NULL;
    }
    puts("PASS: alpha-free vs generic shaders preserve exact color/depth/stencil and visibility across disabled/reject/pass/disabled transitions; distinct cached alpha pipeline and no-alpha reuse");
    failed=0;
done:
    ch_vk_target_destroy(target);
    if(saved) {setenv("CHIHIRO_VULKAN_NOAT",saved,1);free(saved);} else unsetenv("CHIHIRO_VULKAN_NOAT");
    return failed;
#undef REQUIRE
}
