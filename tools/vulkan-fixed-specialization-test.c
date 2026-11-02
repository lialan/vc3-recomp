/* Immutable fixed-function shader state must preserve dynamic-path pixels and visibility. */
#include "vk_fixed.h"
#include "vk_nv2a.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void identity(float m[16]) {memset(m,0,16*sizeof(float));for(unsigned i=0;i<4;i++)m[i*5]=1;}
int ch_test_fixed_specialization(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    enum {W=32,H=32};unsigned char pixels[2][W*H*4];ChVkTarget *target[2]={0};int failed=1;
    const char *setting=getenv("CHIHIRO_VULKAN_SPECIALIZE_FETCH");char *saved=setting?strdup(setting):NULL;
#define REQUIRE(x) do {if(!(x)) {fprintf(stderr,"fixed specialization line%d: %s\n",__LINE__,#x);goto done;}}while(0)
    for(unsigned path=0;path<2;path++) {
        setenv("CHIHIRO_VULKAN_SPECIALIZE_FETCH",path?"1":"0",1);
        REQUIRE(ch_vk_target_create(gpu,device,queue,family,W,H,&target[path])==VK_SUCCESS);
        REQUIRE(ch_vk_target_enable_queries(target[path])==VK_SUCCESS);
    }
    const float black[]={0,0,0,0};const uint16_t indices[]={0,1,2,0,2,3};
    for(unsigned test=0;test<48;test++) {
        ChVkFixedParams p={.surface={W,H},.depth={0,1},.texture={1,1},.aa={1,1},.uv_scale0={1,1}};
        Nv2aFixedState *f=&p.state;
        identity(f->composite);f->composite[0]=W*.5f;f->composite[5]=H*.5f;f->composite[10]=.4f;
        identity(f->proj);f->proj[0]=W*.5f;f->proj[5]=H*.5f;f->proj[10]=.4f;
        f->c59[0]=W*.5f;f->c59[1]=H*.5f;f->has_color=1;f->has_normal=1;f->use_composite=1;
        for(unsigned k=0;k<4;k++) {identity(f->mv[k]);identity(f->texmat[k]);p.stage_scale[k][0]=p.stage_scale[k][1]=1;}
        ChVkFixedVertex vertices[4]={0};
        const float positions[4][2]={{-.67f,-.63f},{.61f,-.63f},{.61f,.73f},{-.67f,.73f}};
        for(unsigned i=0;i<4;i++) {
            float (*a)[4]=vertices[i].attributes;a[0][0]=positions[i][0];a[0][1]=positions[i][1];a[0][2]=.3f;a[0][3]=1;
            a[1][0]=.2f;a[1][1]=.3f;a[1][2]=.1f;a[1][3]=.4f;a[2][2]=1;a[2][3]=1;
            a[3][0]=.2f+i*.1f;a[3][1]=.7f-i*.1f;a[3][2]=.8f;a[3][3]=1;
            for(unsigned k=4;k<8;k++) {a[k][0]=.17f+i*.13f;a[k][1]=.23f+i*.11f;a[k][2]=.37f;a[k][3]=1;}
        }
        unsigned mode=0;
        if(test>=4 && test<11) {f->use_composite=0;f->skin_mode=test-4;for(unsigned k=0;k<4;k++) f->mv[k][3]=k*.073f;}
        if(test>=11 && test<20) {
            f->lighting=1;f->scene_amb[0]=.1f;f->scene_amb[1]=.05f;f->mat_alpha=.8f;
            f->colmat_amb=test&1;f->colmat_dif=(test>>1)&1;f->flip_normals=(test>>2)&1;
            for(unsigned k=0;k<(test==19?8u:1u);k++) {
                f->light_kind[k]=test&1?1:2;float *l=f->light[k];l[0]=.1f;l[1]=.05f;l[5]=.4f;l[6]=.6f;
                l[10]=1;l[14]=1;l[16]=1;l[3]=100;
            }
        }
        if(test>=20 && test<40) {
            mode=1+(test%4);unsigned k=mode-1;f->texgen_on[k]=1;
            for(unsigned c=0;c<3;c++) f->texgen[k][c]=test%3==0?0x8511:test%3==1?0x8512:0x2402;
            f->texmat_on[k]=test&1;f->texmat[k][0]=.73f;f->texmat[k][5]=.81f;f->texmat[k][3]=.13f;
        }
        if(test>=40 && test<44) {mode=5;p.fog_enable=1;p.fog_mode=0x800+(test-40);p.fog_param[0]=.8f;p.fog_param[1]=.13f;}
        if(test==44) {f->composite[12]=.4f;f->composite[15]=.7f;}
        if(test==45) {p.near_z=1;f->composite[8]=.8f;f->composite[11]=-.1f;}
        if(test==46) f->composite[15]=0;
        if(test==47) {f->composite[12]=.75f;f->composite[15]=.2f;}
        if(test==1) {p.aa[0]=p.aa[1]=2;f->composite[0]*=.5f;f->composite[5]*=.5f;f->c59[0]*=.5f;f->c59[1]*=.5f;}
        if(test==2) f->has_color=0;
        if(test==3) {f->composite[0]*=.83f;f->composite[3]=1.7f;f->composite[7]=-.9f;}
        Nv2aRenderState state={.color_mask=0x01010101,.depth_max=1};Nv2aCombiner rc={0};
        if(mode) {
            rc.control=1;rc.stage_program=mode<5?4u<<((mode-1)*5):0;
            unsigned reg=mode<5?8+mode-1:0x13;
            rc.color_icw[0]=(reg<<24)|0x00200000;rc.alpha_icw[0]=mode<5?((reg|0x10)<<24)|0x00200000:0x30200000;
            rc.color_ocw[0]=rc.alpha_ocw[0]=0xc0;
        }
        uint32_t samples=0;
        f->color_slot=3;f->tex0_slot=9;
        Nv2aRawGeometry raw={.prim=5,.indices=indices,.index_count=6,.min_index=17,.max_index=20,
            .kind=NV2A_RAW_FIXED,.ff=f,.aa_sx=p.aa[0],.aa_sy=p.aa[1],.near_z=p.near_z,.vp_off_z=p.vp_off_z,
            .fog_enable=p.fog_enable,.fog_mode=p.fog_mode,.uv_scale0={1,1}};
        memcpy(raw.fog_param,p.fog_param,sizeof raw.fog_param);
        const unsigned roles[]={0,1,2,3,9,10,11,12};
        for(unsigned i=0;i<8;i++) raw.attr[roles[i]]=(Nv2aAttrib){.data=vertices[0].attributes[i],
            .type=2,.size=4,.stride=sizeof(ChVkFixedVertex),.bytes=3*sizeof(ChVkFixedVertex)+16};
        Nv2aSurface surface={.width=W,.height=H,.pitch=W*4,.bytes_per_pixel=4};
        Nv2aBatch batch={.raw=&raw,.state=&state,.rc=mode?&rc:NULL,.zpass_out=&samples};
        ChVkFixedGeometry fixed;
        REQUIRE(ch_vk_fixed_prepare(&raw,&fixed)==VK_SUCCESS);
        uint32_t counts[2];
        for(unsigned path=0;path<2;path++) {
            samples=0;
            REQUIRE(ch_vk_target_clear_read(target[path],black,pixels[path],sizeof pixels[path])==VK_SUCCESS);
            REQUIRE(ch_vk_nv2a_draw_fixed_resolved_read(target[path],&surface,&batch,&fixed,NULL,NULL,NULL,pixels[path],sizeof pixels[path])==VK_SUCCESS);
            counts[path]=samples;
        }
        ch_vk_fixed_release(&fixed);
        if(counts[0]!=counts[1]) fprintf(stderr,"fixed visibility case=%u counts=%u/%u\n",test,counts[0],counts[1]);
        REQUIRE(counts[0]==counts[1]);
        unsigned maximum=0;
        for(unsigned i=0;i<sizeof pixels[0];i++) {
            unsigned delta=(unsigned)abs(pixels[0][i]-pixels[1][i]);
            if(delta>maximum) maximum=delta;
        }
        if(maximum>1) fprintf(stderr,"fixed specialization case=%u maximum=%u\n",test,maximum);
        REQUIRE(maximum<=1);
    }
    puts("PASS: fixed shader specialization: 48 transform/skin/lighting/material/texgen/fog/clipping cases, exact visibility and <=1 UNORM byte");failed=0;
done:
    if(saved) setenv("CHIHIRO_VULKAN_SPECIALIZE_FETCH",saved,1);else unsetenv("CHIHIRO_VULKAN_SPECIALIZE_FETCH");
    free(saved);ch_vk_target_destroy(target[0]);ch_vk_target_destroy(target[1]);return failed;
#undef REQUIRE
}
