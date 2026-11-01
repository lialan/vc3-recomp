/* Real Vulkan copies/cache/coherence, with an isolated scalar ARGB fixture
 * decoder. Headless entry point injects executor registration only. */
#include "vk_backend.h"
#include "vk_texture_cache.h"
#include "vk_guest_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char memory[65536];static unsigned decodes;
static int decode(const Nv2aTexture *tx,uint32_t *argb) {
    decodes++;ChVkGuestLayout l;uint32_t format=0x100;
    if(tx->color==0x06) {
        unsigned wx=0,hy=0;while((1u<<wx)<tx->width && wx<16) wx++;while((1u<<hy)<tx->height && hy<16) hy++;
        format=0x200|(wx<<16)|(hy<<24);
    } else if(tx->color!=0x12 && tx->color!=0x1e) return 0;
    if(ch_vk_guest_layout(format,tx->width,tx->height,tx->pitch,4,&l)!=VK_SUCCESS ||
        tx->offset>sizeof memory || l.span>sizeof memory-tx->offset) return 0;
    for(unsigned y=0;y<tx->height;y++) for(unsigned x=0;x<tx->width;x++) {
        uint32_t pixel;memcpy(&pixel,memory+tx->offset+ch_vk_guest_offset(&l,x,y),4);
        argb[y*tx->width+x]=tx->color==0x1e?pixel|0xff000000u:pixel;
    }
    return 1;
}
static void release_image(ChVkSampleImage *image) {
    if(image->release) image->release(image->release_context,image->gpu);
    memset(image,0,sizeof *image);
}
static int snapshots(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    ChVkTarget *source=NULL,*destination=NULL;ChVkTexture *copy=NULL,*upload=NULL;
    ChVkTextureCache *cache=NULL;ChVkSampleImage a={0},b={0},c={0},probe={0};int failed=1;
    unsigned char rgba[1024],got[1024],expected[1024];uint32_t argb[256];const float black[]={0,0,0,0};
#define REQUIRE(x) do {if(!(x)) {fprintf(stderr,"render texture snapshot failure line%d: %s\n",__LINE__,#x);goto done;}}while(0)
    for(unsigned i=0;i<256;i++) {
        rgba[i*4]=(unsigned char)i;rgba[i*4+1]=(unsigned char)(255-i);rgba[i*4+2]=(unsigned char)(i*37);rgba[i*4+3]=(unsigned char)(i*13);
        argb[i]=((uint32_t)rgba[i*4+3]<<24)|((uint32_t)rgba[i*4]<<16)|((uint32_t)rgba[i*4+1]<<8)|rgba[i*4+2];
    }
    REQUIRE(ch_vk_target_create(gpu,device,queue,family,16,16,&source)==VK_SUCCESS);
    REQUIRE(ch_vk_target_create(gpu,device,queue,family,16,16,&destination)==VK_SUCCESS);
    REQUIRE(ch_vk_target_batching(destination,1)==VK_SUCCESS);
    REQUIRE(ch_vk_target_clear_read(source,black,got,sizeof got)==VK_SUCCESS);
    REQUIRE(ch_vk_target_import_color(source,rgba,sizeof rgba)==VK_SUCCESS);
    REQUIRE(ch_vk_target_snapshot_texture(source,5,&copy)==VK_SUCCESS);
    REQUIRE(ch_vk_texture_upload_mips(gpu,device,queue,family,16,16,argb,5,&upload)==VK_SUCCESS);
    for(unsigned level=0;level<5;level++) {
        unsigned bytes=(16u>>level)*(16u>>level)*4;
        REQUIRE(ch_vk_texture_read_level_rgba(copy,level,got,sizeof got)==VK_SUCCESS);
        REQUIRE(ch_vk_texture_read_level_rgba(upload,level,expected,sizeof expected)==VK_SUCCESS);
        REQUIRE(!memcmp(got,expected,bytes));
        if(!level) REQUIRE(!memcmp(got,rgba,sizeof rgba));
    }
    ChVkTextureCacheConfig config={.queued_uploads=1,.physical=gpu,.device=device,.queue=queue,.family=family,
        .guest_base=memory,.guest_bytes=sizeof memory,.decode=decode,.max_entries=2,.max_bytes=4096};
    REQUIRE(ch_vk_texture_cache_create(&config,&cache)==VK_SUCCESS);
    Nv2aTexture tx={.offset=256,.width=16,.height=16,.pitch=64,.color=0x12,.levels=1,.filter=0x01010000,.addr_u=3,.addr_v=3};
    REQUIRE(ch_vk_texture_cache_resolve_target(cache,&tx,source,1,&a)==VK_SUCCESS);
    REQUIRE(ch_vk_texture_cache_resolve_target(cache,&tx,source,1,&probe)==VK_SUCCESS && probe.gpu==a.gpu);
    release_image(&probe);
    REQUIRE(ch_vk_target_clear_read(destination,black,got,sizeof got)==VK_SUCCESS);
    ch_vk_target_defer_color_readback(destination,1);
    const uint16_t indices[]={0,1,2};Nv2aRenderState state={.color_mask=0x01010101};
    const ChVkColorVertex v[]={{{-1,-1,0,1,0,0},{1,1,1,1}},{{3,-1,0,1,2,0},{1,1,1,1}},{{-1,3,0,1,0,2},{1,1,1,1}}};
    REQUIRE(ch_vk_target_draw_texture_read(destination,NULL,v,3,indices,3,&state,a.gpu,got,sizeof got)==VK_SUCCESS);
    REQUIRE(ch_vk_target_retire_sample(destination,a.gpu,a.release,a.release_context,0)==VK_SUCCESS);
    memset(&a,0,sizeof a); /* Ownership moved to pending destination commands. */
    memset(expected,0xff,sizeof expected);
    REQUIRE(ch_vk_target_import_color(source,expected,sizeof expected)==VK_SUCCESS);
    REQUIRE(ch_vk_texture_cache_resolve_target(cache,&tx,source,2,&b)==VK_SUCCESS);
    memset(expected,0x33,sizeof expected);
    REQUIRE(ch_vk_target_import_color(source,expected,sizeof expected)==VK_SUCCESS);
    REQUIRE(ch_vk_texture_cache_resolve_target(cache,&tx,source,3,&probe)==VK_NOT_READY && !probe.gpu);
    release_image(&b);
    REQUIRE(ch_vk_texture_cache_resolve_target(cache,&tx,source,3,&c)==VK_SUCCESS);
    REQUIRE(ch_vk_texture_read_rgba(c.gpu,got,sizeof got)==VK_SUCCESS && !memcmp(got,expected,sizeof got));
    release_image(&c);
    tx.addr_u=1;
    REQUIRE(ch_vk_texture_cache_resolve_target(cache,&tx,source,3,&c)==VK_SUCCESS);
    release_image(&c);
    ch_vk_target_destroy(source);source=NULL;
    REQUIRE(ch_vk_target_read_color(destination,got,sizeof got)==VK_SUCCESS && !memcmp(got,rgba,sizeof got));
    ChVkTextureCacheStats stats;ch_vk_texture_cache_stats(cache,&stats);
    REQUIRE(stats.gpu_copies==4 && stats.gpu_hits==1 && stats.gpu_reuses==1 && !stats.uploads && stats.entries==2 && stats.bytes<=4096);
    puts("PASS: exact GPU snapshots preserve all UNORM bytes/mips/orientation; version and sampler keys, pin-budget rejection, source destruction before pending sampling and shared bounded cache lifetime");failed=0;
done:
    ch_vk_target_destroy(source);ch_vk_target_destroy(destination);
    release_image(&a);release_image(&b);release_image(&c);release_image(&probe);
    ch_vk_texture_cache_destroy(cache);ch_vk_texture_destroy(copy);ch_vk_texture_destroy(upload);
    return failed;
#undef REQUIRE
}
static void vertices(Nv2aVertex v[6],uint32_t color,float texels) {
    static const float xy[][2]={{0,0},{4,0},{4,4},{0,0},{4,4},{0,4}};
    for(unsigned i=0;i<6;i++) v[i]=(Nv2aVertex){.x=xy[i][0],.y=xy[i][1],.rhw=1,.diffuse=color,
        .u=xy[i][0]*texels/4,.v=xy[i][1]*texels/4};
}
static int scenario(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family,
    unsigned fast,unsigned test,unsigned char *result,unsigned *decode_count) {
    ChVkBackend *backend=NULL;int failed=1;memset(memory,0xa5,sizeof memory);decodes=0;
    ChVkBackendConfig config={.physical=gpu,.device=device,.queue=queue,.family=family,
        .guest_base=memory,.guest_bytes=sizeof memory,.decode=decode,.resident_color=1,.cache_textures=1,
        .sample_render_targets=fast,.resident_depth=test>=10 && fast,.quantized_depth=test>=10 && fast,.depth_quant_inline=test>=10 && fast==1,.depth_quant_fragment=test>=10 && fast==2,.batch_draws=1,.texture_cache_entries=2,
        .texture_cache_bytes=test==7?64:test==8?256:8192};
#define REQUIRE(x) do {if(!(x)) {fprintf(stderr,"render texture integration case%u fast%u line%d: %s\n",test,fast,__LINE__,#x);goto done;}}while(0)
    config.batch_draw_limit=257;
    REQUIRE(ch_vk_backend_create(&config,&backend)==VK_ERROR_INITIALIZATION_FAILED && !backend);
    config.batch_draw_limit=test%3==0?1:test%3==1?64:0;
    REQUIRE(ch_vk_backend_create(&config,&backend)==VK_SUCCESS && ch_vk_backend_attach(backend)==VK_SUCCESS);
    const Nv2aBackend *cb=ch_vk_backend_callbacks();
    Nv2aSurface source={.color_va=256,.width=4,.height=4,.pitch=24,.bytes_per_pixel=4};
    Nv2aSurface dest={.color_va=4096,.width=4,.height=4,.pitch=16,.bytes_per_pixel=4};
    Nv2aRenderState src_state={.color_mask=0x01010101,.surface_format=0x100,.depth_max=1};
    if(test>=10) {
        src_state.surface_format=0x120;src_state.zeta_va=8192;src_state.zeta_pitch=24;
        src_state.depth_test_enable=src_state.depth_write=1;src_state.depth_func=0x201;
    }
    Nv2aRenderState dst_state=src_state;
    if(test==5 || test==6) src_state.surface_format=0x200|((test==6?3:2)<<16)|(2u<<24);
    Nv2aTexture tx={.offset=source.color_va,.width=4,.height=4,.pitch=source.pitch,
        .color=test==5 || test==6?0x06:test==4?0x1e:0x12,.levels=1,.filter=0x01010000,.addr_u=3,.addr_v=3};
    if(test==3) {tx.offset+=4;tx.width=3;}
    cb->clear(&source,&src_state,test>=10?0xf3:0xf0,0x204080c0,0x80000007);
    cb->clear(&dest,&dst_state,0xf0,0,0xffffff00);
    Nv2aVertex src_vertices[6],dst_vertices[6];vertices(src_vertices,0x8040c020,4);vertices(dst_vertices,0xffffffff,(float)tx.width);
    Nv2aBatch draw_source={.vertices=src_vertices,.count=6,.state=&src_state};
    Nv2aBatch draw_dest={.vertices=dst_vertices,.count=6,.state=&dst_state,.texture=&tx};
    {
        Nv2aSurface empty={.color_va=0x80001bf0};
        Nv2aRenderState reset={.surface_format=0xffffffffu};
        Nv2aBatch empty_batch=draw_source;empty_batch.state=&reset;
        cb->draw(&empty,&empty_batch);
        empty.width=4;cb->draw(&empty,&empty_batch);
        empty.width=0;empty.height=4;cb->draw(&empty,&empty_batch);
    }
    cb->draw(&source,&draw_source);
    if(test>=10) {
        if(test==11) {memory[8192]=0x39;memory[8192+24+7]=0x40;}
        cb->draw(&source,&draw_source);
        if(test==12) {source.width=source.height=2;cb->clear(&source,&src_state,3,0,0x40000027);}
    }
    if(test==8) {
        dst_state.blend_enable=1;dst_state.blend_src=dst_state.blend_dst=1;dst_state.blend_eq=0x8006;
        for(unsigned i=0;i<8;i++) {
            uint32_t color=0x01000000u|((i+1)<<(16-8*(i%3)));
            cb->clear(&source,&src_state,0xf0,color,0xffffff00);
            cb->draw(&dest,&draw_dest);
        }
    } else if(test==2) {
        vertices(dst_vertices,0xff808080,4);draw_dest.state=&src_state;
        cb->draw(&source,&draw_dest); /* Own source is snapshotted before this draw. */
    } else {
        unsigned char before[96];memcpy(before,memory+source.color_va,sizeof before);
        cb->draw(&dest,&draw_dest);
        int eligible=test!=3 && test!=4 && test!=6 && test!=7;
        if(fast && eligible) REQUIRE(!memcmp(before,memory+source.color_va,sizeof before));
        if(test==1) {
            ChVkGuestLayout layout;REQUIRE(ch_vk_guest_layout(src_state.surface_format,4,4,24,4,&layout)==VK_SUCCESS);
            uint32_t cpu=0x2749a1fe;memcpy(memory+source.color_va+ch_vk_guest_offset(&layout,1,2),&cpu,4);
            cb->draw(&dest,&draw_dest);
        } else if(test==9) {
            source.width=source.height=2;
            cb->clear(&source,&src_state,0xf0,0xffa04020,0xffffff00);
        } else {
            tx.addr_u=tx.addr_v=1;cb->draw(&dest,&draw_dest); /* Independent sampler key. */
        }
    }
    cb->flush();
    if(test==8) {
        static const unsigned char expected[]={9,15,12,8}; /* Guest BGRA ordering. */
        for(unsigned i=0;i<64;i++) REQUIRE(memory[dest.color_va+i]==expected[i%4]);
    }
    memcpy(result,memory,sizeof memory);*decode_count=decodes;
    failed=0;
done:
    ch_vk_backend_destroy(backend);return failed;
#undef REQUIRE
}
static int queued_uploads(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    uint32_t argb[16*16*6];unsigned char expected[sizeof argb],got[sizeof argb];
    ChVkTexture *sync=NULL,*queued=NULL;int failed=1;
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"queued upload failure line%d: %s\n",__LINE__,#x);goto done;}}while(0)
    for(unsigned i=0;i<sizeof argb/sizeof *argb;i++) argb[i]=0xff000000u|(i*7919u);
    for(int cube=0;cube<2;cube++) {
        if(cube) CHECK(ch_vk_texture_upload_cube(gpu,device,queue,family,16,argb,&sync)==VK_SUCCESS);
        else CHECK(ch_vk_texture_upload_mips(gpu,device,queue,family,16,16,argb,5,&sync)==VK_SUCCESS);
        CHECK(ch_vk_texture_upload_queued(gpu,device,queue,family,16,16,argb,cube?1:5,cube,&queued)==VK_SUCCESS);
        for(unsigned level=0;level<(cube?1u:5u);level++) {
            size_t bytes=(16u>>level)*(16u>>level)*4*(cube?6:1);
            CHECK(ch_vk_texture_read_level_rgba(sync,level,expected,bytes)==VK_SUCCESS);
            CHECK(ch_vk_texture_read_level_rgba(queued,level,got,bytes)==VK_SUCCESS);
            CHECK(!memcmp(expected,got,bytes));
        }
        ch_vk_texture_destroy(sync);sync=NULL;ch_vk_texture_destroy(queued);queued=NULL;
    }
    /* No consumer/readback to incidentally complete the upload: destruction
     * must retain the staging buffer and command pool until its fence signals. */
    for(unsigned i=0;i<32;i++) {
        CHECK(ch_vk_texture_upload_queued(gpu,device,queue,family,16,16,argb,1,0,&queued)==VK_SUCCESS);
        ch_vk_texture_destroy(queued);queued=NULL;
    }
    failed=0;
done:
    ch_vk_texture_destroy(sync);ch_vk_texture_destroy(queued);
#undef CHECK
    return failed;
}
int ch_test_render_textures(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    if(queued_uploads(gpu,device,queue,family) || snapshots(gpu,device,queue,family)) return 1;
    unsigned char *reference=malloc(sizeof memory),*candidate=malloc(sizeof memory);
    if(!reference || !candidate) {free(reference);free(candidate);return 1;}
    int failed=0;
    for(unsigned test=0;test<13 && !failed;test++) {
        unsigned control=0,fast=0;
        failed=scenario(gpu,device,queue,family,0,test,reference,&control) ||
            scenario(gpu,device,queue,family,1,test,candidate,&fast);
        if(!failed && (memcmp(reference,candidate,sizeof memory) ||
            (test<10 && ((test==3 || test==4 || test==6 || test==7)?!fast:test==8?fast>=control:fast!=0)))) {
            fprintf(stderr,"render texture integration mismatch case%u decoder counts %u/%u\n",test,control,fast);failed=1;
        }
        if(!failed && test>=10) {
            failed=scenario(gpu,device,queue,family,2,test,candidate,&fast);
            if(!failed && memcmp(reference,candidate,sizeof memory)) {
                fprintf(stderr,"fragment-depth backend storage mismatch case%u\n",test);failed=1;
            }
        }
    }
    free(reference);free(candidate);
    if(!failed) puts("PASS: strict-vs-GPU render-target sampling preserves exact guest memory for pitched/Morton targets, CPU edits, self sampling, resize, sampler changes, format/subview rejection and budget/pin fallback; eligible cases avoid CPU decoding");
    return failed;
}
