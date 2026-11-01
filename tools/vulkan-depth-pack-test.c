/* Exact D32 float + stencil <-> guest Z24/S8, including odd-sized buffers. */
#include "vk_target.h"
#include "vk_depth_resident.h"
#include "../d3d/d3d8_swizzle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
int ch_test_depth_fragment(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    ChVkTarget *target=NULL,*peer=NULL;ChVkDepthResident *owner=NULL;void *guest=NULL;int failed=1;uint32_t *input=NULL,*output=NULL;
    float *depth=NULL;uint8_t *stencil=NULL,*rgba=NULL;
    const unsigned sizes[][2]={{1,1},{3,5},{17,9},{512,512}};uint32_t seed=12345;
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"depth pack line%d: %s\n",__LINE__,#x);goto done;}}while(0)
    for(unsigned test=0;test<8;test++) {
        unsigned w=sizes[test%4][0],h=sizes[test%4][1],n=w*h;
        input=malloc(n*4);output=malloc(n*4);depth=malloc(n*4);stencil=malloc(n);rgba=malloc(n*4);
        REQUIRE(input && output && depth && stencil && rgba);
        REQUIRE(ch_vk_target_create(gpu,device,queue,family,w,h,&target)==VK_SUCCESS);
        REQUIRE(ch_vk_target_color_bgra(target,test>=4)==VK_SUCCESS);
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
            // In-place GPU color conversion must preserve every byte and its
            // inverse across reuse with float/packed depth staging.
            for(unsigned i=0;i<n*4;i++) rgba[i]=(uint8_t)(i*79u+repeat*37u);
            REQUIRE(ch_vk_target_import_color(target,rgba,n*4)==VK_SUCCESS);
            REQUIRE(ch_vk_target_read_color(target,output,n*4)==VK_SUCCESS);
            REQUIRE(!memcmp(rgba,output,n*4));
            REQUIRE(ch_vk_target_color_bgra(target,test<4)==VK_SUCCESS);
            REQUIRE(ch_vk_target_read_color(target,output,n*4)==VK_SUCCESS);
            for(unsigned i=0;i<n;i++) {
                const uint8_t *q=(const uint8_t *)output+i*4;
                REQUIRE(q[0]==rgba[i*4+2] && q[1]==rgba[i*4+1] && q[2]==rgba[i*4] && q[3]==rgba[i*4+3]);
            }
            REQUIRE(ch_vk_target_color_bgra(target,test>=4)==VK_SUCCESS);
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
        for(unsigned i=0;i<n;i++) {REQUIRE(output[i]==0x40000071);REQUIRE(rgba[i*4+(test>=4?2:0)]==255 && rgba[i*4+1]==0 && rgba[i*4+(test>=4?0:2)]==0);}
        ch_vk_target_destroy(target);target=NULL;free(input);input=NULL;free(output);output=NULL;free(depth);depth=NULL;free(stencil);stencil=NULL;free(rgba);rgba=NULL;
    }
    const unsigned morton_sizes[][2]={{1,1},{1,8},{8,1},{4,16},{16,4},{128,128},{256,256}};
    for(unsigned test=0;test<14;test++) {
        unsigned w=morton_sizes[test%7][0],h=morton_sizes[test%7][1],n=w*h;
        input=malloc(n*4);output=malloc(n*4);depth=malloc(n*4);stencil=malloc(n);rgba=malloc(n*4);
        REQUIRE(input && output && depth && stencil && rgba);
        VkResult created=ch_vk_target_create_depth(gpu,device,queue,family,w,h,
            test<7?VK_FORMAT_D32_SFLOAT_S8_UINT:VK_FORMAT_D24_UNORM_S8_UINT,&target);
        if(created==VK_ERROR_FORMAT_NOT_SUPPORTED && test>=7) {
            free(input);input=NULL;free(output);output=NULL;free(depth);depth=NULL;free(stencil);stencil=NULL;free(rgba);rgba=NULL;continue;
        }
        REQUIRE(created==VK_SUCCESS);
        const float black[]={0,0,0,1};REQUIRE(ch_vk_target_clear_read(target,black,rgba,n*4)==VK_SUCCESS);
        for(unsigned repeat=0;repeat<3;repeat++) {
            REQUIRE(ch_vk_target_color_layout(target,0)==VK_SUCCESS);
            for(unsigned i=0;i<n;i++) {seed=seed*1664525u+1013904223u;input[i]=seed;}
            REQUIRE(ch_vk_target_import_color(target,input,n*4)==VK_SUCCESS);
            for(unsigned i=0;i<n;i++) {uint32_t v=input[i];output[i]=(v&0xff00ff00)|((v&255)<<16)|((v>>16)&255);}
            xbox_swizzle_rect(depth,output,w,h,4);
            REQUIRE(ch_vk_target_color_layout(target,2)==VK_SUCCESS);
            REQUIRE(ch_vk_target_read_color(target,output,n*4)==VK_SUCCESS);
            REQUIRE(!memcmp(output,depth,n*4));
            // CPU guest edit in Morton order, then reverse layout conversion.
            output[n/2]^=0x12345678;memcpy(depth,output,n*4);
            REQUIRE(ch_vk_target_import_color(target,output,n*4)==VK_SUCCESS);
            // Reuse the main staging plane for packed depth, paired color still
            // returns the unchanged Morton snapshot in its second plane.
            for(unsigned i=0;i<n;i++) input[i]=0x60000071;
            REQUIRE(ch_vk_target_write_z24s8(target,input,n)==VK_SUCCESS);
            REQUIRE(ch_vk_target_read_depth_color(target,output,NULL,n,1,rgba,n*4)==VK_SUCCESS);
            REQUIRE(!memcmp(input,output,n*4) && !memcmp(rgba,depth,n*4));
            REQUIRE(ch_vk_target_color_layout(target,0)==VK_SUCCESS);
            REQUIRE(ch_vk_target_read_color(target,input,n*4)==VK_SUCCESS);
            xbox_unswizzle_rect(output,depth,w,h,4);
            for(unsigned i=0;i<n;i++) {uint32_t v=output[i];REQUIRE(input[i]==((v&0xff00ff00)|((v&255)<<16)|((v>>16)&255)));}
            REQUIRE(ch_vk_target_depth_morton(target,0)==VK_SUCCESS);
            for(unsigned i=0;i<n;i++) {seed=seed*1664525u+1013904223u;input[i]=seed;}
            REQUIRE(ch_vk_target_write_z24s8(target,input,n)==VK_SUCCESS);
            xbox_swizzle_rect(output,input,w,h,4);
            REQUIRE(ch_vk_target_depth_morton(target,1)==VK_SUCCESS);
            REQUIRE(ch_vk_target_read_z24s8(target,(uint32_t *)depth,n)==VK_SUCCESS);
            REQUIRE(!memcmp(output,depth,n*4));
            output[n/2]^=0x91827364;
            REQUIRE(ch_vk_target_write_z24s8(target,output,n)==VK_SUCCESS);
            REQUIRE(ch_vk_target_read_depth_color(target,depth,NULL,n,1,rgba,n*4)==VK_SUCCESS);
            REQUIRE(!memcmp(output,depth,n*4));
            REQUIRE(ch_vk_target_depth_morton(target,0)==VK_SUCCESS);
            REQUIRE(ch_vk_target_read_z24s8(target,input,n)==VK_SUCCESS);
            xbox_unswizzle_rect(depth,output,w,h,4);
            REQUIRE(!memcmp(input,depth,n*4));
        }
        // Shared owner must preserve per-component guest edits in Morton
        // order, including GPU copies to a second image and back to linear.
        guest=malloc((size_t)n*8+8);REQUIRE(guest);
        for(unsigned i=0;i<n;i++) {seed=seed*1664525u+1013904223u;input[i]=seed;}
        memcpy((char *)guest+4,input,n*4);
        Nv2aSurface surface={.width=w,.height=h,.pitch=w*4,.bytes_per_pixel=4};
        unsigned wx=0,hy=0;while((1u<<wx)<w)wx++;while((1u<<hy)<h)hy++;
        Nv2aRenderState state={.surface_format=0x228|(wx<<16)|(hy<<24),.zeta_va=4,.zeta_pitch=w*4};
        REQUIRE(ch_vk_depth_resident_create(guest,(size_t)n*8+8,&owner)==VK_SUCCESS);
        ch_vk_depth_resident_gpu_layout(owner,1);
        REQUIRE(ch_vk_depth_resident_bind(owner,target,&surface,&state)==VK_SUCCESS);
        REQUIRE(ch_vk_target_read_z24s8(target,output,n)==VK_SUCCESS && !memcmp(input,output,n*4));
        for(unsigned i=0;i<n;i++) {seed=seed*1664525u+1013904223u;output[i]=seed;}
        REQUIRE(ch_vk_target_write_z24s8(target,output,n)==VK_SUCCESS);
        REQUIRE(ch_vk_depth_resident_dirty(owner,target)==VK_SUCCESS);
        input[n/2]^=255;memcpy((char *)guest+4+(n/2)*4,input+n/2,4);
        output[n/2]=(output[n/2]&0xffffff00u)|(input[n/2]&255u);
        REQUIRE(ch_vk_depth_resident_bind(owner,target,&surface,&state)==VK_SUCCESS);
        REQUIRE(ch_vk_target_create_depth(gpu,device,queue,family,w,h,
            test<7?VK_FORMAT_D32_SFLOAT_S8_UINT:VK_FORMAT_D24_UNORM_S8_UINT,&peer)==VK_SUCCESS);
        REQUIRE(ch_vk_target_clear_read(peer,black,rgba,n*4)==VK_SUCCESS);
        REQUIRE(ch_vk_depth_resident_bind(owner,peer,&surface,&state)==VK_SUCCESS);
        REQUIRE(ch_vk_depth_resident_flush(owner)==VK_SUCCESS);
        REQUIRE(!memcmp((char *)guest+4,output,n*4));
        ChVkDepthResidentStats stats;ch_vk_depth_resident_stats(owner,&stats);
        REQUIRE(!stats.cpu_layout_pixels && !stats.cpu_float_pixels && stats.cpu_merges==1 && stats.gpu_copies==1);
        // Changing the view flushes the old layout before interpreting RAM.
        state.surface_format=0x128;
        REQUIRE(ch_vk_depth_resident_bind(owner,peer,&surface,&state)==VK_SUCCESS);
        REQUIRE(ch_vk_target_read_z24s8(peer,input,n)==VK_SUCCESS && !memcmp(input,output,n*4));
        REQUIRE(ch_vk_depth_resident_release(owner)==VK_SUCCESS);
        ch_vk_depth_resident_reuse_clean(owner,1);
        for(unsigned layout=0;layout<2;layout++) {
            state.surface_format=layout?(0x228|(wx<<16)|(hy<<24)):0x128;
            Nv2aRenderState other=state;other.zeta_va=4+n*4;
            for(unsigned i=0;i<n;i++) input[i]=0x40000031;
            memcpy((char *)guest+4,input,n*4);memcpy((char *)guest+other.zeta_va,input,n*4);
            REQUIRE(ch_vk_depth_resident_bind(owner,target,&surface,&state)==VK_SUCCESS);
            for(unsigned i=0;i<n;i++) input[i]=0x60000051;
            REQUIRE(ch_vk_target_write_z24s8(target,input,n)==VK_SUCCESS);
            REQUIRE(ch_vk_depth_resident_dirty(owner,target)==VK_SUCCESS);
            REQUIRE(ch_vk_depth_resident_bind(owner,peer,&surface,&other)==VK_SUCCESS);
            REQUIRE(!memcmp((char *)guest+4,input,n*4)); // published before dormancy
            ch_vk_depth_resident_stats(owner,&stats);uint64_t hits=stats.clean_reuses,imports=stats.imports;
            REQUIRE(ch_vk_depth_resident_bind(owner,target,&surface,&state)==VK_SUCCESS);
            ch_vk_depth_resident_stats(owner,&stats);REQUIRE(stats.clean_reuses==hits+1 && stats.imports==imports);
            REQUIRE(ch_vk_target_read_z24s8(target,output,n)==VK_SUCCESS && !memcmp(input,output,n*4));
            REQUIRE(ch_vk_depth_resident_bind(owner,peer,&surface,&other)==VK_SUCCESS);
            input[0]^=0x123456ff;memcpy((char *)guest+4,input,4); // dormant CPU edit
            ch_vk_depth_resident_stats(owner,&stats);imports=stats.imports;hits=stats.clean_reuses;
            REQUIRE(ch_vk_depth_resident_bind(owner,target,&surface,&state)==VK_SUCCESS);
            ch_vk_depth_resident_stats(owner,&stats);REQUIRE(stats.imports==imports+1 && stats.clean_reuses==hits);
            REQUIRE(ch_vk_target_read_z24s8(target,output,n)==VK_SUCCESS && !memcmp(input,output,n*4));
            // A GPU copy into the dormant target must invalidate its old image key.
            REQUIRE(ch_vk_depth_resident_bind(owner,peer,&surface,&other)==VK_SUCCESS);
            REQUIRE(ch_vk_depth_resident_bind(owner,target,&surface,&other)==VK_SUCCESS);
            ch_vk_depth_resident_stats(owner,&stats);imports=stats.imports;hits=stats.clean_reuses;
            REQUIRE(ch_vk_depth_resident_bind(owner,target,&surface,&state)==VK_SUCCESS);
            ch_vk_depth_resident_stats(owner,&stats);REQUIRE(stats.imports==imports+1 && stats.clean_reuses==hits);
            REQUIRE(ch_vk_target_read_z24s8(target,output,n)==VK_SUCCESS && !memcmp(input,output,n*4));
            // A publication through partially overlapping storage changes the snapshot.
            other.zeta_va=8;
            REQUIRE(ch_vk_depth_resident_bind(owner,peer,&surface,&other)==VK_SUCCESS);
            for(unsigned i=0;i<n;i++) output[i]=0x200000b7;
            REQUIRE(ch_vk_target_write_z24s8(peer,output,n)==VK_SUCCESS);
            REQUIRE(ch_vk_depth_resident_dirty(owner,peer)==VK_SUCCESS);
            ch_vk_depth_resident_stats(owner,&stats);imports=stats.imports;hits=stats.clean_reuses;
            REQUIRE(ch_vk_depth_resident_bind(owner,target,&surface,&state)==VK_SUCCESS);
            ch_vk_depth_resident_stats(owner,&stats);REQUIRE(stats.imports==imports+(n>1) && stats.clean_reuses==hits+(n==1));
            memcpy(input,(char *)guest+4,n*4);
            REQUIRE(ch_vk_target_read_z24s8(target,output,n)==VK_SUCCESS && !memcmp(input,output,n*4));
            // Public release invalidates every borrowed image, allowing external
            // mutation/destruction before this owner is used again.
            other.zeta_va=4+n*4;
            REQUIRE(ch_vk_depth_resident_bind(owner,peer,&surface,&other)==VK_SUCCESS);
            REQUIRE(ch_vk_depth_resident_release(owner)==VK_SUCCESS);
            for(unsigned i=0;i<n;i++) output[i]=0x100000a2;
            REQUIRE(ch_vk_target_write_z24s8(target,output,n)==VK_SUCCESS);
            ch_vk_depth_resident_stats(owner,&stats);imports=stats.imports;
            REQUIRE(ch_vk_depth_resident_bind(owner,target,&surface,&state)==VK_SUCCESS);
            ch_vk_depth_resident_stats(owner,&stats);REQUIRE(stats.imports==imports+1);
            REQUIRE(ch_vk_target_read_z24s8(target,output,n)==VK_SUCCESS && !memcmp(input,output,n*4));
            REQUIRE(ch_vk_depth_resident_release(owner)==VK_SUCCESS);
        }
        ch_vk_depth_resident_destroy(owner);owner=NULL;free(guest);guest=NULL;
        ch_vk_target_destroy(peer);peer=NULL;
        REQUIRE(ch_vk_target_color_layout(target,3)==VK_ERROR_FEATURE_NOT_PRESENT);
        ch_vk_target_destroy(target);target=NULL;free(input);input=NULL;free(output);output=NULL;free(depth);depth=NULL;free(stencil);stencil=NULL;free(rgba);rgba=NULL;
    }
    puts("PASS: bounded clean depth reuse preserves publication, dormant CPU edits, GPU copy invalidation and partial overlap for D32/D24 linear/Morton images");
    puts("PASS: GPU linear/Morton RGBA/BGRA byte roundtrip and D32/D24 linear/Morton Z24S8 exact roundtrip, owner CPU component edits/GPU copies/layout changes, float packing, stencil, odd sizes, repeated staging reuse, combined pending depth/color readback");failed=0;
done:
    if(owner) ch_vk_depth_resident_release(owner);ch_vk_depth_resident_destroy(owner);free(guest);ch_vk_target_destroy(peer);
    ch_vk_target_destroy(target);free(input);free(output);free(depth);free(stencil);free(rgba);return failed;
}
