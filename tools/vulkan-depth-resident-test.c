#include "vk_depth_resident.h"
#include "vk_depth_guest.h"
#include "vk_backend.h"
#include <stdio.h>
#include <string.h>
void nv2a_pb_exec_method(uint32_t,uint32_t,uint32_t);
static uint32_t read_word(const unsigned char *p) {uint32_t n;memcpy(&n,p,4);return n;}
static void put_word(unsigned char *p,uint32_t n) {memcpy(p,&n,4);}
static int depth_resident_mode(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family,
    void *guest,size_t bytes,ChVkDecodeTexture decode,int packed) {
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
    REQUIRE(ch_vk_depth_resident_packed(z,packed)==VK_SUCCESS);
    REQUIRE(ch_vk_depth_resident_bind(z,a,&surface,&state)==VK_SUCCESS);
    REQUIRE(ch_vk_depth_resident_packed(z,!packed)==VK_ERROR_INITIALIZATION_FAILED);
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
    /* CPU edits to a clean target need no GPU transfer at completion. They
     * must be observed before its next bind, including a shared-target copy. */
    REQUIRE(ch_vk_depth_resident_bind(z,a,&surface,&state)==VK_SUCCESS);
    ch_vk_depth_resident_stats(z,&stats);
    uint64_t idle_merges=stats.cpu_merges,idle_writebacks=stats.writebacks;
    put_word(ram+1024,0x6000005a);
    REQUIRE(ch_vk_depth_resident_flush(z)==VK_SUCCESS && read_word(ram+1024)==0x6000005a);
    ch_vk_depth_resident_stats(z,&stats);
    REQUIRE(stats.cpu_merges==idle_merges && stats.writebacks==idle_writebacks);
    REQUIRE(ch_vk_depth_resident_bind(z,b,&surface,&state)==VK_SUCCESS);
    REQUIRE(ch_vk_target_read_depth_stencil(b,depth,stencil,N)==VK_SUCCESS);
    REQUIRE(depth[0]==(float)(6291456.0/16777215.0) && stencil[0]==0x5a);
    ch_vk_depth_resident_stats(z,&stats);REQUIRE(stats.cpu_merges==idle_merges+1);
    REQUIRE(ch_vk_depth_resident_release(z)==VK_SUCCESS);
    /* An all-component/full-extent clear supersedes dirty GPU data and CPU
     * edits. It must not import old pixels or publish superseded contents. */
    ram[1040]=0xcd;ram[1041]=0xab; /* prior shifted-zeta case used these bytes */
    ChVkDepthResidentStats before;ch_vk_depth_resident_stats(z,&before);
    REQUIRE(ch_vk_depth_resident_bind_clear(z,a,&surface,&state)==VK_SUCCESS);
    REQUIRE(ch_vk_target_clear_aspects_read(a,6,NULL,.25f,0x31,rgba,sizeof rgba)==VK_SUCCESS);
    REQUIRE(ch_vk_depth_resident_dirty(z,a)==VK_SUCCESS);
    put_word(ram+1024,0xabcdef12);
    REQUIRE(ch_vk_depth_resident_bind_clear(z,b,&surface,&state)==VK_SUCCESS);
    REQUIRE(ch_vk_target_clear_aspects_read(b,6,NULL,.5f,0x42,rgba,sizeof rgba)==VK_SUCCESS);
    REQUIRE(ch_vk_depth_resident_dirty(z,b)==VK_SUCCESS);
    REQUIRE(read_word(ram+1024)==0xabcdef12);
    REQUIRE(ch_vk_depth_resident_bind(z,b,&surface,&state)==VK_SUCCESS);
    REQUIRE(ch_vk_target_read_depth_stencil(b,depth,stencil,N)==VK_SUCCESS);
    for(unsigned i=0;i<N;i++) REQUIRE(depth[i]==(float)(8388608.0/16777215.0) && stencil[i]==0x42);
    ch_vk_depth_resident_stats(z,&stats);
    REQUIRE(stats.imports==before.imports && stats.writebacks==before.writebacks &&
        stats.clear_skips==before.clear_skips+2 && stats.cpu_merges==before.cpu_merges);
    REQUIRE(ch_vk_depth_resident_flush(z)==VK_SUCCESS && read_word(ram+1024)==0x80000042);
    REQUIRE(ram[1040]==0xcd && ram[1041]==0xab);
    REQUIRE(ch_vk_depth_resident_release(z)==VK_SUCCESS);
    Nv2aRenderState rect=state;
    REQUIRE(ch_vk_nv2a_clear_depth_full(&surface,&rect,3));
    REQUIRE(!ch_vk_nv2a_clear_depth_full(&surface,&rect,1) && !ch_vk_nv2a_clear_depth_full(&surface,&rect,2));
    rect.clear_rect_set=3;rect.clear_rect_h=rect.clear_rect_v=3u<<16;
    REQUIRE(ch_vk_nv2a_clear_depth_full(&surface,&rect,0xf3));
    rect.clear_rect_h=2u<<16;REQUIRE(!ch_vk_nv2a_clear_depth_full(&surface,&rect,3));
    rect.clear_rect_h=(3u<<16)|1;REQUIRE(!ch_vk_nv2a_clear_depth_full(&surface,&rect,3));
    rect.clear_rect_h=rect.clear_rect_v=65535u<<16;REQUIRE(ch_vk_nv2a_clear_depth_full(&surface,&rect,3));
    rect.clear_rect_set=1;REQUIRE(!ch_vk_nv2a_clear_depth_full(&surface,&rect,3));
    /* Registered backend must publish depth before the real executor's
     * semaphore write; precise query results remain synchronous. */
    ChVkBackendConfig config={.physical=gpu,.device=device,.queue=queue,.family=family,
        .guest_base=guest,.guest_bytes=bytes,.decode=decode,.precise_queries=1,
        .resident_color=1,.resident_depth=1,.depth_float_transfers=!packed,.cache_textures=1};
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
    if(!failed) printf("PASS: resident D24 %s transfers: component CPU merges, padding, target/shared/overlapping zeta ownership, texture/palette alias publication and real semaphore visibility\n",packed?"packed":"float");
    return failed;
#undef REQUIRE
}
int ch_test_depth_resident(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family,
    void *guest,size_t bytes,ChVkDecodeTexture decode) {
    for(int packed=0;packed<2;packed++) if(depth_resident_mode(gpu,device,queue,family,guest,bytes,decode,packed)) return 1;
    return 0;
}
