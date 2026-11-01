#include "vk_target.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int packed_depth(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    enum {N=256*256};int failed=1;ChVkTarget *target=NULL;
    uint32_t *words=malloc(N*4),*got=malloc(N*4);float *depth=malloc(N*4);
    unsigned char *stencil=malloc(N),*rgba=malloc(N*4);
#define PACKED_REQUIRE(x) do {if(!(x)) {fprintf(stderr,"packed D24 failure line%d: %s\n",__LINE__,#x);goto done;}}while(0)
    PACKED_REQUIRE(words && got && depth && stencil && rgba);
    PACKED_REQUIRE(ch_vk_target_create_depth(gpu,device,queue,family,256,256,VK_FORMAT_D24_UNORM_S8_UINT,&target)==VK_SUCCESS);
    const float black[]={0,0,0,1};
    PACKED_REQUIRE(ch_vk_target_clear_read(target,black,rgba,N*4)==VK_SUCCESS);
    for(unsigned i=0;i<N;i++) {
        uint32_t z=i<48?((i&1)?~(1u<<(i/2)):(1u<<(i/2))):(i*0x9e3779b9u+0x123456u);
        words[i]=((z&0xffffffu)<<8)|(i&255);
    }
    words[48]=0;words[49]=0xffffffff;words[50]=0x7fffff42;words[51]=0x80000081;
    PACKED_REQUIRE(ch_vk_target_write_z24s8(target,words,N)==VK_SUCCESS);
    PACKED_REQUIRE(ch_vk_target_read_z24s8(target,got,N)==VK_SUCCESS && !memcmp(words,got,N*4));
    PACKED_REQUIRE(ch_vk_target_read_depth_stencil(target,depth,stencil,N)==VK_SUCCESS);
    for(unsigned i=0;i<N;i++) {
        PACKED_REQUIRE(depth[i]==(float)((double)(words[i]>>8)/16777215.0) && stencil[i]==(words[i]&255));
    }
    PACKED_REQUIRE(ch_vk_target_write_depth_stencil(target,depth,stencil,N)==VK_SUCCESS);
    PACKED_REQUIRE(ch_vk_target_read_z24s8(target,got,N)==VK_SUCCESS && !memcmp(words,got,N*4));
    memset(got,0xa5,N*4);
    PACKED_REQUIRE(ch_vk_target_read_z24s8(target,got,N-1)==VK_ERROR_INITIALIZATION_FAILED && got[0]==0xa5a5a5a5);
    puts("PASS: 65536 exact packed D24S8 GPU words, all depth bit positions/stencil byte values and float-control roundtrip");failed=0;
done:
    ch_vk_target_destroy(target);free(words);free(got);free(depth);free(stencil);free(rgba);return failed;
#undef PACKED_REQUIRE
}

/* Resident and strict round-trip paths must make the same depth decisions.
 * The main suite retains D32S8 to verify existing float-depth API clients. */
/* Native D24 compares incoming fixed-point depth. The legacy D32 roundtrip
 * instead compares an unrounded incoming float to a previously rounded float.
 * Preserve a concrete counterexample before attempting a faster D24 fallback. */
static int comparison_contracts(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    ChVkTarget *targets[2]={0};unsigned char rgba[2][4];int failed=1;
    const float black[]={0,0,0,1};const uint16_t indices[]={0,1,2};
    ChVkColorVertex v[]={{{-1,-1,.5f,1},{1,0,0,1}},{{3,-1,.5f,1},{1,0,0,1}},{{-1,3,.5f,1},{1,0,0,1}}};
#define CONTRACT(x) do {if(!(x)){fprintf(stderr,"depth comparison contract failed line%d\n",__LINE__);goto done;}}while(0)
    for(unsigned path=0;path<2;path++) {
        CONTRACT(ch_vk_target_create_depth(gpu,device,queue,family,1,1,path?VK_FORMAT_D32_SFLOAT_S8_UINT:VK_FORMAT_D24_UNORM_S8_UINT,&targets[path])==VK_SUCCESS);
        CONTRACT(ch_vk_target_clear_read(targets[path],black,rgba[path],4)==VK_SUCCESS);
        if(path) CONTRACT(ch_vk_target_depth_quantization(targets[path],1)==VK_SUCCESS);
    }
    for(unsigned draw=0;draw<2;draw++) {
        for(unsigned i=0;i<3;i++){v[i].rgba[0]=!draw;v[i].rgba[1]=draw;}
        Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=1,.depth_write=1,.depth_func=draw?0x202:0x207};
        for(unsigned path=0;path<2;path++) CONTRACT(ch_vk_target_draw_state_read(targets[path],NULL,v,3,indices,3,&state,rgba[path],4)==VK_SUCCESS);
    }
    CONTRACT(rgba[0][0]==0 && rgba[0][1]==255 && rgba[1][0]==255 && rgba[1][1]==0);
    puts("PASS: contract distinction: repeated z=0.5 EQUAL passes native D24, fails legacy D32 post-draw rounding");failed=0;
