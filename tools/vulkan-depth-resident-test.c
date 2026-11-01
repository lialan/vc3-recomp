#include "vk_depth_resident.h"
#include "vk_depth_guest.h"
#include "vk_backend.h"
#include <stdio.h>
#include <string.h>
void nv2a_pb_exec_method(uint32_t,uint32_t,uint32_t);
static uint32_t read_word(const unsigned char *p) {uint32_t n;memcpy(&n,p,4);return n;}
static void put_word(unsigned char *p,uint32_t n) {memcpy(p,&n,4);}
int ch_test_depth_copy(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    ChVkTarget *a=NULL,*b=NULL,*wrong=NULL;int failed=1;
    unsigned char rgba[64],stencil[16];float depth[16];
    const float blue[]={0,0,1,1},green[]={0,1,0,1};
#define COPY_REQUIRE(x) do {if(!(x)) {fprintf(stderr,"depth copy failure line%d: %s\n",__LINE__,#x);goto done;}}while(0)
    COPY_REQUIRE(ch_vk_target_create(gpu,device,queue,family,4,4,&a)==VK_SUCCESS);
    COPY_REQUIRE(ch_vk_target_create(gpu,device,queue,family,4,4,&b)==VK_SUCCESS);
    COPY_REQUIRE(ch_vk_target_create(gpu,device,queue,family,3,4,&wrong)==VK_SUCCESS);
    COPY_REQUIRE(ch_vk_target_clear_read(wrong,green,rgba,sizeof rgba)==VK_SUCCESS);
    COPY_REQUIRE(ch_vk_target_batching(a,1)==VK_SUCCESS && ch_vk_target_batching(b,1)==VK_SUCCESS);
    COPY_REQUIRE(ch_vk_target_enable_queries(b)==VK_SUCCESS);
    COPY_REQUIRE(ch_vk_target_clear_read(a,blue,rgba,sizeof rgba)==VK_SUCCESS);
    COPY_REQUIRE(ch_vk_target_clear_read(b,green,rgba,sizeof rgba)==VK_SUCCESS);
    for(unsigned i=0;i<16;i++) {depth[i]=.6f+i/64.f;stencil[i]=(unsigned char)(i+1);}
    COPY_REQUIRE(ch_vk_target_write_depth_stencil(a,depth,stencil,16)==VK_SUCCESS);
    ch_vk_target_defer_color_readback(a,1);ch_vk_target_defer_color_readback(b,1);
    ChVkColorVertex v[]={{{-1,-1,.25f,1},{1,0,0,1}},{{1,-1,.25f,1},{1,0,0,1}},
        {{1,1,.25f,1},{1,0,0,1}},{{-1,1,.25f,1},{1,0,0,1}}};
    const uint16_t indices[]={0,1,2,0,2,3};
    Nv2aRenderState color={.color_mask=0x01010101};
    COPY_REQUIRE(ch_vk_target_draw_state_read(b,NULL,v,4,indices,6,&color,rgba,sizeof rgba)==VK_SUCCESS);
    v[1].xy[0]=v[2].xy[0]=0;
    Nv2aRenderState ds={.depth_test_enable=1,.depth_write=1,.depth_func=0x207,
        .stencil_enable=1,.stencil_func=0x207,.stencil_ref=0x7b,.stencil_func_mask=255,.stencil_write_mask=255,
        .stencil_fail=0x1e00,.stencil_zfail=0x1e00,.stencil_zpass=0x1e01};
    COPY_REQUIRE(ch_vk_target_draw_state_read(a,NULL,v,4,indices,6,&ds,rgba,sizeof rgba)==VK_SUCCESS);
    COPY_REQUIRE(ch_vk_target_pending(a) && ch_vk_target_pending(b));
    COPY_REQUIRE(ch_vk_target_copy_depth_stencil(a,wrong)==VK_ERROR_INITIALIZATION_FAILED && ch_vk_target_pending(a));
    COPY_REQUIRE(ch_vk_target_copy_depth_stencil(a,b)==VK_SUCCESS);
    COPY_REQUIRE(!ch_vk_target_pending(a) && !ch_vk_target_pending(b));
    COPY_REQUIRE(vkGetFenceStatus(device,ch_vk_target_completion_fence(a))==VK_SUCCESS &&
        vkGetFenceStatus(device,ch_vk_target_completion_fence(b))==VK_SUCCESS);
    COPY_REQUIRE(ch_vk_target_read_depth_stencil(b,depth,stencil,16)==VK_SUCCESS);
    for(unsigned i=0;i<16;i++) {
        COPY_REQUIRE(depth[i]==(i%4<2?.25f:.6f+i/64.f));
        COPY_REQUIRE(stencil[i]==(i%4<2?0x7b:i+1));
    }
    COPY_REQUIRE(ch_vk_target_read_color(a,rgba,sizeof rgba)==VK_SUCCESS);
    for(unsigned i=0;i<64;i++) COPY_REQUIRE(rgba[i]==(i%4>=2?255:0));
    COPY_REQUIRE(ch_vk_target_read_color(b,rgba,sizeof rgba)==VK_SUCCESS);
    for(unsigned i=0;i<64;i++) COPY_REQUIRE(rgba[i]==((i%4==0 || i%4==3)?255:0));
    v[1].xy[0]=v[2].xy[0]=1;
    ds.color_mask=0x01010101;ds.depth_func=ds.stencil_func=0x202;ds.depth_write=ds.stencil_write_mask=0;
    uint32_t samples=0;
    COPY_REQUIRE(ch_vk_target_draw_count_read(b,NULL,v,4,indices,6,&ds,NULL,&samples,rgba,sizeof rgba)==VK_SUCCESS && samples==8);
    COPY_REQUIRE(ch_vk_target_copy_depth_stencil(b,b)==VK_SUCCESS);
    puts("PASS: exact GPU depth/stencil copy completes pending source/destination, preserves both colors, restores depth/stencil tests and rejects extent mismatch");failed=0;
done:
    ch_vk_target_destroy(wrong);ch_vk_target_destroy(a);ch_vk_target_destroy(b);return failed;
#undef COPY_REQUIRE
}
int ch_test_depth_resident(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family,
    void *guest,size_t bytes,ChVkDecodeTexture decode) {
    enum {W=4,H=4,N=W*H};
    ChVkTarget *a=NULL,*b=NULL;ChVkDepthResident *z=NULL;ChVkBackend *backend=NULL;
    unsigned char *ram=guest,rgba[N*4],stencil[N];float depth[N];int failed=0;
    const float black[]={0,0,0,1};
    Nv2aSurface surface={.color_va=2048,.width=W,.height=H,.pitch=W*4,.bytes_per_pixel=4};
    Nv2aRenderState state={.surface_format=0x128,.zeta_va=1024,.zeta_pitch=24,.color_mask=0x01010101};
#define REQUIRE(x) do {if(!(x)) {fprintf(stderr,"resident depth failure line %d: %s\n",__LINE__,#x);failed=1;goto done;}}while(0)
    REQUIRE(bytes>=4096);
    VkResult r=ch_vk_target_create_depth(gpu,device,queue,family,W,H,VK_FORMAT_D24_UNORM_S8_UINT,&a);
    if(r==VK_ERROR_FORMAT_NOT_SUPPORTED) {puts("SKIP: resident depth fixture requires native D24S8");return 0;}
    REQUIRE(r==VK_SUCCESS);
    REQUIRE(ch_vk_target_create_depth(gpu,device,queue,family,W,H,VK_FORMAT_D24_UNORM_S8_UINT,&b)==VK_SUCCESS);
    REQUIRE(ch_vk_target_clear_read(a,black,rgba,sizeof rgba)==VK_SUCCESS);
    REQUIRE(ch_vk_target_clear_read(b,black,rgba,sizeof rgba)==VK_SUCCESS);
    memset(ram+1024,0xab,96);
    for(unsigned y=0;y<H;y++) for(unsigned x=0;x<W;x++) put_word(ram+1024+y*24+x*4,0xffffff01);
    REQUIRE(ch_vk_depth_resident_create(guest,bytes,&z)==VK_SUCCESS);
    REQUIRE(ch_vk_depth_resident_bind(z,a,&surface,&state)==VK_SUCCESS);
    REQUIRE(ch_vk_target_clear_aspects_read(a,6,NULL,.25f,7,rgba,sizeof rgba)==VK_SUCCESS);
    REQUIRE(ch_vk_depth_resident_dirty(z,a)==VK_SUCCESS);
    for(unsigned i=0;i<100;i++) REQUIRE(ch_vk_depth_resident_bind(z,a,&surface,&state)==VK_SUCCESS);
    REQUIRE(read_word(ram+1024)==0xffffff01); /* still GPU resident */
    ChVkDepthResidentStats stats;ch_vk_depth_resident_stats(z,&stats);
    REQUIRE(stats.imports==1 && stats.writebacks==0 && stats.cpu_merges==0);
    /* Edit depth in pixel zero, stencil in pixel one. Neither edit may
     * resurrect the other component from stale guest RAM. */
    put_word(ram+1024,0xbfffff01);put_word(ram+1028,0xffffff2a);
    REQUIRE(ch_vk_depth_resident_bind(z,a,&surface,&state)==VK_SUCCESS);
    REQUIRE(ch_vk_target_read_depth_stencil(a,depth,stencil,N)==VK_SUCCESS);
    REQUIRE(depth[0]==(float)(12582911.0/16777215.0) && stencil[0]==7);
    REQUIRE(depth[1]==(float)(4194304.0/16777215.0) && stencil[1]==0x2a);
    REQUIRE(depth[2]==depth[1] && stencil[2]==7);
    ram[1024+16]=0xcd; /* padding is not a depth edit */
    REQUIRE(ch_vk_depth_resident_bind(z,a,&surface,&state)==VK_SUCCESS);
    ch_vk_depth_resident_stats(z,&stats);REQUIRE(stats.cpu_merges==1);
    REQUIRE(ch_vk_depth_resident_flush(z)==VK_SUCCESS);
    REQUIRE(read_word(ram+1024)==0xbfffff07 && read_word(ram+1028)==0x4000002a && read_word(ram+1032)==0x40000007);
    REQUIRE(ram[1040]==0xcd && ram[1041]==0xab);
    /* Exact shared-zeta switches remain GPU owned, including dirty contents. */
    REQUIRE(ch_vk_depth_resident_bind(z,b,&surface,&state)==VK_SUCCESS);
    REQUIRE(ch_vk_target_clear_aspects_read(b,6,NULL,.5f,3,rgba,sizeof rgba)==VK_SUCCESS);
    REQUIRE(ch_vk_depth_resident_dirty(z,b)==VK_SUCCESS);
    REQUIRE(ch_vk_depth_resident_bind(z,a,&surface,&state)==VK_SUCCESS);
    REQUIRE(ch_vk_target_read_depth_stencil(a,depth,stencil,N)==VK_SUCCESS);
    for(unsigned i=0;i<N;i++) REQUIRE(depth[i]==(float)(8388608.0/16777215.0) && stencil[i]==3);
    ch_vk_depth_resident_stats(z,&stats);
    REQUIRE(stats.imports==1 && stats.writebacks==1 && stats.gpu_copies==2);
    REQUIRE(read_word(ram+1024)==0xbfffff07); /* no intermediate CPU publication */
    put_word(ram+1024,0xbfffff5a); /* stencil-only CPU edit while GPU depth is .5 */
    REQUIRE(ch_vk_depth_resident_bind(z,b,&surface,&state)==VK_SUCCESS);
    REQUIRE(ch_vk_target_read_depth_stencil(b,depth,stencil,N)==VK_SUCCESS);
    REQUIRE(depth[0]==(float)(8388608.0/16777215.0) && stencil[0]==0x5a);
    for(unsigned i=0;i<100;i++) REQUIRE(ch_vk_depth_resident_bind(z,i&1?a:b,&surface,&state)==VK_SUCCESS);
    ch_vk_depth_resident_stats(z,&stats);
    REQUIRE(stats.imports==1 && stats.writebacks==1 && stats.gpu_copies==102 && stats.cpu_merges==2);
    /* Diagnostic control must preserve the same bits through CPU publication. */
    ch_vk_depth_resident_gpu_copies(z,0);
    REQUIRE(ch_vk_depth_resident_bind(z,b,&surface,&state)==VK_SUCCESS);
    REQUIRE(read_word(ram+1024)==0x8000005a);
    REQUIRE(ch_vk_target_read_depth_stencil(b,depth,stencil,N)==VK_SUCCESS && stencil[0]==0x5a);
    ch_vk_depth_resident_gpu_copies(z,1);
    /* Overlapping zeta interpretation must also transfer ownership. */
    Nv2aRenderState shifted=state;shifted.zeta_va+=4;
    REQUIRE(ch_vk_depth_resident_bind(z,b,&surface,&shifted)==VK_SUCCESS);
    float expected[N];unsigned char expected_stencil[N];
    REQUIRE(ch_vk_guest_depth_load(&surface,&shifted,guest,bytes,expected,expected_stencil)==VK_SUCCESS);
    REQUIRE(ch_vk_target_read_depth_stencil(b,depth,stencil,N)==VK_SUCCESS);
    REQUIRE(!memcmp(depth,expected,sizeof depth) && !memcmp(stencil,expected_stencil,sizeof stencil));
    REQUIRE(ch_vk_depth_resident_check_color(z,shifted.zeta_va-4,8)==VK_ERROR_FEATURE_NOT_PRESENT);
    REQUIRE(ch_vk_depth_resident_check_color(z,3000,16)==VK_SUCCESS);
    Nv2aRenderState invalid=state;invalid.zeta_va=(uint32_t)bytes-1;
    REQUIRE(ch_vk_depth_resident_bind(z,a,&surface,&invalid)==VK_ERROR_INITIALIZATION_FAILED);
    /* Texture starts before the depth allocation, but overlaps its first row. */
    REQUIRE(ch_vk_target_clear_aspects_read(b,6,NULL,.25f,0x71,rgba,sizeof rgba)==VK_SUCCESS);
    REQUIRE(ch_vk_depth_resident_dirty(z,b)==VK_SUCCESS);
    Nv2aTexture tx={.offset=1008,.width=8,.height=4,.pitch=32,.color=0x12};
    REQUIRE(ch_vk_depth_resident_sample(z,&tx)==VK_SUCCESS);
    REQUIRE(read_word(ram+shifted.zeta_va)==0x40000071);
    REQUIRE(ch_vk_target_clear_aspects_read(b,6,NULL,.5f,0x42,rgba,sizeof rgba)==VK_SUCCESS);
    REQUIRE(ch_vk_depth_resident_dirty(z,b)==VK_SUCCESS);
    tx=(Nv2aTexture){.offset=3040,.palette=1024,.width=4,.height=4,.color=0x0b};
    REQUIRE(ch_vk_depth_resident_sample(z,&tx)==VK_SUCCESS);
    REQUIRE(read_word(ram+shifted.zeta_va)==0x80000042);
    /* Morton storage uses the same component merge at swizzled addresses. */
    Nv2aRenderState morton=state;morton.surface_format=0x02020228;morton.zeta_va=256;
    for(unsigned i=0;i<N;i++) put_word(ram+256+i*4,0xffffff01);
    REQUIRE(ch_vk_depth_resident_bind(z,a,&surface,&morton)==VK_SUCCESS);
    REQUIRE(ch_vk_target_clear_aspects_read(a,6,NULL,.25f,7,rgba,sizeof rgba)==VK_SUCCESS);
    REQUIRE(ch_vk_depth_resident_dirty(z,a)==VK_SUCCESS);
    put_word(ram+256+8*4,0xffffff2a); /* Morton (0,2), linear pixel 8 */
    REQUIRE(ch_vk_depth_resident_bind(z,a,&surface,&morton)==VK_SUCCESS);
    REQUIRE(ch_vk_target_read_depth_stencil(a,depth,stencil,N)==VK_SUCCESS);
    REQUIRE(depth[8]==(float)(4194304.0/16777215.0) && stencil[8]==0x2a && stencil[2]==7);
    REQUIRE(ch_vk_depth_resident_flush(z)==VK_SUCCESS && read_word(ram+256+8*4)==0x4000002a);
    REQUIRE(ch_vk_depth_resident_release(z)==VK_SUCCESS);
    /* Registered backend must publish depth before the real executor's
     * semaphore write; precise query results remain synchronous. */
    ChVkBackendConfig config={.physical=gpu,.device=device,.queue=queue,.family=family,
        .guest_base=guest,.guest_bytes=bytes,.decode=decode,.precise_queries=1,
        .resident_color=1,.resident_depth=1,.cache_textures=1};
    REQUIRE(ch_vk_backend_create(&config,&backend)==VK_SUCCESS && ch_vk_backend_attach(backend)==VK_SUCCESS);
    const Nv2aBackend *cb=ch_vk_backend_callbacks();REQUIRE(cb->flush);
    cb->clear(&surface,&state,0xf3,0xffff0000,0x40000017);
    REQUIRE(read_word(ram+1024)!=0x40000017);
    nv2a_pb_set_semaphore_target(3968);nv2a_pb_exec_method(0,0x1d70,0x12345678);
    REQUIRE(read_word(ram+1024)==0x40000017 && read_word(ram+3968)==0x12345678);
    state.depth_test_enable=1;state.depth_write=1;state.depth_func=0x201;state.depth_max=1;
    Nv2aVertex vertices[6]={0};
    const float xy[6][2]={{0,0},{4,0},{4,4},{0,0},{4,4},{0,4}};
    for(unsigned i=0;i<6;i++) {vertices[i].x=xy[i][0];vertices[i].y=xy[i][1];
        vertices[i].z=.5f;vertices[i].rhw=1;vertices[i].diffuse=0xff00ff00;}
    uint32_t visible=7;
    Nv2aBatch batch={.vertices=vertices,.count=6,.state=&state,.zpass_out=&visible};
    cb->draw(&surface,&batch);REQUIRE(visible==7); /* behind resident .25 */
    for(unsigned i=0;i<6;i++) vertices[i].z=.125f;
    cb->draw(&surface,&batch);REQUIRE(visible==23); /* precise 16-pixel query */
    REQUIRE(read_word(ram+1024)==0x40000017);
    nv2a_pb_exec_method(0,0x1d70,0x87654321);
    REQUIRE(read_word(ram+1024)==0x20000017 && read_word(ram+3968)==0x87654321);
    Nv2aSurface small=surface;small.width=small.height=2;
    cb->clear(&small,&state,1,0,0xbfffff00);cb->flush();
    for(unsigned y=0;y<H;y++) for(unsigned x=0;x<W;x++)
        REQUIRE(read_word(ram+1024+y*24+x*4)==(x<2 && y<2?0xbfffff17:0x20000017));
    cb->draw(&surface,&batch);REQUIRE(visible==27); /* only four pixels pass after growth */
    cb->flush();
    for(unsigned y=0;y<H;y++) for(unsigned x=0;x<W;x++) REQUIRE(read_word(ram+1024+y*24+x*4)==0x20000017);
done:
    nv2a_pb_set_semaphore_target(0);
    ch_vk_backend_destroy(backend);
    if(z) ch_vk_depth_resident_release(z);
    ch_vk_depth_resident_destroy(z);ch_vk_target_destroy(a);ch_vk_target_destroy(b);
    if(!failed) puts("PASS: resident D24 component CPU merges, padding, target/shared/overlapping zeta ownership, texture/palette alias publication and real semaphore visibility");
    return failed;
#undef REQUIRE
}
