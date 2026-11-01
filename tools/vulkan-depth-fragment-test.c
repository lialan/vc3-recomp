/* Fragment rounding vs native D24; constant-depth CPU oracle when unavailable. */
#include "vk_target.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
static float rounded(float z) {return (float)((double)(uint32_t)((double)z*16777215.0+0.5)/16777215.0);}
int ch_test_depth_fragment(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    enum {W=16,H=16,N=W*H};ChVkTarget *t[2]={0};ChVkTexture *texture=NULL;int failed=1,native=1;
    unsigned char rgba[2][N*4],st[2][N],initial_st[N];float depth[2][N],initial_z[N];
    const float black[]={0,0,0,1};const uint16_t indices[]={0,1,2};
    const float values[]={0,0x1p-149f,0x1p-25f,0x1.000002p-25f,.1f,0x1.fffffep-2f,.5f,0x1.000002p-1f,0x1.fffffep-1f,1};
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"fragment depth failed line%d: %s\n",__LINE__,#x);goto done;}}while(0)
    VkResult r=ch_vk_target_create_depth(gpu,device,queue,family,W,H,VK_FORMAT_D24_UNORM_S8_UINT,&t[0]);
    if(r==VK_ERROR_FORMAT_NOT_SUPPORTED){native=0;r=ch_vk_target_create(gpu,device,queue,family,W,H,&t[0]);}
    REQUIRE(r==VK_SUCCESS);
    REQUIRE(ch_vk_target_create(gpu,device,queue,family,W,H,&t[1])==VK_SUCCESS);
    for(unsigned p=0;p<2;p++) {
        REQUIRE(ch_vk_target_clear_read(t[p],black,rgba[p],sizeof rgba[p])==VK_SUCCESS);
        REQUIRE(ch_vk_target_enable_queries(t[p])==VK_SUCCESS);
        REQUIRE(ch_vk_target_batching(t[p],1)==VK_SUCCESS);
    }
    REQUIRE(ch_vk_target_depth_quantization(t[1],5)==VK_SUCCESS);
    uint32_t texel=0x80ff33aau;
    REQUIRE(ch_vk_target_upload_texture(t[0],1,1,&texel,&texture)==VK_SUCCESS);
    const ChVkTexture *stages[]={texture,NULL,NULL,NULL};
    Nv2aCombiner rc={.control=1,.stage_program=1,.final0=0xc,.final1=0x1c00};
    rc.color_icw[0]=0x08040000;rc.alpha_icw[0]=0x18140000;
    rc.color_ocw[0]=rc.alpha_ocw[0]=0xc00;
    for(unsigned test=0;test<(native?384u:256u);test++) {
        int gradient=test>=256;float z=values[(test/8)%10];
        for(unsigned i=0;i<N;i++){initial_z[i]=rounded(values[(i+test/8)%10]);initial_st[i]=(i*17+test)&255;}
        for(unsigned p=0;p<2;p++) {
            REQUIRE(ch_vk_target_clear_read(t[p],black,rgba[p],sizeof rgba[p])==VK_SUCCESS);
            REQUIRE(ch_vk_target_write_depth_stencil(t[p],initial_z,initial_st,N)==VK_SUCCESS);
        }
        Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=1,.depth_write=1,.depth_func=0x200+(test%8),
            .stencil_enable=1,.stencil_func=test%3?0x207:0x201,.stencil_ref=0x71,.stencil_func_mask=255,.stencil_write_mask=255,
            .stencil_fail=0x1e02,.stencil_zfail=0x1e01,.stencil_zpass=0x150a};
        state.depth_write=gradient || !((test/80)&1);
        state.alpha_test_enable=(test&2)!=0;state.alpha_func=(test&4)?0x207:0x200;
        uint32_t samples[2]={0};
        for(unsigned p=0;p<2;p++) {
            ChVkColorVertex v[]={{{-1,-1,z,1},{.2f,.4f,.6f,1}},{{3,-1,z,1},{.2f,.4f,.6f,1}},{{-1,3,z,1},{.2f,.4f,.6f,1}}};
            if(gradient){v[0].xy[2]=values[(test/8)%10];v[1].xy[2]=values[((test/8)*3+2)%10];v[2].xy[2]=values[((test/8)*7+4)%10];}
            if(!native && !p) for(unsigned k=0;k<3;k++)v[k].xy[2]=(float)((double)(uint32_t)nearbyintf(v[k].xy[2]*16777215.0f)/16777215.0);
            for(unsigned k=0;k<3;k++){v[k].xy[4]=v[k].xy[5]=.5f;v[k].xy[11]=v[k].xy[12]=.5f;v[k].xy[14]=1;}
            REQUIRE(ch_vk_target_draw_combiner_read(t[p],NULL,v,3,indices,3,&state,test%3==1?texture:NULL,&samples[p],test%3==2?&rc:NULL,0,stages,rgba[p],sizeof rgba[p])==VK_SUCCESS);
            REQUIRE(ch_vk_target_read_depth_stencil(t[p],depth[p],st[p],N)==VK_SUCCESS);
        }
        if(samples[0]!=samples[1] || memcmp(rgba[0],rgba[1],sizeof rgba[0]) || memcmp(st[0],st[1],N) || memcmp(depth[0],depth[1],sizeof depth[0])) {
            for(unsigned i=0;i<N;i++)if(depth[0][i]!=depth[1][i] || st[0][i]!=st[1][i] || memcmp(&rgba[0][i*4],&rgba[1][i*4],4)) {
                fprintf(stderr,"case%u pixel%u gradient=%d func=%x z=%a native=%d depths=%a/%a stencil=%u/%u samples=%u/%u\n",test,i,gradient,state.depth_func,z,native,depth[0][i],depth[1][i],st[0][i],st[1][i],samples[0],samples[1]);break;
            }
            goto done;
        }
    }
    /* Deferred draws exercise the production batching path without queries. */
    ChVkTargetResourceStats before[2],after[2];
    for(unsigned p=0;p<2;p++) {
        REQUIRE(ch_vk_target_clear_read(t[p],black,rgba[p],sizeof rgba[p])==VK_SUCCESS);
        ch_vk_target_defer_color_readback(t[p],1);
        ch_vk_target_resource_stats(t[p],&before[p]);
    }
    for(unsigned draw=0;draw<64;draw++) {
        float z=values[draw%10];
        Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=1,.depth_write=1,.depth_func=0x203,
            .stencil_enable=1,.stencil_func=0x207,.stencil_ref=0x71,.stencil_func_mask=255,.stencil_write_mask=255,
            .stencil_fail=0x1e02,.stencil_zfail=0x1e01,.stencil_zpass=0x150a};
        for(unsigned p=0;p<2;p++) {
            float incoming=(!native && !p)?(float)((double)(uint32_t)nearbyintf(z*16777215.0f)/16777215.0):z;
            ChVkColorVertex v[]={{{-1,-1,incoming,1},{1,0,0,1}},{{3,-1,incoming,1},{1,0,0,1}},{{-1,3,incoming,1},{1,0,0,1}}};
            REQUIRE(ch_vk_target_draw_state_read(t[p],NULL,v,3,indices,3,&state,rgba[p],sizeof rgba[p])==VK_SUCCESS);
        }
    }
    for(unsigned p=0;p<2;p++) {
        REQUIRE(ch_vk_target_read_depth_stencil(t[p],depth[p],st[p],N)==VK_SUCCESS);
        REQUIRE(ch_vk_target_read_color(t[p],rgba[p],sizeof rgba[p])==VK_SUCCESS);
        ch_vk_target_resource_stats(t[p],&after[p]);
        REQUIRE(after[p].batch_draws-before[p].batch_draws==64 && after[p].batch_submissions-before[p].batch_submissions==1);
        for(unsigned op=0;op<8;op++) REQUIRE(after[p].depth_roundtrips_by_compare[op]==0);
    }
    REQUIRE(!memcmp(rgba[0],rgba[1],sizeof rgba[0]) && !memcmp(depth[0],depth[1],sizeof depth[0]) && !memcmp(st[0],st[1],N));
    printf("PASS: fragment depth exact color/depth/stencil/query comparisons against %s\n",native?"native D24 (384 constant/gradient cases)":"CPU-rounded constant-depth oracle (256 cases; native D24 unavailable)");failed=0;
done:for(unsigned p=0;p<2;p++)ch_vk_target_destroy(t[p]);ch_vk_texture_destroy(texture);return failed;
#undef REQUIRE
}