done:for(unsigned path=0;path<2;path++)ch_vk_target_destroy(targets[path]);return failed;
#undef CONTRACT
}
int ch_test_depth24(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    enum {W=13,H=9,N=W*H};
    ChVkTarget *target[2]={0};
    unsigned char rgba[2][N*4],stencil[N],got_stencil[N];
    float depth[N],got_depth[N];
    int failed=0;
#define REQUIRE(x) do {if(!(x)) {fprintf(stderr,"D24 failure line %d: %s\n",__LINE__,#x);failed=1;goto done;}}while(0)
    VkResult r=ch_vk_target_create_depth(gpu,device,queue,family,W,H,VK_FORMAT_D24_UNORM_S8_UINT,&target[0]);
    if(r==VK_ERROR_FORMAT_NOT_SUPPORTED) {puts("SKIP: native D24S8 attachment unsupported");return 0;}
    REQUIRE(r==VK_SUCCESS);
    REQUIRE(!packed_depth(gpu,device,queue,family));
    REQUIRE(!comparison_contracts(gpu,device,queue,family));
    REQUIRE(ch_vk_target_create_depth(gpu,device,queue,family,W,H,VK_FORMAT_D24_UNORM_S8_UINT,&target[1])==VK_SUCCESS);
    const float black[]={0,0,0,1};
    const uint32_t values[]={0,1,2,0x123456,0x7fffff,0x800000,0xfffffe,0xffffff};
    for(unsigned i=0;i<N;i++) {depth[i]=(float)((double)values[i%8]/16777215.0);stencil[i]=(i*37)&255;}
    for(unsigned path=0;path<2;path++) {
        REQUIRE(ch_vk_target_clear_read(target[path],black,rgba[path],sizeof rgba[path])==VK_SUCCESS);
        REQUIRE(ch_vk_target_write_depth_stencil(target[path],depth,stencil,N)==VK_SUCCESS);
        REQUIRE(ch_vk_target_read_depth_stencil(target[path],got_depth,got_stencil,N)==VK_SUCCESS);
        REQUIRE(!memcmp(depth,got_depth,sizeof depth) && !memcmp(stencil,got_stencil,sizeof stencil));
    }
    /* Cover clear, attachment precision, EQUAL and LESS without a CPU
     * conversion on path zero, with strict conversion on path one. */
    const uint16_t indices[]={0,1,2,0,2,3};
    const float z[]={.5f,.5f,.5001f,.4999f};
    const uint32_t funcs[]={0x207,0x202,0x201,0x201};
    for(unsigned draw=0;draw<4;draw++) {
        ChVkColorVertex quad[]={
            {{-1,-1,z[draw],1},{1,0,0,1}},{{1,-1,z[draw],1},{1,0,0,1}},
            {{1,1,z[draw],1},{1,0,0,1}},{{-1,1,z[draw],1},{1,0,0,1}}};
        for(unsigned i=0;i<4;i++) {quad[i].rgba[0]=draw==0;quad[i].rgba[1]=draw==1;quad[i].rgba[2]=draw>=2;}
        Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=1,.depth_write=1,
            .depth_func=funcs[draw],.stencil_enable=1,.stencil_func=0x207,.stencil_ref=0x71,
            .stencil_func_mask=255,.stencil_write_mask=255,.stencil_fail=0x1e00,
            .stencil_zfail=0x1e00,.stencil_zpass=0x1e01};
        for(unsigned path=0;path<2;path++) {
            if(!draw) REQUIRE(ch_vk_target_clear_read(target[path],black,rgba[path],sizeof rgba[path])==VK_SUCCESS);
            REQUIRE(ch_vk_target_draw_state_read(target[path],NULL,quad,4,indices,6,&state,rgba[path],sizeof rgba[path])==VK_SUCCESS);
            if(path) {
                REQUIRE(ch_vk_target_read_depth_stencil(target[path],got_depth,got_stencil,N)==VK_SUCCESS);
                REQUIRE(ch_vk_target_write_depth_stencil(target[path],got_depth,got_stencil,N)==VK_SUCCESS);
            }
        }
        REQUIRE(!memcmp(rgba[0],rgba[1],sizeof rgba[0]));
        for(unsigned i=0;i<N;i++) {
            unsigned channel=draw==0?0:draw<3?1:2;
            REQUIRE(rgba[0][i*4+channel]==255 && rgba[0][i*4+3]==255);
        }
    }
    REQUIRE(ch_vk_target_read_depth_stencil(target[0],got_depth,got_stencil,N)==VK_SUCCESS);
    for(unsigned i=0;i<N;i++) REQUIRE(got_stencil[i]==0x71 && fabsf(got_depth[i]-.4999f)<1e-7f);
    depth[0]=NAN;
    REQUIRE(ch_vk_target_write_depth_stencil(target[0],depth,stencil,N)==VK_ERROR_INITIALIZATION_FAILED);
done:
    ch_vk_target_destroy(target[0]);ch_vk_target_destroy(target[1]);
    if(!failed) puts("PASS: native D24S8 exact packed depth/stencil transfers and resident-vs-roundtrip EQUAL/LESS decisions");
    return failed;
#undef REQUIRE
}
