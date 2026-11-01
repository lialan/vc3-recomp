/* Registered shared backend: program execution, clipping, queued constants,
 * complete program cache identity, and guest publication without OpenGL. */
#include "vk_backend.h"
#include "vk_fixed.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int raw_snapshots(void) {
    ChVkFixedScratch scratch={0};ChVkFixedGeometry g={0};int failed=1;
    unsigned char source[16][8192];uint32_t program[16][4]={{0}};float constants[192][4]={{0}};
    uint16_t indices[6];const unsigned types[]={0,1,2,4,5,6},counts[]={1,4,17,129,4};
#define SNAP(x) do {if(!(x)){fprintf(stderr,"raw snapshot line%d: %s\n",__LINE__,#x);goto done;}}while(0)
    for(unsigned a=0;a<16;a++) {program[a][1]=a<<9;program[a][2]=2u<<26;program[a][3]=a==15;
        for(unsigned i=0;i<sizeof source[a];i++)source[a][i]=(unsigned char)(i*7+a*13);}
    Nv2aRawGeometry raw={.kind=NV2A_RAW_PROGRAM,.program=program,.program_len=16,.constants=constants,.min_index=37};
    for(unsigned type=0;type<6;type++) for(unsigned size=1;size<=4;size++) for(unsigned trial=0;trial<5;trial++) {
        unsigned count=counts[trial],element=types[type]==0 || types[type]==6?4:types[type]==1 || types[type]==5?size*2:types[type]==4?size:size*4;
        raw.max_index=raw.min_index+count-1;raw.prim=5+trial%3;raw.indices=indices;raw.index_count=raw.prim==5?6:4;
        for(unsigned i=0;i<6;i++)indices[i]=i%count;
        for(unsigned a=0;a<16;a++) raw.attr[a]=(Nv2aAttrib){.data=a==15?NULL:source[a],.type=types[type],.size=size,
            .stride=trial==0?0:element+3,.bytes=(count-1)*(trial==0?0:element+3)+element,.value={3,4,5,6}};
        SNAP(ch_vk_raw_prepare_reuse(&raw,&g,&scratch)==VK_SUCCESS && g.borrowed==3 && g.vertex_count==count);
        for(unsigned a=0;a<16;a++) for(unsigned i=0;i<count;i++) {
            float expected[4]={0,0,0,1};
            if(a==15)memcpy(expected,raw.attr[a].value,sizeof expected);
            else memcpy(expected,source[a]+i*raw.attr[a].stride,element);
            SNAP(!memcmp((unsigned char *)g.vertices+i*256+a*16,expected,16));
            SNAP(g.params.attribute_format[a]==(a==15?0:(size<<8)|(types[type]+1)));
        }
        for(unsigned i=0;i<g.index_count;i++) {
            unsigned tri=i/3,lane=i%3;
            unsigned at=raw.prim==5?i:raw.prim==7?(lane==0?0:tri+lane):lane==0?tri+(tri&1):lane==1?tri+((tri&1)^1):tri+2;
            SNAP(g.indices[i]==indices[at]);
        }
        ch_vk_fixed_release(&g);
        // Rejection after a valid prepare must not free or poison retained storage.
        size_t bytes=raw.attr[0].bytes;raw.attr[0].bytes=0;
        SNAP(ch_vk_raw_prepare_reuse(&raw,&g,&scratch)==VK_ERROR_FEATURE_NOT_PRESENT && !g.vertices && !g.indices);
        raw.attr[0].bytes=bytes;
    }
    SNAP(scratch.vertex_bytes<=(16u<<20) && scratch.index_bytes<=(1u<<20));
    puts("PASS: reusable encoded raw snapshots across all formats/sizes, constant and zero-stride streams, growth/shrink, topology and invalid-span recovery");failed=0;
done:
    ch_vk_fixed_release(&g);ch_vk_fixed_scratch_release(&scratch);return failed;
