/* Exact D32 float + stencil <-> guest Z24/S8, including odd-sized buffers. */
#include "vk_target.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
int ch_test_depth_fragment(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    ChVkTarget *target=NULL;int failed=1;uint32_t *input=NULL,*output=NULL;
    float *depth=NULL;uint8_t *stencil=NULL,*rgba=NULL;
    const unsigned sizes[][2]={{1,1},{3,5},{17,9},{512,512}};uint32_t seed=12345;
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"depth pack line%d: %s\n",__LINE__,#x);goto done;}}while(0)
    for(unsigned test=0;test<4;test++) {
        unsigned w=sizes[test][0],h=sizes[test][1],n=w*h;
        input=malloc(n*4);output=malloc(n*4);depth=malloc(n*4);stencil=malloc(n);rgba=malloc(n*4);
        REQUIRE(input && output && depth && stencil && rgba);
        REQUIRE(ch_vk_target_create(gpu,device,queue,family,w,h,&target)==VK_SUCCESS);
        const float black[]={0,0,0,1};REQUIRE(ch_vk_target_clear_read(target,black,rgba,n*4)==VK_SUCCESS);
        REQUIRE(ch_vk_target_batching(target,1)==VK_SUCCESS);
        for(unsigned repeat=0;repeat<3;repeat++) {
            for(unsigned i=0;i<n;i++) {seed=seed*1664525u+1013904223u;input[i]=seed;}
            input[0]=repeat==0?0:repeat==1?0xffffffff:0x80000080;
            REQUIRE(ch_vk_target_write_z24s8(target,input,n)==VK_SUCCESS);
            REQUIRE(ch_vk_target_read_z24s8(target,output,n)==VK_SUCCESS);
            REQUIRE(!memcmp(input,output,n*4));
            REQUIRE(ch_vk_target_read_depth_stencil(target,depth,stencil,n)==VK_SUCCESS);
            for(unsigned i=0;i<n;i++) {
                float expected=(float)((double)(input[i]>>8)/16777215.0);
                REQUIRE(!memcmp(&expected,&depth[i],4) && stencil[i]==(uint8_t)input[i]);
                // Non-grid finite floats verify packing against double precision.
                uint32_t bits=0x3f000000u+(input[i]%0x800001u);memcpy(&depth[i],&bits,4);
                input[i]=((uint32_t)((double)depth[i]*16777215.0+.5)<<8)|stencil[i];
            }
            REQUIRE(ch_vk_target_write_depth_stencil(target,depth,stencil,n)==VK_SUCCESS);
            REQUIRE(ch_vk_target_read_depth_color(target,output,NULL,n,1,rgba,n*4)==VK_SUCCESS);
            REQUIRE(!memcmp(input,output,n*4));
            // Reuse the same staging for color before the next packed upload.
            REQUIRE(ch_vk_target_import_color(target,rgba,n*4)==VK_SUCCESS);
        }
        REQUIRE(ch_vk_target_depth_quantization(target,5)==VK_SUCCESS);
        ch_vk_target_defer_color_readback(target,1);
        Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=1,.depth_write=1,.depth_func=0x207,
            .stencil_enable=1,.stencil_func=0x207,.stencil_ref=0x71,.stencil_func_mask=255,.stencil_write_mask=255,
            .stencil_fail=0x1e01,.stencil_zfail=0x1e01,.stencil_zpass=0x1e01};
        ChVkColorVertex v[]={{{-1,-1,.25f,1},{1,0,0,1}},{{3,-1,.25f,1},{1,0,0,1}},{{-1,3,.25f,1},{1,0,0,1}}};
        const uint16_t idx[]={0,1,2};
        ChVkTargetResourceStats before,after;ch_vk_target_resource_stats(target,&before);
        REQUIRE(ch_vk_target_draw_state_read(target,NULL,v,3,idx,3,&state,rgba,n*4)==VK_SUCCESS);
        REQUIRE(ch_vk_target_read_depth_color(target,output,NULL,n,1,rgba,n*4)==VK_SUCCESS);
        ch_vk_target_resource_stats(target,&after);REQUIRE(after.batch_submissions==before.batch_submissions+1);
        for(unsigned i=0;i<n;i++) {REQUIRE(output[i]==0x40000071);REQUIRE(rgba[i*4]==255 && rgba[i*4+1]==0 && rgba[i*4+2]==0);}
        ch_vk_target_destroy(target);target=NULL;free(input);input=NULL;free(output);output=NULL;free(depth);depth=NULL;free(stencil);stencil=NULL;free(rgba);rgba=NULL;
    }
    puts("PASS: GPU D32/Z24S8 exact roundtrip and float packing, stencil, odd sizes, repeated staging reuse, combined pending depth/color readback");failed=0;
done:
    ch_vk_target_destroy(target);free(input);free(output);free(depth);free(stencil);free(rgba);return failed;
}
