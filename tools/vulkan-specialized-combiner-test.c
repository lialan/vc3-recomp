/* Same-device fragment specialization vs the interpreter and CPU arithmetic.
 * Includes 0..8 stages, mappings/mux/dot/output flags, all fetch modes, cube and
 * missing samplers, dynamic factors/fog, alpha visibility and inactive words. */
#include "vk_target.h"
#include "nv2a_combiner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static uint32_t random_word(uint32_t *seed) {return *seed=*seed*1664525u+1013904223u;}
int ch_test_specialized_combiners(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    const char *old=getenv("CHIHIRO_VULKAN_SPECIALIZE_RC"),*old_alpha=getenv("CHIHIRO_VULKAN_NOAT");
    char *saved=old?strdup(old):NULL,*saved_alpha=old_alpha?strdup(old_alpha):NULL;
    if((old && !saved) || (old_alpha && !saved_alpha)) {free(saved);free(saved_alpha);return 1;}
    VkPipelineCache cache=VK_NULL_HANDLE;
    ChVkTarget *t[2]={0};ChVkTexture *textures[4]={0};int failed=1;
    unsigned char rgba[2][64],stencil[2][16];float depth[2][16];
    uint32_t pixels[]={0x80cc6633,0xc033cc66,0x604499dd,0xffcc8833},seed=1729;
    const uint16_t indices[]={0,1,2};const float black[]={0,0,0,0};
    ChVkColorVertex v[]={{{-1,-1,.25f,1},{.2f,.4f,.6f,.8f}},{{3,-1,.25f,1},{.2f,.4f,.6f,.8f}},{{-1,3,.25f,1},{.2f,.4f,.6f,.8f}}};
    Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=1,.depth_write=1,.depth_func=0x207,
        .stencil_enable=1,.stencil_func=0x207,.stencil_ref=7,.stencil_func_mask=255,.stencil_write_mask=255,
        .stencil_fail=0x1e00,.stencil_zfail=0x1e00,.stencil_zpass=0x1e01};
