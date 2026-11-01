#include "vk_depth_guest.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#define CHECK(c) do { if(!(c)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c);return 1;} } while(0)

int main(void)
{
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
    memcpy(before,guest,sizeof guest);
    CHECK(ch_vk_guest_depth_store(&surface,&state,guest,256+127,depth,stencil)!=VK_SUCCESS);
    CHECK(!memcmp(before,guest,sizeof guest));
    depth[31]=NAN;
    CHECK(ch_vk_guest_depth_store(&surface,&state,guest,sizeof guest,depth,stencil)!=VK_SUCCESS);
    CHECK(!memcmp(before,guest,sizeof guest));depth[31]=1;
    state.surface_format=0x110;state.zeta_va=8;state.zeta_pitch=20;
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
    puts("PASS: rectangular Morton color/Z24S8/Z16, pitched Z16, padding, bounds and atomic rejection");
    return 0;
}