#undef SNAP
}
static int decode(const Nv2aTexture *t,uint32_t *p) {(void)t;(void)p;return 0;}
static int registered_fixed(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    unsigned char guest[4096]={0};ChVkBackend *backend=NULL;int failed=1;
    ChVkBackendConfig config={.physical=gpu,.device=device,.queue=queue,.family=family,
        .guest_base=guest,.guest_bytes=sizeof guest,.decode=decode,.raw_fixed=1,.precise_queries=1,.resident_color=1,.batch_draws=1,.gpu_color_pack=1};
    if(ch_vk_backend_create(&config,&backend)!=VK_SUCCESS || ch_vk_backend_attach(backend)!=VK_SUCCESS) goto done;
    const Nv2aBackend *cb=ch_vk_backend_callbacks();
    Nv2aFixedState ff={.use_composite=1,.tex0_slot=9};for(unsigned i=0;i<4;i++) ff.composite[i*4+i]=1;
    ff.composite[0]=ff.composite[5]=8;ff.c59[0]=ff.c59[1]=8;
    const float positions[][4]={{-1,-1,.3f,1},{1,-1,.3f,1},{1,1,.3f,1},{-1,1,.3f,1}};
    const uint16_t strip[]={0,1,3,2},fan[]={0,1,2,3};
    Nv2aRawGeometry raw={.prim=6,.indices=strip,.index_count=4,.min_index=37,.max_index=40,
        .kind=NV2A_RAW_FIXED,.ff=&ff,.aa_sx=1,.aa_sy=1,.uv_scale0={1,1}};
    raw.attr[0]=(Nv2aAttrib){.data=positions,.type=2,.size=4,.stride=16,.bytes=sizeof positions};
    Nv2aSurface surface={.color_va=128,.width=16,.height=16,.pitch=64,.bytes_per_pixel=4};
    Nv2aRenderState state={.color_mask=0x01010101,.depth_max=1};uint32_t samples=0;
    Nv2aBatch batch={.raw=&raw,.state=&state,.zpass_out=&samples};
    for(unsigned primitive=6;primitive<=7;primitive++) {
        raw.prim=primitive;raw.indices=primitive==6?strip:fan;samples=0;
        cb->clear(&surface,&state,0xf3,0,0xffffff00);
        if(!cb->draw_raw || cb->draw_raw(&surface,&batch)!=1 || samples!=256 || !cb->flush) goto done;
        cb->flush();
        for(unsigned i=0;i<256;i++) {uint32_t pixel;memcpy(&pixel,guest+128+i*4,4);if(pixel!=0xffffffff) goto done;}
    }
    int16_t packed_positions[][4]={{-1,-1,0,1},{1,-1,0,1},{1,1,0,1},{-1,1,0,1}};
    raw.attr[0]=(Nv2aAttrib){.data=packed_positions,.type=5,.size=4,.stride=8,.bytes=sizeof packed_positions};
    batch.zpass_out=NULL;ff.has_color=1;ff.color_slot=3;
    const uint32_t packed_colors[]={0xffff0000,0xffff0000,0xffff0000,0xffff0000};
    raw.attr[3]=(Nv2aAttrib){.data=packed_colors,.type=0,.size=4,.stride=4,.bytes=sizeof packed_colors};
    if(cb->draw_raw(&surface,&batch)!=1) goto done;
    ff.has_color=0; /* Shader constants must already have been snapshotted. */
    for(unsigned i=0;i<256;i++) {uint32_t pixel;memcpy(&pixel,guest+128+i*4,4);if(pixel!=0xffffffff) goto done;}
    cb->flush();
    for(unsigned i=0;i<256;i++) {uint32_t pixel;memcpy(&pixel,guest+128+i*4,4);if(pixel!=0xffff0000) goto done;}
    batch.zpass_out=&samples;
    raw.kind=NV2A_RAW_PROGRAM;samples=19;
    if(cb->draw_raw(&surface,&batch)!=0 || samples!=19) goto done;
    raw.kind=NV2A_RAW_FIXED;raw.attr[0].bytes=4;
    if(cb->draw_raw(&surface,&batch)!=0 || samples!=19) goto done;
    puts("PASS: registered raw fixed GPU callbacks, rebased strip/fan indices, precise query256, resident guest flush; unsupported program/bounds decline before drawing");failed=0;
done:
    ch_vk_backend_destroy(backend);return failed;
}
int ch_test_program_render(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    if(raw_snapshots()) return 1;
    if(registered_fixed(gpu,device,queue,family)) return 1;
    unsigned char guest[4096]={0};ChVkBackend *backend=NULL;int failed=1;
#define REQUIRE(x) do {if(!(x)) {fprintf(stderr,"program render line %d: %s\n",__LINE__,#x);goto done;}} while(0)
    ChVkBackendConfig config={.physical=gpu,.device=device,.queue=queue,.family=family,
        .guest_base=guest,.guest_bytes=sizeof guest,.decode=decode,.raw_fixed=1,
        .precise_queries=1,.resident_color=1,.batch_draws=1};
    REQUIRE(ch_vk_backend_create(&config,&backend)==VK_SUCCESS);
    REQUIRE(ch_vk_backend_attach(backend)==VK_SUCCESS);
    const Nv2aBackend *cb=ch_vk_backend_callbacks();
    float positions[4][4]={{0,0,.3f,1},{16,0,.3f,1},{16,16,.3f,1},{0,16,.3f,1}};
    uint16_t strip[]={0,1,3,2},fan[]={0,1,2,3};
    uint32_t program[136][4]={{0}};float constants[192][4]={{0}};
    program[0][1]=(1u<<21)|0x1b;program[0][2]=2u<<26;
    program[0][3]=(15u<<12)|(1u<<11);
    program[1][1]=(1u<<21)|(96u<<13)|0x1b;program[1][2]=3u<<26;
    program[1][3]=(15u<<12)|(1u<<11)|(3u<<3)|1u;
    Nv2aRawGeometry raw={.prim=6,.indices=strip,.index_count=4,.min_index=37,.max_index=40,
        .kind=NV2A_RAW_PROGRAM,.program=program,.program_len=2,.constants=constants,
        .aa_sx=1,.aa_sy=1,.uv_scale0={1,1}};
    raw.attr[0]=(Nv2aAttrib){.data=positions,.type=2,.size=4,.stride=16,.bytes=sizeof positions};
    Nv2aSurface surface={.color_va=128,.width=16,.height=16,.pitch=64,.bytes_per_pixel=4};
    Nv2aRenderState state={.color_mask=0x01010101,.depth_max=1};uint32_t samples=0;
    Nv2aBatch batch={.raw=&raw,.state=&state,.zpass_out=&samples};
    for(unsigned test=0;test<4;test++) {
        constants[96][0]=test==0;constants[96][1]=test==1;constants[96][2]=test>=2;constants[96][3]=1;
        raw.prim=test&1?7:6;raw.indices=test&1?fan:strip;
        if(test==2) { // Same hash/count, changed instruction words.
            program[1][1]=(1u<<21)|(97u<<13)|0x1b;
            memcpy(constants[97],constants[96],16);constants[96][0]=1;
        }
        if(test==3) { // +0 position.w has the reference screen-space meaning.
            for(unsigned i=0;i<4;i++) positions[i][3]=0;
        }
        samples=0;cb->clear(&surface,&state,0xf3,0,0xffffff00);
        REQUIRE(cb->draw_raw && cb->draw_raw(&surface,&batch)==1);
        REQUIRE(samples==256);cb->flush();
        uint32_t expected=test==0?0xffff0000:test==1?0xff00ff00:0xff0000ff;
        for(unsigned i=0;i<256;i++) {uint32_t pixel;memcpy(&pixel,guest+128+i*4,4);REQUIRE(pixel==expected);}
    }
    // Queued draw owns constants even when the caller overwrites them.
    batch.zpass_out=NULL;constants[97][0]=1;constants[97][2]=0;
    REQUIRE(cb->draw_raw(&surface,&batch)==1);memset(constants,0,sizeof constants);
    cb->flush();
    for(unsigned i=0;i<256;i++) {uint32_t pixel;memcpy(&pixel,guest+128+i*4,4);REQUIRE(pixel==0xffff0000);}
    batch.zpass_out=&samples;
    for(unsigned i=0;i<4;i++) positions[i][3]=-1;
    samples=0;REQUIRE(cb->draw_raw(&surface,&batch)==1);REQUIRE(samples==0);
    for(unsigned i=0;i<4;i++) positions[i][3]=1;
    raw.near_z=1;raw.vp_off_z=.5f;
    samples=0;REQUIRE(cb->draw_raw(&surface,&batch)==1);REQUIRE(samples==0);
    raw.program_len=137;REQUIRE(cb->draw_raw(&surface,&batch)==0);raw.program_len=2;
    raw.attr[0].bytes=4;REQUIRE(cb->draw_raw(&surface,&batch)==0);
    puts("PASS: GPU program rendering, indexed strip/fan, precise visibility, constants snapshot, instruction identity, +0 w, w/near clipping, bounds rejection");failed=0;
done:
    ch_vk_backend_destroy(backend);return failed;
}
