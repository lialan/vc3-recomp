#include "vk_depth_guest.h"
#include "vk_guest_layout.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define CHECK(c) do { if(!(c)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c);return 1;} } while(0)

int main(void)
{
    /* Compare every coordinate in small allocations and deterministic edge/
     * random coordinates in all 256 supported exponent pairs against the
     * shared GL-side mask-deposit definition. Include clipped rectangles:
     * their allocation dimensions, not the clip, determine the layout. */
    uint32_t seed=0x7153a9bdu;
    for(unsigned wx=0;wx<=15;wx++) for(unsigned hy=0;hy<=15;hy++) {
        uint32_t aw=1u<<wx,ah=1u<<hy,mx,my;
        xbox_swizzle_masks(aw,ah,&mx,&my);
        for(unsigned bpp=2;bpp<=4;bpp+=2) for(unsigned clip=0;clip<2;clip++) {
            uint32_t w=clip?(aw+1)/2:aw,h=clip?(ah+1)/2:ah;
            ChVkGuestLayout layout;
            CHECK(ch_vk_guest_layout((hy<<24)|(wx<<16)|0x200,w,h,0,bpp,&layout)==VK_SUCCESS);
            uint64_t reference_last=(uint64_t)(swizzle_deposit(w-1,mx)|swizzle_deposit(h-1,my))*bpp;
            CHECK(layout.span==reference_last+bpp);
            uint32_t count=wx+hy<=12?w*h:512;
            for(uint32_t i=0;i<count;i++) {
                seed=seed*1664525u+1013904223u;uint32_t x=seed%w;
                seed=seed*1664525u+1013904223u;uint32_t y=seed%h;
                if(wx+hy<=12) {x=i%w;y=i/w;}
                else if(i<4) {x=(i&1)?w-1:0;y=(i&2)?h-1:0;}
                CHECK(ch_vk_guest_offset(&layout,x,y)==
                    (uint64_t)(swizzle_deposit(x,mx)|swizzle_deposit(y,my))*bpp);
            }
        }
    }
    unsigned char guest[1024],before[1024],rgba[128],loaded[128];
    Nv2aSurface surface={.color_va=16,.width=8,.height=4,.pitch=2560,.bytes_per_pixel=4};
    Nv2aRenderState state={.surface_format=0x02030228,.zeta_va=256,.zeta_pitch=2560};
    memset(guest,0xa5,sizeof guest);
    for(unsigned i=0;i<32;i++) {
        rgba[4*i]=(unsigned char)(i*7);rgba[4*i+1]=(unsigned char)(255-i*3);
        rgba[4*i+2]=(unsigned char)(i^0x5a);rgba[4*i+3]=(unsigned char)(128+i);
    }
    CHECK(ch_vk_nv2a_store_color_state(&surface,&state,rgba,sizeof rgba,guest,sizeof guest)==VK_SUCCESS);
    /* In an 8x4 Morton image, (x=2,y=1) has offset 6, independently of the
     * stale pitched-surface state. Check actual byte order as well as roundtrip. */
    CHECK(guest[16+6*4]==rgba[10*4+2] && guest[16+6*4+2]==rgba[10*4]);
    CHECK(guest[15]==0xa5 && guest[16+128]==0xa5);
    CHECK(ch_vk_nv2a_load_color_state(&surface,&state,loaded,sizeof loaded,guest,sizeof guest)==VK_SUCCESS);
    CHECK(!memcmp(rgba,loaded,sizeof rgba));
    float depth[32],back[32];unsigned char stencil[32],stencil_back[32];
    for(unsigned i=0;i<32;i++) {depth[i]=(float)i/31;stencil[i]=(unsigned char)(i^0x6d);}
    CHECK(ch_vk_guest_depth_store(&surface,&state,guest,sizeof guest,depth,stencil)==VK_SUCCESS);
    CHECK(guest[256+6*4]==stencil[10]);
    CHECK(ch_vk_guest_depth_load(&surface,&state,guest,sizeof guest,back,stencil_back)==VK_SUCCESS);
    for(unsigned i=0;i<32;i++) CHECK(fabsf(depth[i]-back[i])<=1.0f/16777215);
    CHECK(!memcmp(stencil,stencil_back,32));
    uint32_t words[32],got[32];
    for(unsigned i=0;i<32;i++) words[i]=i*0x97e35a17u;
    CHECK(ch_vk_guest_z24s8_store(&surface,&state,guest,sizeof guest,words)==VK_SUCCESS);
    CHECK(ch_vk_guest_z24s8_load(&surface,&state,guest,sizeof guest,got)==VK_SUCCESS && !memcmp(words,got,sizeof words));
    CHECK(guest[256+6*4]==(unsigned char)words[10] && guest[256+6*4+3]==(unsigned char)(words[10]>>24));
    Nv2aRenderState pitched=state;pitched.surface_format=0x128;pitched.zeta_pitch=40;
    memset(guest+256,0xa5,160);
    CHECK(ch_vk_guest_z24s8_store(&surface,&pitched,guest,sizeof guest,words)==VK_SUCCESS);
    CHECK(ch_vk_guest_z24s8_load(&surface,&pitched,guest,sizeof guest,got)==VK_SUCCESS && !memcmp(words,got,sizeof words));
    for(unsigned y=0;y<4;y++) for(unsigned pad=32;pad<40;pad++) CHECK(guest[256+y*40+pad]==0xa5);
    memcpy(before,guest,sizeof guest);
    CHECK(ch_vk_guest_z24s8_store(&surface,&pitched,guest,256+151,words)!=VK_SUCCESS && !memcmp(before,guest,sizeof guest));
    CHECK(ch_vk_guest_depth_store(&surface,&state,guest,sizeof guest,depth,stencil)==VK_SUCCESS);
    memcpy(before,guest,sizeof guest);
    CHECK(ch_vk_guest_depth_store(&surface,&state,guest,256+127,depth,stencil)!=VK_SUCCESS);
    CHECK(!memcmp(before,guest,sizeof guest));
    depth[31]=NAN;
    CHECK(ch_vk_guest_depth_store(&surface,&state,guest,sizeof guest,depth,stencil)!=VK_SUCCESS);
    CHECK(!memcmp(before,guest,sizeof guest));depth[31]=1;
    state.surface_format=0x110;state.zeta_va=8;state.zeta_pitch=20;
    CHECK(ch_vk_guest_z24s8_load(&surface,&state,guest,sizeof guest,got)==VK_ERROR_FORMAT_NOT_SUPPORTED);
    memset(guest,0xa5,sizeof guest);
    CHECK(ch_vk_guest_depth_store(&surface,&state,guest,sizeof guest,depth,stencil)==VK_SUCCESS);
    CHECK(guest[8]==0 && guest[9]==0 && guest[8+3*20+14]==255 && guest[8+3*20+15]==255);
    for(unsigned row=0;row<4;row++) for(unsigned pad=16;pad<20;pad++) CHECK(guest[8+row*20+pad]==0xa5);
    CHECK(ch_vk_guest_depth_load(&surface,&state,guest,sizeof guest,back,stencil_back)==VK_SUCCESS);
    for(unsigned i=0;i<32;i++) {CHECK(fabsf(depth[i]-back[i])<=1.0f/65535);CHECK(stencil_back[i]==0);}
    state.surface_format=0x02030218; /* Swizzled Z16, same rectangular geometry. */
    CHECK(ch_vk_guest_depth_store(&surface,&state,guest,sizeof guest,depth,stencil)==VK_SUCCESS);
    CHECK(ch_vk_guest_depth_load(&surface,&state,guest,sizeof guest,back,stencil_back)==VK_SUCCESS);
    for(unsigned i=0;i<32;i++) CHECK(fabsf(depth[i]-back[i])<=1.0f/65535);
    state.surface_format=0x02020228; /* 4x4 allocation cannot hold an 8x4 clip. */
    memcpy(before,guest,sizeof guest);
    CHECK(ch_vk_nv2a_store_color_state(&surface,&state,rgba,sizeof rgba,guest,sizeof guest)!=VK_SUCCESS);
    CHECK(!memcmp(before,guest,sizeof guest));
    /* Odd widths, unaligned guest bases and odd row strides must retain the
     * scalar byte path's exact channel/alpha/padding and alias behavior. */
    Nv2aSurface color={.color_va=513,.width=17,.height=3,.pitch=75,.bytes_per_pixel=4};
    Nv2aRenderState linear={.surface_format=0x128};
    unsigned char input[204],output[204],legacy[1024],alias[1024];
    const char *old_fast=getenv("CHIHIRO_VULKAN_COLOR_FAST_IO");char *saved_fast=old_fast?strdup(old_fast):NULL;
    for(unsigned i=0;i<sizeof input;i++) input[i]=(unsigned char)(i*173+19);
    for(unsigned fast=0;fast<2;fast++) {
        setenv("CHIHIRO_VULKAN_COLOR_FAST_IO",fast?"1":"0",1);memset(guest,0xa5,sizeof guest);
        CHECK(ch_vk_nv2a_store_color_state(&color,&linear,input,sizeof input,guest,sizeof guest)==VK_SUCCESS);
        CHECK(guest[513]==input[2] && guest[514]==input[1] && guest[515]==input[0] && guest[516]==input[3]);
        if(!fast) memcpy(legacy,guest,sizeof guest);else CHECK(!memcmp(legacy,guest,sizeof guest));
        CHECK(ch_vk_nv2a_load_color_state(&color,&linear,output,sizeof output,guest,sizeof guest)==VK_SUCCESS && !memcmp(input,output,sizeof input));
        memset(output,0xcd,sizeof output);memcpy(before,guest,sizeof guest);
        CHECK(ch_vk_nv2a_store_color_state(&color,&linear,input,sizeof input,guest,730)!=VK_SUCCESS && !memcmp(before,guest,sizeof guest));
        CHECK(ch_vk_nv2a_load_color_state(&color,&linear,output,sizeof output,guest,730)!=VK_SUCCESS);
        for(unsigned i=0;i<sizeof output;i++) CHECK(output[i]==0xcd);
        CHECK(ch_vk_nv2a_load_color_state(&color,&linear,guest+513,sizeof input,guest,sizeof guest)==VK_SUCCESS);
        CHECK(!memcmp(guest+513,input,sizeof input));
        if(!fast) memcpy(alias,guest,sizeof guest);else CHECK(!memcmp(alias,guest,sizeof guest));
    }
    if(saved_fast) {setenv("CHIHIRO_VULKAN_COLOR_FAST_IO",saved_fast,1);free(saved_fast);}
    else unsetenv("CHIHIRO_VULKAN_COLOR_FAST_IO");
    /* Exact scalar oracle for unaligned, padded linear depth publication. */
    for(unsigned bpp=2;bpp<=4;bpp+=2) {
        Nv2aSurface ds={.width=17,.height=3};
        Nv2aRenderState dr={.surface_format=bpp==2?0x110:0x120,.zeta_va=7,.zeta_pitch=17*bpp+3};
        float dz[51];uint8_t st[51],expected[1024];
        memset(guest,0xa5,sizeof guest);memset(expected,0xa5,sizeof expected);
        for(unsigned i=0;i<51;i++) {
            dz[i]=(float)i/50;st[i]=(uint8_t)(i*37);
            uint32_t z=(uint32_t)((double)dz[i]*(bpp==2?65535.0:16777215.0)+0.5);
            uint32_t w=bpp==2?z:(z<<8)|st[i];
            size_t off=7+(i/17)*dr.zeta_pitch+(i%17)*bpp;
            for(unsigned b=0;b<bpp;b++) expected[off+b]=(uint8_t)(w>>(8*b));
        }
        CHECK(ch_vk_guest_depth_store(&ds,&dr,guest,sizeof guest,dz,st)==VK_SUCCESS);
        CHECK(!memcmp(guest,expected,sizeof guest));
    }
    puts("PASS: rectangular Morton color/Z24S8/Z16, packed depth, pitched/unaligned ARGB fast-vs-scalar rows, padding, bounds, alias loads and atomic rejection");
    return 0;
}