#define REQUIRE(x) do {if(!(x)) {fprintf(stderr,"specialized combiner failure line%d: %s\n",__LINE__,#x);goto done;}}while(0)
    VkPipelineCacheCreateInfo cache_info={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    REQUIRE(vkCreatePipelineCache(device,&cache_info,NULL,&cache)==VK_SUCCESS);
    setenv("CHIHIRO_VULKAN_NOAT","0",1);
    for(unsigned mode=0;mode<2;mode++) {
        setenv("CHIHIRO_VULKAN_SPECIALIZE_RC",mode?"1":"0",1);
        REQUIRE(ch_vk_target_create(gpu,device,queue,family,4,4,&t[mode])==VK_SUCCESS);
        REQUIRE(ch_vk_target_pipeline_cache(t[mode],cache)==VK_SUCCESS);
        REQUIRE(ch_vk_target_pipeline_cache(t[mode],cache)==VK_ERROR_INITIALIZATION_FAILED);
        REQUIRE(ch_vk_target_enable_queries(t[mode])==VK_SUCCESS);
    }
    for(unsigned i=0;i<4;i++) {
        if(i==2) {uint32_t faces[6];for(unsigned f=0;f<6;f++) faces[f]=pixels[i];
            REQUIRE(ch_vk_target_upload_cube(t[0],1,faces,&textures[i])==VK_SUCCESS);}
        else REQUIRE(ch_vk_target_upload_texture(t[0],1,1,&pixels[i],&textures[i])==VK_SUCCESS);
    }
    for(unsigned test=0;test<32;test++) {
        Nv2aCombiner rc={0};unsigned n=test%9;
        for(unsigned i=0;i<8;i++) {
            rc.color_icw[i]=random_word(&seed);rc.alpha_icw[i]=random_word(&seed);
            rc.color_ocw[i]=random_word(&seed)&0xfffff;rc.alpha_ocw[i]=random_word(&seed)&0x3ffff;
        }
        rc.control=n|((test&1)?0x11100:0);
        rc.final0=test%3?random_word(&seed):0;rc.final1=test%3?random_word(&seed):0;
        const ChVkTexture *stages[4];float texels[4][4];
        for(unsigned st=0;st<4;st++) {
            static const unsigned modes[]={0,1,2,3,4,7};unsigned mode=modes[(test+st)%6];
            rc.stage_program|=mode<<(st*5);stages[st]=(test+st)%7?textures[st]:NULL;
            for(unsigned k=0;k<3;k++) {v[k].xy[11+st*4]=.25f;v[k].xy[12+st*4]=.75f;v[k].xy[13+st*4]=.5f;v[k].xy[14+st*4]=1;}
            for(unsigned c=0;c<4;c++) {
                static const unsigned shifts[]={16,8,0,24};
                texels[st][c]=mode==0?(c==3):mode==4?v[0].xy[11+st*4+c]:
                    mode<=3?(stages[st]?((pixels[st]>>shifts[c])&255)/255.f:1):0;
            }
        }
        for(unsigned phase=0;phase<2;phase++) {
            for(unsigned i=0;i<8;i++) {rc.factor0[i]=random_word(&seed);rc.factor1[i]=random_word(&seed);
                if(phase && i>=n) {rc.color_icw[i]^=0xa1234567;rc.alpha_ocw[i]^=0x798abcde;}}
            rc.final_c0=random_word(&seed);rc.final_c1=random_word(&seed);uint32_t fog_color=random_word(&seed);
            for(unsigned k=0;k<3;k++) {v[k].xy[6]=.7f;v[k].xy[7]=.3f;v[k].xy[8]=.1f;v[k].xy[9]=.9f;v[k].xy[10]=phase?.25f:.75f;}
            state.alpha_test_enable=phase;state.alpha_func=0x200+test%8;state.alpha_ref=(test*19)&255;
            uint32_t samples[2]={0};ChVkTargetResourceStats before,after;ch_vk_target_resource_stats(t[1],&before);
            for(unsigned mode=0;mode<2;mode++) {
                REQUIRE(ch_vk_target_draw_combiner_read(t[mode],black,v,3,indices,3,&state,NULL,&samples[mode],&rc,fog_color,stages,rgba[mode],64)==VK_SUCCESS);
                REQUIRE(ch_vk_target_read_depth_stencil(t[mode],depth[mode],stencil[mode],16)==VK_SUCCESS);
            }
            REQUIRE(samples[0]==samples[1] && !memcmp(depth[0],depth[1],sizeof depth[0]) && !memcmp(stencil[0],stencil[1],16));
            for(unsigned i=0;i<64;i++) REQUIRE(abs((int)rgba[0][i]-rgba[1][i])<=1);
            ch_vk_target_resource_stats(t[1],&after);
            REQUIRE(after.specialized_pipeline_builds>=before.specialized_pipeline_builds &&
                after.specialized_pipeline_builds<=before.specialized_pipeline_builds+(phase?0:1));
            if(phase) REQUIRE(after.specialized_pipeline_hits==before.specialized_pipeline_hits+1);
            else {
                float expected[4],spec[]={.7f,.3f,.1f,.9f},fog[]={((fog_color>>16)&255)/255.f,((fog_color>>8)&255)/255.f,(fog_color&255)/255.f,.75f};
                nv2a_rc_eval(&rc,v[0].rgba,spec,fog,(const float (*)[4])texels,expected);
                for(unsigned i=0;i<64;i++) REQUIRE(isfinite(expected[i%4]) && abs((int)rgba[1][i]-(int)(expected[i%4]*255+.5f))<=1);
            }
        }
    }
    ChVkTargetResourceStats stats;ch_vk_target_resource_stats(t[1],&stats);
    REQUIRE(stats.specialized_pipeline_builds>=24 && stats.specialized_pipeline_hits>=32);
    /* Vary q before interpolation: static instruction selection must retain
     * fragment-stage projective division. Exercise the NOAT module as well. */
    setenv("CHIHIRO_VULKAN_NOAT","1",1);
    for(unsigned mode=0;mode<2;mode++) {
        ch_vk_target_destroy(t[mode]);t[mode]=NULL;
        setenv("CHIHIRO_VULKAN_SPECIALIZE_RC",mode?"1":"0",1);
        REQUIRE(ch_vk_target_create(gpu,device,queue,family,4,4,&t[mode])==VK_SUCCESS);
        REQUIRE(ch_vk_target_pipeline_cache(t[mode],cache)==VK_SUCCESS);
        REQUIRE(ch_vk_target_pipeline_cache(t[mode],cache)==VK_ERROR_INITIALIZATION_FAILED);
        if(mode) REQUIRE(ch_vk_target_batching(t[mode],1)==VK_SUCCESS);
    }
    ch_vk_texture_destroy(textures[3]);textures[3]=NULL;
    REQUIRE(ch_vk_target_upload_texture(t[0],2,2,pixels,&textures[3])==VK_SUCCESS);
    const ChVkTexture *projected[]={textures[3],NULL,NULL,NULL};
    Nv2aCombiner project={.control=1,.stage_program=1,.final0=0xc,.final1=0x1c00};
    project.color_icw[0]=0x08040000;project.alpha_icw[0]=0x18140000;
    project.color_ocw[0]=project.alpha_ocw[0]=0xc00;
    const float coords[][4]={{0,0,0,1},{8,0,0,2},{0,16,0,4}};
    for(unsigned i=0;i<3;i++) memcpy(v[i].xy+11,coords[i],sizeof coords[i]);
    state.alpha_test_enable=0;
    for(unsigned mode=0;mode<2;mode++)
        REQUIRE(ch_vk_target_draw_combiner_read(t[mode],black,v,3,indices,3,&state,NULL,NULL,&project,0,projected,rgba[mode],64)==VK_SUCCESS);
    for(unsigned i=0;i<64;i++) REQUIRE(abs((int)rgba[0][i]-rgba[1][i])<=1);
    ch_vk_target_resource_stats(t[1],&stats);REQUIRE(stats.no_alpha_pipeline_builds==1 && stats.specialized_pipeline_builds==1);
    /* Distinct active instruction keys with discarded stage outputs stress the
     * bounded program LRU. Dynamic factors are copied for each pending draw. */
    REQUIRE(ch_vk_target_clear_read(t[1],black,rgba[1],64)==VK_SUCCESS);
    ch_vk_target_defer_color_readback(t[1],1);
    Nv2aRenderState add={.color_mask=0x01010101,.blend_enable=1,.blend_src=1,.blend_dst=1,.blend_eq=0x8006};
    project.stage_program=0;project.color_icw[0]=0x01200000;project.alpha_icw[0]=0x11200000;
    ch_vk_target_resource_stats(t[1],&stats);
    for(unsigned i=0;i<140;i++) {
        static const uint32_t factors[]={0x01010000,0x01000100,0x01000001};
        project.control=2;project.color_icw[1]=i+1;
        project.factor0[0]=factors[i%3];
        REQUIRE(ch_vk_target_draw_combiner_read(t[1],NULL,v,3,indices,3,&add,NULL,NULL,&project,0,NULL,rgba[1],64)==VK_SUCCESS);
    }
    REQUIRE(ch_vk_target_read_color(t[1],rgba[1],64)==VK_SUCCESS);
    ChVkTargetResourceStats after;ch_vk_target_resource_stats(t[1],&after);
    REQUIRE(after.specialized_pipeline_builds-stats.specialized_pipeline_builds==140 && after.pipeline_evictions>0 &&
        after.batch_draws-stats.batch_draws==140 && after.batch_submissions-stats.batch_submissions==2);
    const unsigned char summed[]={47,47,46,140};
    for(unsigned i=0;i<64;i++) REQUIRE(rgba[1][i]==summed[i%4]);
    /* Reserved control flags cannot create shader variants. */
    ch_vk_target_resource_stats(t[1],&stats);
    for(unsigned i=1;i<=8;i++) {
        project.control=2|(i<<20);
        REQUIRE(ch_vk_target_draw_combiner_read(t[1],NULL,v,3,indices,3,&add,NULL,NULL,&project,0,NULL,rgba[1],64)==VK_SUCCESS);
    }
    REQUIRE(ch_vk_target_finish(t[1])==VK_SUCCESS);
    ch_vk_target_resource_stats(t[1],&after);
    REQUIRE(after.pipeline_builds==stats.pipeline_builds && after.pipeline_hits-stats.pipeline_hits==8);
    /* Blend color changes must retain one pipeline and per-draw values even
     * while multiple draws share a command buffer. */
    REQUIRE(ch_vk_target_clear_read(t[1],black,rgba[1],64)==VK_SUCCESS);
    project.control=1;project.factor0[0]=0xffffffff;
    add.blend_src=0x8001; /* CONSTANT_COLOR */
    ch_vk_target_resource_stats(t[1],&stats);
    for(unsigned i=0;i<3;i++) {
        const uint32_t colors[]={0x01010000,0x01000100,0x01000001};
        add.blend_color=colors[i];
        REQUIRE(ch_vk_target_draw_combiner_read(t[1],NULL,v,3,indices,3,&add,NULL,NULL,&project,0,NULL,rgba[1],64)==VK_SUCCESS);
    }
    REQUIRE(ch_vk_target_read_color(t[1],rgba[1],64)==VK_SUCCESS);
    ch_vk_target_resource_stats(t[1],&after);
    REQUIRE(after.pipeline_builds-stats.pipeline_builds==1 && after.pipeline_hits-stats.pipeline_hits==2);
    const unsigned char blended[]={1,1,1,3};
    for(unsigned i=0;i<64;i++) REQUIRE(rgba[1][i]==blended[i%4]);
    /* Pending depth/stencil writes and readback share one real submission. */
    Nv2aRenderState zs={.color_mask=0x01010101,.depth_test_enable=1,.depth_write=1,.depth_func=0x207,
        .stencil_enable=1,.stencil_func=0x207,.stencil_ref=0xab,.stencil_func_mask=255,.stencil_write_mask=255,
        .stencil_fail=0x1e00,.stencil_zfail=0x1e00,.stencil_zpass=0x1e01};
    ch_vk_target_resource_stats(t[1],&stats);
    for(unsigned mode=0;mode<2;mode++) {
        REQUIRE(ch_vk_target_draw_combiner_read(t[mode],NULL,v,3,indices,3,&zs,NULL,NULL,&project,0,NULL,rgba[mode],64)==VK_SUCCESS);
        REQUIRE(ch_vk_target_read_depth_stencil(t[mode],depth[mode],stencil[mode],16)==VK_SUCCESS);
    }
    REQUIRE(!memcmp(depth[0],depth[1],sizeof depth[0]) && !memcmp(stencil[0],stencil[1],16));
    for(unsigned i=0;i<16;i++) REQUIRE(stencil[1][i]==0xab && fabsf(depth[1][i]-.25f)<1e-6f);
    ch_vk_target_resource_stats(t[1],&after);
    REQUIRE(after.combined_depth_readbacks-stats.combined_depth_readbacks==1 && after.batch_submissions-stats.batch_submissions==1);
    /* Exercise all three exact roundtrip implementations and their barriers. */
    for(unsigned round=0;round<128;round++) {
        for(unsigned i=0;i<16;i++) {
            uint32_t bits=random_word(&seed)%0x3f800001u;
            if(round<3) bits=i==0?0:i==1?0x3f800000u:0x3f000000u+i-8;
            memcpy(&depth[0][i],&bits,4);stencil[0][i]=(uint8_t)(round+i*17);
        }
        REQUIRE(ch_vk_target_write_depth_stencil(t[0],depth[0],stencil[0],16)==VK_SUCCESS);
        REQUIRE((round%3==2?ch_vk_target_quantize_depth_input(t[0]):round%3==1?ch_vk_target_quantize_depth_draw(t[0]):ch_vk_target_quantize_depth(t[0]))==VK_SUCCESS);
        REQUIRE(ch_vk_target_read_depth_stencil(t[0],depth[1],stencil[1],16)==VK_SUCCESS);
        for(unsigned i=0;i<16;i++) {
            uint32_t q=(uint32_t)((double)depth[0][i]*16777215.0+0.5);
            float expected=(float)((double)q/16777215.0);
            REQUIRE(depth[1][i]==expected && stencil[1][i]==stencil[0][i]);
        }
    }
    for(unsigned quant_mode=1;quant_mode<=3;quant_mode++) {
    REQUIRE(ch_vk_target_depth_quantization(t[1],quant_mode)==VK_SUCCESS);
    for(unsigned mode=0;mode<2;mode++) {
        for(unsigned i=0;i<16;i++){depth[mode][i]=.5f;stencil[mode][i]=0;}
        REQUIRE(ch_vk_target_write_depth_stencil(t[mode],depth[mode],stencil[mode],16)==VK_SUCCESS);
        REQUIRE(ch_vk_target_clear_read(t[mode],black,rgba[mode],64)==VK_SUCCESS);
    }
    for(unsigned draw=0;draw<32;draw++) {
        uint32_t bits=0x3f000000u+(draw%7)-3;
        for(unsigned k=0;k<3;k++)memcpy(&v[k].xy[2],&bits,4);
        zs.depth_func=0x200+(draw%8);zs.stencil_ref=draw;
        for(unsigned mode=0;mode<2;mode++) {
            REQUIRE(ch_vk_target_draw_combiner_read(t[mode],NULL,v,3,indices,3,&zs,NULL,NULL,&project,0,NULL,rgba[mode],64)==VK_SUCCESS);
            if(!mode) {
                REQUIRE(ch_vk_target_read_depth_stencil(t[0],depth[0],stencil[0],16)==VK_SUCCESS);
                for(unsigned i=0;i<16;i++){uint32_t q=(uint32_t)((double)depth[0][i]*16777215.0+0.5);depth[0][i]=(float)((double)q/16777215.0);}
                REQUIRE(ch_vk_target_write_depth_stencil(t[0],depth[0],stencil[0],16)==VK_SUCCESS);
            }
        }
    }
    REQUIRE(ch_vk_target_read_depth_stencil(t[1],depth[1],stencil[1],16)==VK_SUCCESS);
    REQUIRE(ch_vk_target_read_color(t[1],rgba[1],64)==VK_SUCCESS);
    REQUIRE(!memcmp(depth[0],depth[1],sizeof depth[0]) && !memcmp(stencil[0],stencil[1],16) && !memcmp(rgba[0],rgba[1],64));
    }
    puts("PASS: 32 specialized combiner cases match interpreter/CPU across 0..8 stages, dynamic factors/fog, fetch modes/cube/missing samplers and exact alpha/depth/stencil; inactive edits reuse pipelines, NOAT retains projective q, 140 pending factor updates survive bounded LRU eviction and two batches");failed=0;
done:
    for(unsigned i=0;i<2;i++) ch_vk_target_destroy(t[i]);
    for(unsigned i=0;i<4;i++) ch_vk_texture_destroy(textures[i]);
    if(cache) vkDestroyPipelineCache(device,cache,NULL);
    if(saved) {setenv("CHIHIRO_VULKAN_SPECIALIZE_RC",saved,1);free(saved);} else unsetenv("CHIHIRO_VULKAN_SPECIALIZE_RC");
    if(saved_alpha) {setenv("CHIHIRO_VULKAN_NOAT",saved_alpha,1);free(saved_alpha);} else unsetenv("CHIHIRO_VULKAN_NOAT");
    return failed;
#undef REQUIRE
}
