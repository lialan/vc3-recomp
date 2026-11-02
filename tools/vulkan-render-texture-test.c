/* Real Vulkan copies/cache/coherence, with an isolated scalar ARGB fixture
 * decoder. Headless entry point injects executor registration only. */
#include "vk_backend.h"
#include "vk_gpu_mailbox.h"
#include "vk_state.h"
#include "vk_pipeline_cache_file.h"
#include <unistd.h>
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
/* Sampler variants share the lookup bucket but remain separate images. */
void ch_test_presented_va(uint32_t);
static unsigned cpu_presents;
static VkResult count_cpu_present(void *context,const Nv2aSurface *s,const void *p,size_t bytes) {
    (void)context;(void)s;(void)p;(void)bytes;cpu_presents++;return VK_SUCCESS;
}
static int gpu_backend_present(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    ChVkBackend *backend=NULL;ChVkGpuMailbox *box=NULL;ChVkGpuFrame frame={0};int failed=1;
    unsigned char got[64];cpu_presents=0;
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"GPU backend present line%d: %s\n",__LINE__,#x);goto done;}}while(0)
    for(unsigned mode=0;mode<3;mode++) {
        memset(memory,0,sizeof memory);
        REQUIRE(ch_vk_gpu_mailbox_create(&box)==VK_SUCCESS);
        ChVkBackendConfig cfg={.physical=gpu,.device=device,.queue=queue,.family=family,
            .guest_base=memory,.guest_bytes=sizeof memory,.decode=decode,
            .resident_color=mode!=0,.gpu_color_pack=mode==2,.batch_draws=1,
            .render_context=box,.present_target=ch_vk_gpu_mailbox_publish,.present_rgba=count_cpu_present};
        REQUIRE(ch_vk_backend_create(&cfg,&backend)==VK_SUCCESS && ch_vk_backend_attach(backend)==VK_SUCCESS);
        const Nv2aBackend *cb=ch_vk_backend_callbacks();
        Nv2aSurface surface={.color_va=256,.width=4,.height=4,.pitch=24,.bytes_per_pixel=4};
        Nv2aRenderState state={.surface_format=0x100,.color_mask=0x01010101,.depth_max=1};
        ch_test_presented_va(surface.color_va);
        cb->clear(&surface,&state,0xf0,0x1749a1fe,0);if(cb->flush) cb->flush();
        /* CPU movie/idle-surface write after guest completion must reach the
         * GPU handoff, even though no further guest draw marks color dirty. */
        uint32_t edit=0x753917ab;memcpy(memory+256+24+8,&edit,4);
        cb->flip();
        REQUIRE(ch_vk_gpu_mailbox_take(box,&frame)==VK_SUCCESS);
        REQUIRE(ch_vk_texture_read_rgba(frame.image,got,sizeof got)==VK_SUCCESS);
        for(unsigned y=0;y<4;y++) for(unsigned x=0;x<4;x++) {
            uint32_t argb;memcpy(&argb,memory+256+y*24+x*4,4);
            unsigned char expected[]={argb>>16,argb>>8,argb,argb>>24};
            REQUIRE(!memcmp(got+(y*4+x)*4,expected,4));
        }
        REQUIRE(!cpu_presents);
        ch_vk_backend_destroy(backend);backend=NULL;
        REQUIRE(ch_vk_texture_read_rgba(frame.image,got,sizeof got)==VK_SUCCESS);
        ch_vk_gpu_frame_release(&frame);REQUIRE(ch_vk_gpu_mailbox_destroy(box)==VK_SUCCESS);box=NULL;
    }
    puts("PASS: GPU presentation bypasses CPU callbacks and includes idle CPU edits in strict/resident/packed paths");failed=0;
done:
    ch_test_presented_va(0);ch_vk_gpu_frame_release(&frame);ch_vk_backend_destroy(backend);ch_vk_gpu_mailbox_destroy(box);return failed;
#undef REQUIRE
}
static int gpu_mailbox(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    ChVkGpuMailbox *box=NULL;ChVkGpuFrame held[3]={{0}},next={0};ChVkTarget *source=NULL;
    unsigned char got[64],expected[3][64];int failed=1;const float black[4]={0};
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"GPU mailbox line%d: %s\n",__LINE__,#x);goto done;}}while(0)
    REQUIRE(ch_vk_gpu_mailbox_create(&box)==VK_SUCCESS);
    REQUIRE(ch_vk_target_create(gpu,device,queue,family,4,4,&source)==VK_SUCCESS);
    REQUIRE(ch_vk_target_clear_read(source,black,got,sizeof got)==VK_SUCCESS);
    Nv2aSurface surface={.width=4,.height=4,.pitch=16,.bytes_per_pixel=4};
    for(unsigned frame=0;frame<3;frame++) {
        for(unsigned i=0;i<64;i++) expected[frame][i]=(unsigned char)(i*13+frame*71);
        REQUIRE(ch_vk_target_import_color(source,expected[frame],64)==VK_SUCCESS);
        REQUIRE(ch_vk_gpu_mailbox_publish(box,&surface,source)==VK_SUCCESS);
        REQUIRE(ch_vk_gpu_mailbox_take(box,&held[frame])==VK_SUCCESS && held[frame].sequence==frame+1);
    }
    REQUIRE(ch_vk_gpu_mailbox_publish(box,&surface,source)==VK_NOT_READY);
    REQUIRE(ch_vk_gpu_mailbox_destroy(box)==VK_NOT_READY);
    for(unsigned i=0;i<3;i++) {
        REQUIRE(ch_vk_texture_read_rgba(held[i].image,got,sizeof got)==VK_SUCCESS);
        REQUIRE(!memcmp(got,expected[i],64));
    }
    /* Reuse only the released slot; queued replacement cannot touch the
     * other two frames held by consumers. */
    ChVkTexture *reused=held[1].image;ch_vk_gpu_frame_release(&held[1]);
    REQUIRE(ch_vk_gpu_mailbox_publish(box,&surface,source)==VK_SUCCESS);
    REQUIRE(ch_vk_gpu_mailbox_take(box,&next)==VK_SUCCESS && next.image==reused);
    REQUIRE(ch_vk_texture_read_rgba(next.image,got,sizeof got)==VK_SUCCESS && !memcmp(got,expected[2],64));
    ch_vk_gpu_frame_release(&next);ch_vk_gpu_frame_release(&held[2]);
    for(unsigned i=0;i<5;i++) REQUIRE(ch_vk_gpu_mailbox_publish(box,&surface,source)==VK_SUCCESS);
    REQUIRE(ch_vk_gpu_mailbox_take(box,&next)==VK_SUCCESS && next.sequence==9);
    ch_vk_target_destroy(source);source=NULL;
    REQUIRE(ch_vk_texture_read_rgba(held[0].image,got,sizeof got)==VK_SUCCESS && !memcmp(got,expected[0],64));
    REQUIRE(ch_vk_texture_read_rgba(next.image,got,sizeof got)==VK_SUCCESS && !memcmp(got,expected[2],64));
    ch_vk_gpu_frame_release(&next);
    /* Resize keeps old leased images alive. */
    REQUIRE(ch_vk_target_create(gpu,device,queue,family,2,2,&source)==VK_SUCCESS);
    REQUIRE(ch_vk_target_clear_read(source,black,got,sizeof got)==VK_SUCCESS);
    surface.width=surface.height=2;surface.pitch=8;
    REQUIRE(ch_vk_gpu_mailbox_publish(box,&surface,source)==VK_SUCCESS);
    REQUIRE(ch_vk_gpu_mailbox_take(box,&next)==VK_SUCCESS && next.surface.width==2);
    REQUIRE(ch_vk_texture_read_rgba(held[0].image,got,sizeof got)==VK_SUCCESS && !memcmp(got,expected[0],64));
    puts("PASS: GPU mailbox exact snapshots, bounded pins, dropped pending frames, reuse, source destruction and resize");failed=0;
done:
    ch_vk_gpu_frame_release(&next);for(unsigned i=0;i<3;i++) ch_vk_gpu_frame_release(&held[i]);
    ch_vk_target_destroy(source);ch_vk_gpu_mailbox_destroy(box);return failed;
#undef REQUIRE
}
static int sampler_collisions(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    ChVkTextureCache *cache=NULL;ChVkSampleImage a={0},b={0},probe={0};int failed=1;
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"sampler collision line%d: %s\n",__LINE__,#x);goto done;}}while(0)
    ChVkTextureCacheConfig cfg={.physical=gpu,.device=device,.queue=queue,.family=family,
        .guest_base=memory,.guest_bytes=sizeof memory,.decode=decode,.max_entries=2,.max_bytes=1024};
    REQUIRE(ch_vk_texture_cache_create(&cfg,&cache)==VK_SUCCESS);
    Nv2aTexture t={.width=1,.height=1,.pitch=4,.color=0x12,.levels=1,.filter=0x01010000,.addr_u=1,.addr_v=3};
    REQUIRE(ch_vk_texture_cache_resolve(cache,&t,&a)==VK_SUCCESS);
    t.addr_u=2;REQUIRE(ch_vk_texture_cache_resolve(cache,&t,&b)==VK_SUCCESS && b.gpu!=a.gpu);
    for(unsigned i=0;i<8;i++) {
        t.addr_u=(i&1)?2:1;
        REQUIRE(ch_vk_texture_cache_resolve(cache,&t,&probe)==VK_SUCCESS && probe.gpu==((i&1)?b.gpu:a.gpu));
        release_image(&probe);
    }
    release_image(&a);t.addr_u=3;
    REQUIRE(ch_vk_texture_cache_resolve(cache,&t,&probe)==VK_SUCCESS);release_image(&probe);
    t.addr_u=2;REQUIRE(ch_vk_texture_cache_resolve(cache,&t,&probe)==VK_SUCCESS && probe.gpu==b.gpu);release_image(&probe);
    t.addr_u=1;REQUIRE(ch_vk_texture_cache_resolve(cache,&t,&probe)==VK_SUCCESS);release_image(&probe);
    t.addr_u=2;REQUIRE(ch_vk_texture_cache_resolve(cache,&t,&probe)==VK_SUCCESS && probe.gpu==b.gpu);
    puts("PASS: colliding sampler identities preserve pinned hits across eviction and slot reuse");failed=0;
done:
    release_image(&probe);release_image(&a);release_image(&b);ch_vk_texture_cache_destroy(cache);return failed;
#undef REQUIRE
}
static int cold_eviction(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    ChVkTextureCache *cache=NULL;ChVkSampleImage image={0},pinned={0};int failed=1;
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"cold eviction line%d: %s\n",__LINE__,#x);goto done;}}while(0)
    for(unsigned mode=0;mode<4;mode++) {
        // Four tiny old entries and one newer large entry fill 240 bytes:
        // guest snapshot + one mip image + transfer staging, no GPU decode.
        ChVkTextureCacheConfig cfg={.physical=gpu,.device=device,.queue=queue,.family=family,
            .guest_base=memory,.guest_bytes=sizeof memory,.decode=decode,.max_entries=8,.max_bytes=240,
            .size_aware_eviction=mode!=0};
        REQUIRE(ch_vk_texture_cache_create(&cfg,&cache)==VK_SUCCESS);
        uint32_t red=0xffff0000;for(unsigned i=0;i<1024;i+=4)memcpy(memory+i,&red,4);
        Nv2aTexture t={.width=1,.height=1,.pitch=4,.color=0x12,.levels=1,.filter=0x01010000,.addr_u=3,.addr_v=3};
        for(unsigned i=0;i<4;i++) {t.offset=i*4;REQUIRE(ch_vk_texture_cache_resolve(cache,&t,&image)==VK_SUCCESS);release_image(&image);}
        t.offset=256;t.width=t.height=4;t.pitch=16;
        REQUIRE(ch_vk_texture_cache_resolve(cache,&t,&image)==VK_SUCCESS);
        if(mode==3) {pinned=image;memset(&image,0,sizeof image);}else release_image(&image);
        ChVkTextureCacheStats stats;ch_vk_texture_cache_stats(cache,&stats);REQUIRE(stats.entries==5 && stats.bytes==240);
        ch_vk_texture_cache_advance_frame(cache);
        if(mode!=2)ch_vk_texture_cache_advance_frame(cache);
        ch_vk_texture_cache_stats(cache,&stats);REQUIRE(stats.evictions==((mode==1 || mode==3)?1:0));
        t.offset=512;
        if(mode==3) {
            REQUIRE(ch_vk_texture_cache_resolve(cache,&t,&image)==VK_NOT_READY && !image.gpu);
            unsigned char retained[64];REQUIRE(ch_vk_texture_read_rgba(pinned.gpu,retained,sizeof retained)==VK_SUCCESS);
            for(unsigned i=0;i<16;i++)REQUIRE(retained[4*i]==255 && retained[4*i+3]==255);
            release_image(&pinned);ch_vk_texture_cache_advance_frame(cache);
        }
        REQUIRE(ch_vk_texture_cache_resolve(cache,&t,&image)==VK_SUCCESS);
        unsigned char rgba[64];REQUIRE(ch_vk_texture_read_rgba(image.gpu,rgba,sizeof rgba)==VK_SUCCESS);
        for(unsigned i=0;i<16;i++) REQUIRE(rgba[4*i]==255 && !rgba[4*i+1] && !rgba[4*i+2] && rgba[4*i+3]==255);
        release_image(&image);ch_vk_texture_cache_stats(cache,&stats);
        REQUIRE(stats.evictions==(mode==1?1:5) && stats.bytes<=240);
        if(mode==1) {
            uint64_t hits=stats.hits;t.offset=0;t.width=t.height=1;t.pitch=4;
            REQUIRE(ch_vk_texture_cache_resolve(cache,&t,&image)==VK_SUCCESS);release_image(&image);
            ch_vk_texture_cache_stats(cache,&stats);REQUIRE(stats.hits==hits+1);
        }
        ch_vk_texture_cache_destroy(cache);cache=NULL;
    }
    puts("PASS: amortized byte-budget eviction selects one fitting cold image, retains tiny snapshots, preserves frame grace and LRU control");failed=0;
done:
    release_image(&image);release_image(&pinned);ch_vk_texture_cache_destroy(cache);return failed;
#undef REQUIRE
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
    REQUIRE(ch_vk_target_batching(source,1)==VK_SUCCESS);
    ch_vk_target_defer_color_readback(source,1);
    for(unsigned epoch=0;epoch<2;epoch++) {
        ChVkColorVertex vertices[]={{{-1,-1,0,1},{1,0,0,1}},{{3,-1,0,1},{1,0,0,1}},{{-1,3,0,1},{1,0,0,1}}};
        const uint16_t tri[]={0,1,2};Nv2aRenderState color={.color_mask=0x01010101};
        for(unsigned i=0;i<3;i++) {vertices[i].rgba[0]=epoch?0:1;vertices[i].rgba[2]=epoch?1:0;}
        REQUIRE(ch_vk_target_draw_state_read(source,NULL,vertices,3,tri,3,&color,got,sizeof got)==VK_SUCCESS);
        REQUIRE(ch_vk_target_pending(source));
        /* Idempotent state updates must preserve a pending draw, allowing the
         * following snapshot copy to share its one submission. */
        ChVkTargetResourceStats state_before,state_after;
        ch_vk_target_resource_stats(source,&state_before);
        for(unsigned repeat=0;repeat<3;repeat++) {
            REQUIRE(ch_vk_target_depth_quantization(source,0)==VK_SUCCESS);
            REQUIRE(ch_vk_target_color_layout(source,0)==VK_SUCCESS);
            REQUIRE(ch_vk_target_pending(source));
        }
        ch_vk_target_resource_stats(source,&state_after);
        REQUIRE(state_after.batch_submissions==state_before.batch_submissions);
        REQUIRE(ch_vk_target_refresh_texture(source,NULL)==VK_ERROR_INITIALIZATION_FAILED && ch_vk_target_pending(source));
        if(!epoch) {
            ch_vk_texture_destroy(copy);copy=NULL;
            REQUIRE(ch_vk_target_snapshot_texture(source,5,&copy)==VK_SUCCESS && !ch_vk_target_pending(source));
        } else REQUIRE(ch_vk_target_refresh_texture(source,copy)==VK_SUCCESS && !ch_vk_target_pending(source));
        ChVkTargetResourceStats stats;ch_vk_target_resource_stats(source,&stats);
        REQUIRE(stats.combined_texture_copies==epoch+1);
        REQUIRE(stats.combined_new_texture_copies==1);
        REQUIRE(stats.batch_submissions==state_before.batch_submissions+1);
        for(unsigned level=0;level<5;level++) {
            unsigned bytes=(16u>>level)*(16u>>level)*4;
            REQUIRE(ch_vk_texture_read_level_rgba(copy,level,got,sizeof got)==VK_SUCCESS);
            for(unsigned n=0;n<bytes;n++) REQUIRE(got[n]==((n%4==3 || n%4==(epoch?2:0))?255:0));
        }
    }
    REQUIRE(ch_vk_target_import_color(source,rgba,sizeof rgba)==VK_SUCCESS);
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
    unsigned fast,unsigned test,unsigned char *result,unsigned *decode_count,VkPipelineCache pipeline_cache) {
    ChVkBackend *backend=NULL;int failed=1;memset(memory,0xa5,sizeof memory);decodes=0;
    ChVkBackendConfig config={.physical=gpu,.device=device,.queue=queue,.family=family,
        .pipeline_cache=pipeline_cache,.guest_base=memory,.guest_bytes=sizeof memory,.decode=decode,.resident_color=1,.cache_textures=1,
        .sample_render_targets=fast,.multiple_depth=test>=10 && fast,.resident_depth=test>=10 && fast,.quantized_depth=test>=10 && fast,.gpu_depth_pack=test>=10 && fast,.gpu_color_pack=fast,.gpu_morton_color=fast,.depth_quant_inline=test>=10 && fast==1,.depth_quant_fragment=test>=10 && fast==2,.batch_draws=1,.texture_cache_entries=2,
        .texture_cache_bytes=test==7?64:test==8?256:8192};
#define REQUIRE(x) do {if(!(x)) {fprintf(stderr,"render texture integration case%u fast%u line%d: %s\n",test,fast,__LINE__,#x);goto done;}}while(0)
    config.batch_draw_limit=257;
    REQUIRE(ch_vk_backend_create(&config,&backend)==VK_ERROR_INITIALIZATION_FAILED && !backend);
    config.batch_draw_limit=test>=15?64:test%3==0?1:test%3==1?64:0;
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
    if(test>=15) {dst_state.depth_test_enable=dst_state.depth_write=0;dst_state.zeta_va=0;}
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
    if(test>=13 && test!=15) {uint32_t edit=0x7349a1fe;memcpy(memory+dest.color_va+4,&edit,4);}
    if(test==14) {uint32_t edit=0x40000039;memcpy(memory+8192,&edit,4);}
    if(test>=15) cb->draw(&source,&draw_source); /* Independent pending depth owner. */
    if(test>=17) {
        uint32_t edit=0x27395bef;memcpy(memory+source.color_va+4,&edit,4);
        if(test==18) {src_state.clear_rect_set=3;src_state.clear_rect_h=1u<<16;src_state.clear_rect_v=1u<<16;}
        if(test==19) {src_state.clear_rect_set=3;src_state.clear_rect_h=3u<<16;src_state.clear_rect_v=3u<<16;}
        cb->clear(&source,&src_state,0xf0,0x98654321,0);
        src_state.clear_rect_set=0;
        edit=0x1234abef;memcpy(memory+source.color_va+8,&edit,4);
        cb->draw(&dest,&draw_dest); /* CPU edit after clear still must merge. */
    }
    cb->flush();
    if(test>=17) {
        const unsigned char cleared[]={0x21,0x43,0x65,0x98},edited[]={0xef,0xab,0x34,0x12};
        REQUIRE(!memcmp(memory+source.color_va,cleared,4));
        REQUIRE(!memcmp(memory+source.color_va+4,cleared,4));
        REQUIRE(!memcmp(memory+source.color_va+8,edited,4));
    }
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
        VkDescriptorImageInfo descriptor=ch_vk_texture_descriptor(queued);
        CHECK(ch_vk_texture_replace_argb(queued,argb,1,1)==VK_ERROR_INITIALIZATION_FAILED);
        for(unsigned pass=0;pass<2;pass++) {
            for(unsigned i=0;i<sizeof argb/sizeof *argb;i++) argb[i]^=0x00f01234u+pass;
            CHECK(ch_vk_texture_replace_argb(queued,argb,sizeof argb/sizeof *argb,1)==VK_SUCCESS);
        }
        CHECK(ch_vk_texture_replace_argb(sync,argb,sizeof argb/sizeof *argb,0)==VK_SUCCESS);
        VkDescriptorImageInfo reused=ch_vk_texture_descriptor(queued);
        CHECK(reused.imageView==descriptor.imageView && reused.sampler==descriptor.sampler);
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
static int queued_depth_copies(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    ChVkTarget *a=NULL,*b=NULL;ChVkTexture *snapshot=NULL;int failed=1;
    const float black[]={0,0,0,1};unsigned char rgba[64];float depth[16];uint8_t stencil[16];
    const uint16_t indices[]={0,1,2};
    ChVkColorVertex vertices[]={{{-1,-1,.25f,1},{1,0,0,1}},{{3,-1,.25f,1},{1,0,0,1}},{{-1,3,.25f,1},{1,0,0,1}}};
    Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=1,.depth_write=1,.depth_func=0x207};
#define CHECK(x) do {if(!(x)){fprintf(stderr,"queued depth copy line%d case%u: %s\n",__LINE__,test,#x);goto done;}}while(0)
    for(unsigned test=0;test<13;test++) {
        CHECK(ch_vk_target_create(gpu,device,queue,family,4,4,&a)==VK_SUCCESS);
        CHECK(ch_vk_target_create(gpu,device,queue,family,4,4,&b)==VK_SUCCESS);
        CHECK(ch_vk_target_batching(a,1)==VK_SUCCESS && ch_vk_target_batching(b,1)==VK_SUCCESS);
        CHECK(ch_vk_target_clear_aspects_read(a,7,black,1,7,rgba,sizeof rgba)==VK_SUCCESS);
        CHECK(ch_vk_target_clear_aspects_read(b,7,black,1,3,rgba,sizeof rgba)==VK_SUCCESS);
        ch_vk_target_defer_color_readback(a,1);ch_vk_target_defer_color_readback(b,1);
        for(unsigned i=0;i<3;i++) vertices[i].xy[2]=.25f;
        CHECK(ch_vk_target_draw_state_read(a,NULL,vertices,3,indices,3,&state,rgba,sizeof rgba)==VK_SUCCESS);
        if(test>=9) {
            for(unsigned i=0;i<3;i++) vertices[i].xy[2]=.75f;
            CHECK(ch_vk_target_draw_state_read(b,NULL,vertices,3,indices,3,&state,rgba,sizeof rgba)==VK_SUCCESS);
            for(unsigned i=0;i<3;i++) vertices[i].xy[2]=.25f;
        }
        CHECK(ch_vk_target_copy_depth_stencil_queued(a,b)==VK_SUCCESS);
        ChVkTargetResourceStats stats;ch_vk_target_resource_stats(a,&stats);
        const char *option=getenv("CHIHIRO_VULKAN_ASYNC_DEPTH_COPIES");
        CHECK(stats.queued_depth_copies==(!option || atoi(option)?1u:0u));
        if(stats.queued_depth_copies) CHECK(ch_vk_target_completion_fence(b)==VK_NULL_HANDLE);
        if(test==1) CHECK(ch_vk_target_read_color(a,rgba,sizeof rgba)==VK_SUCCESS && rgba[0]==255);
        if(test==2 || test==5 || test==10) {
            for(unsigned i=0;i<3;i++) vertices[i].xy[2]=.5f;
            CHECK(ch_vk_target_draw_state_read(test==2?a:b,NULL,vertices,3,indices,3,&state,rgba,sizeof rgba)==VK_SUCCESS);
        }
        if(test==3 || test==11) {ch_vk_target_destroy(a);a=NULL;}
        if(test==4 || test==12) {ch_vk_target_destroy(b);b=NULL;}
        if(test==6) CHECK(ch_vk_target_copy_depth_stencil_queued(b,a)==VK_SUCCESS);
        if(test==7) {
            CHECK(ch_vk_target_snapshot_texture(a,1,&snapshot)==VK_SUCCESS);
            CHECK(ch_vk_texture_read_rgba(snapshot,rgba,sizeof rgba)==VK_SUCCESS && rgba[0]==255);
            ch_vk_texture_destroy(snapshot);snapshot=NULL;
        }
        if(test==8) {
            CHECK(ch_vk_target_read_depth_begin(a,depth,stencil,16,0,NULL,0)==VK_SUCCESS);
            CHECK(ch_vk_target_read_depth_end(a)==VK_SUCCESS && depth[0]==.25f);
        }
        if(b) {
            CHECK(ch_vk_target_read_depth_stencil(b,depth,stencil,16)==VK_SUCCESS);
            for(unsigned i=0;i<16;i++) CHECK(depth[i]==(test==5 || test==10?.5f:.25f) && stencil[i]==7);
        }
        if(a) {
            CHECK(ch_vk_target_read_depth_stencil(a,depth,stencil,16)==VK_SUCCESS);
            for(unsigned i=0;i<16;i++) CHECK(depth[i]==(test==2?.5f:.25f) && stencil[i]==7);
        }
        ch_vk_target_destroy(a);a=NULL;ch_vk_target_destroy(b);b=NULL;
    }
    failed=0;puts("PASS: queued depth copy preserves source/destination readbacks, redraws, reverse copies, snapshotting and either destruction order");
done:
    ch_vk_texture_destroy(snapshot);ch_vk_target_destroy(a);ch_vk_target_destroy(b);return failed;
#undef CHECK
}
static int paired_readbacks(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    ChVkTarget *t=NULL;int failed=1;
    unsigned char rgba[64],reference[64],stencil[16],stencil_ref[16];float depth[16],depth_ref[16];uint32_t words[16],words_ref[16];
    const float black[]={0,0,0,1};const uint16_t idx[]={0,1,2};
    ChVkColorVertex v[]={{{-1,-1,.25f,1},{1,0,0,1}},{{3,-1,.25f,1},{1,0,0,1}},{{-1,3,.25f,1},{1,0,0,1}}};
    Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=1,.depth_write=1,.depth_func=0x207};
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"paired readback failure line%d: %s\n",__LINE__,#x);goto done;}}while(0)
    for(unsigned packed=0;packed<2;packed++) {
        VkResult r=ch_vk_target_create_depth(gpu,device,queue,family,4,4,
            packed?VK_FORMAT_D24_UNORM_S8_UINT:VK_FORMAT_D32_SFLOAT_S8_UINT,&t);
        if(packed && r==VK_ERROR_FORMAT_NOT_SUPPORTED) continue;
        CHECK(r==VK_SUCCESS);
        CHECK(ch_vk_target_batching(t,1)==VK_SUCCESS);
        /* Compare predicted non-writes with actual rasterization. Keep draws
         * executing: this predicate only controls guest publication. */
        for(unsigned test=0;test<7;test++) {
            Nv2aRenderState probe=state;
            probe.stencil_enable=1;probe.stencil_func=0x207;
            probe.stencil_func_mask=255;probe.stencil_write_mask=255;probe.stencil_ref=0x71;
            probe.stencil_fail=probe.stencil_zfail=probe.stencil_zpass=0x1e00;
            probe.depth_write=0;
            unsigned expected=1;
            if(test==0) {probe.stencil_enable=0;probe.depth_func=0x200;expected=0;}
            if(test==2) {probe.stencil_func=0x200;probe.stencil_fail=0x1e01;probe.depth_write=1;expected=2;}
            if(test==3) {probe.depth_func=0x200;probe.stencil_zfail=0x1e01;expected=2;}
            if(test==4) {probe.depth_test_enable=0;probe.stencil_zfail=0x1e01;}
            if(test==5) {probe.alpha_test_enable=1;probe.alpha_func=0x200;probe.stencil_func=0x200;probe.stencil_fail=0x1e01;expected=0;}
            if(test==6) {probe.stencil_fail=probe.stencil_zfail=0x1e01;}
            CHECK(ch_vk_attachment_write_mask(&probe)==expected);
            CHECK(ch_vk_target_clear_aspects_read(t,7,black,1,7,reference,sizeof reference)==VK_SUCCESS);
            CHECK(ch_vk_target_read_z24s8(t,words_ref,16)==VK_SUCCESS);
            CHECK(ch_vk_target_draw_state_read(t,NULL,v,3,idx,3,&probe,rgba,sizeof rgba)==VK_SUCCESS);
            CHECK(ch_vk_target_read_color(t,rgba,sizeof rgba)==VK_SUCCESS);
            CHECK(ch_vk_target_read_z24s8(t,words,16)==VK_SUCCESS);
            CHECK(!!memcmp(reference,rgba,sizeof rgba)==!!(expected&1));
            CHECK(!!memcmp(words_ref,words,sizeof words)==!!(expected&2));
            if(test==2 || test==3) for(unsigned j=0;j<16;j++) CHECK((words[j]&255)==0x71);
        }
        /* Irrelevant state changes must reuse a pipeline, while the raster
         * output remains a red color-only draw and untouched depth/stencil. */
        Nv2aRenderState equivalent={.color_mask=0x01010101,.stencil_enable=1,
            .stencil_func=0x207,.stencil_fail=0x1e01,.stencil_zfail=0x1e01,.stencil_zpass=0x1e00};
        CHECK(ch_vk_target_clear_aspects_read(t,7,black,1,7,reference,sizeof reference)==VK_SUCCESS);
        CHECK(ch_vk_target_read_z24s8(t,words_ref,16)==VK_SUCCESS);
        CHECK(ch_vk_target_draw_state_read(t,NULL,v,3,idx,3,&equivalent,rgba,sizeof rgba)==VK_SUCCESS);
        ChVkTargetResourceStats canonical,changed;ch_vk_target_resource_stats(t,&canonical);
        for(unsigned j=0;j<32;j++) {
            equivalent.depth_write=j&1;equivalent.stencil_ref=j;
            equivalent.stencil_write_mask=j;equivalent.stencil_func_mask=j*7;
            CHECK(ch_vk_target_draw_state_read(t,NULL,v,3,idx,3,&equivalent,rgba,sizeof rgba)==VK_SUCCESS);
        }
        ch_vk_target_resource_stats(t,&changed);
        CHECK(changed.pipeline_builds==canonical.pipeline_builds && changed.pipeline_hits==canonical.pipeline_hits+32);
        CHECK(ch_vk_target_read_color(t,rgba,sizeof rgba)==VK_SUCCESS);
        for(unsigned j=0;j<16;j++) CHECK(rgba[4*j]==255 && rgba[4*j+1]==0 && rgba[4*j+2]==0 && rgba[4*j+3]==255);
        CHECK(ch_vk_target_read_z24s8(t,words,16)==VK_SUCCESS && !memcmp(words,words_ref,sizeof words));
        /* Reference changes in one pending batch must reuse the pipeline and
         * retain per-draw values, including after command-buffer retirement. */
        equivalent.stencil_write_mask=255;equivalent.stencil_func_mask=255;
        equivalent.stencil_zpass=0x1e01;equivalent.stencil_ref=0;
        ch_vk_target_defer_color_readback(t,1);
        CHECK(ch_vk_target_draw_state_read(t,NULL,v,3,idx,3,&equivalent,rgba,sizeof rgba)==VK_SUCCESS);
        ch_vk_target_resource_stats(t,&canonical);
        for(unsigned j=1;j<=32;j++) {
            equivalent.stencil_ref=j;
            CHECK(ch_vk_target_draw_state_read(t,NULL,v,3,idx,3,&equivalent,rgba,sizeof rgba)==VK_SUCCESS);
        }
        ch_vk_target_resource_stats(t,&changed);
        CHECK(changed.pipeline_builds==canonical.pipeline_builds && changed.pipeline_hits==canonical.pipeline_hits+32);
        CHECK(ch_vk_target_read_z24s8(t,words,16)==VK_SUCCESS);
        for(unsigned j=0;j<16;j++) CHECK((words[j]&255)==32);
        equivalent.stencil_zpass=0x1e00;equivalent.stencil_func=0x202;
        for(unsigned j=0;j<2;j++) {
            CHECK(ch_vk_target_clear_aspects_read(t,7,black,1,7,rgba,sizeof rgba)==VK_SUCCESS);
            equivalent.stencil_ref=j?8:7;
            CHECK(ch_vk_target_draw_state_read(t,NULL,v,3,idx,3,&equivalent,rgba,sizeof rgba)==VK_SUCCESS);
            CHECK(ch_vk_target_read_color(t,rgba,sizeof rgba)==VK_SUCCESS);
            for(unsigned k=0;k<16;k++) CHECK(rgba[k*4]==(j?0:255));
        }
        ch_vk_target_defer_color_readback(t,0);
        CHECK(ch_vk_target_clear_aspects_read(t,7,black,1,7,rgba,sizeof rgba)==VK_SUCCESS);
        ch_vk_target_defer_color_readback(t,1);
        CHECK(ch_vk_target_draw_state_read(t,NULL,v,3,idx,3,&state,rgba,sizeof rgba)==VK_SUCCESS);
        CHECK(ch_vk_target_pending(t)==1);
        CHECK(ch_vk_target_read_depth_color(t,packed?(void *)words:(void *)depth,stencil,16,packed,rgba,63)==VK_ERROR_INITIALIZATION_FAILED);
        CHECK(ch_vk_target_pending(t)==1);
        ChVkTargetResourceStats before,after;ch_vk_target_resource_stats(t,&before);
        CHECK(ch_vk_target_read_depth_color(t,packed?(void *)words:(void *)depth,stencil,16,packed,rgba,sizeof rgba)==VK_SUCCESS);
        ch_vk_target_resource_stats(t,&after);
        CHECK(after.batch_submissions==before.batch_submissions+1 && after.combined_depth_readbacks==before.combined_depth_readbacks+1 && after.combined_color_readbacks==before.combined_color_readbacks+1);
        CHECK(ch_vk_target_read_color(t,reference,sizeof reference)==VK_SUCCESS && !memcmp(reference,rgba,sizeof rgba));
        if(packed) {CHECK(ch_vk_target_read_z24s8(t,words_ref,16)==VK_SUCCESS);CHECK(!memcmp(words,words_ref,sizeof words));}
        else {CHECK(ch_vk_target_read_depth_stencil(t,depth_ref,stencil_ref,16)==VK_SUCCESS);CHECK(!memcmp(depth,depth_ref,sizeof depth) && !memcmp(stencil,stencil_ref,sizeof stencil));}
        /* Exercise the no-pending path and staging reuse too. */
        CHECK(ch_vk_target_read_depth_color(t,packed?(void *)words:(void *)depth,stencil,16,packed,rgba,sizeof rgba)==VK_SUCCESS);
        CHECK(!memcmp(reference,rgba,sizeof rgba));
        for(int layout=0;layout<3;layout++) {
            CHECK(ch_vk_target_color_layout(t,layout)==VK_SUCCESS);
            CHECK(ch_vk_target_draw_state_read(t,NULL,v,3,idx,3,&state,rgba,sizeof rgba)==VK_SUCCESS);
            memset(rgba,0x55,sizeof rgba);
            CHECK(ch_vk_target_read_color_begin(t,rgba,sizeof rgba)==VK_SUCCESS);
            for(unsigned j=0;j<sizeof rgba;j++) CHECK(rgba[j]==0x55);
            CHECK(ch_vk_target_read_color_begin(t,rgba,sizeof rgba)==VK_ERROR_INITIALIZATION_FAILED);
            CHECK(ch_vk_target_read_depth_begin(t,words,NULL,16,1,NULL,0)==VK_ERROR_INITIALIZATION_FAILED);
            CHECK(ch_vk_target_read_color_end(t)==VK_SUCCESS);
            CHECK(ch_vk_target_read_color_end(t)==VK_ERROR_INITIALIZATION_FAILED);
            CHECK(ch_vk_target_read_color(t,reference,sizeof reference)==VK_SUCCESS);
            CHECK(!memcmp(reference,rgba,sizeof rgba));
            CHECK(ch_vk_target_read_color_begin(t,rgba,sizeof rgba)==VK_SUCCESS);
            CHECK(ch_vk_target_read_color_end(t)==VK_SUCCESS);
        }
        CHECK(ch_vk_target_draw_state_read(t,NULL,v,3,idx,3,&state,rgba,sizeof rgba)==VK_SUCCESS);
        memset(rgba,0x55,sizeof rgba);
        CHECK(ch_vk_target_read_color_begin(t,rgba,sizeof rgba)==VK_SUCCESS);
        ch_vk_target_destroy(t);t=NULL;
        CHECK(!memcmp(reference,rgba,sizeof rgba));
    }
    failed=0;
done:
    ch_vk_target_destroy(t);
#undef CHECK
    return failed;
}
static int cache_roundtrip(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    VkPipelineCache cold=VK_NULL_HANDLE,warm=VK_NULL_HANDLE,rejected=VK_NULL_HANDLE;
    unsigned char reference[sizeof memory],candidate[sizeof memory];
    unsigned char *saved=NULL,*edit=NULL;FILE *file=NULL;int failed=1;size_t loaded=0,size=0;
    char directory[4096]={0},path[4096]={0};unsigned count=0;
    const char *temporary=getenv("TMPDIR");
#if defined(__ANDROID__)
    if(!temporary) temporary="/data/local/tmp";
#else
    if(!temporary) temporary="/tmp";
#endif
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"pipeline cache failure line%d: %s\n",__LINE__,#x);goto done;}}while(0)
    CHECK(snprintf(directory,sizeof directory,"%s/ch-vk-cache-XXXXXX",temporary)<(int)sizeof directory);
    CHECK(mkdtemp(directory));CHECK(snprintf(path,sizeof path,"%s/cache",directory)<(int)sizeof path);
    CHECK(ch_vk_pipeline_cache_file_load(gpu,device,path,&cold,&loaded)==VK_SUCCESS && !loaded);
    CHECK(!scenario(gpu,device,queue,family,2,14,reference,&count,cold));
    /* Backend destroyed its targets but must not destroy the borrowed cache. */
    CHECK(vkGetPipelineCacheData(device,cold,&size,NULL)==VK_SUCCESS && size>=32);
    saved=malloc(size);CHECK(saved);
    CHECK(vkGetPipelineCacheData(device,cold,&size,saved)==VK_SUCCESS);
    free(saved);saved=NULL;
    if(size<sizeof(VkPipelineCacheHeaderVersionOne)) {
        CHECK(!ch_vk_pipeline_cache_file_save(device,cold,path));
        CHECK(access(path,F_OK)!=0);
        puts("PASS: driver exports no complete cache header; persistence skipped without creating a file");
        failed=0;goto done;
    }
    CHECK(ch_vk_pipeline_cache_file_save(device,cold,path));
    CHECK(ch_vk_pipeline_cache_file_load(gpu,device,path,&warm,&loaded)==VK_SUCCESS && loaded>=32);
    CHECK(!scenario(gpu,device,queue,family,2,14,candidate,&count,warm));
    CHECK(!memcmp(reference,candidate,sizeof reference));
    CHECK(!ch_vk_pipeline_cache_file_save(device,VK_NULL_HANDLE,path));
    file=fopen(path,"rb");CHECK(file && !fseek(file,0,SEEK_END));
    long length=ftell(file);CHECK(length>=56 && !fseek(file,0,SEEK_SET));size=(size_t)length;
    saved=malloc(size+1);edit=malloc(size+1);CHECK(saved && edit);
    CHECK(fread(saved,1,size,file)==size);CHECK(!fclose(file));file=NULL;
    CHECK(ch_vk_pipeline_cache_memory_load(gpu,device,saved,size,&rejected,&loaded)==VK_SUCCESS && loaded>=32);
    CHECK(vkMergePipelineCaches(device,warm,1,&rejected)==VK_SUCCESS);
    vkDestroyPipelineCache(device,rejected,NULL);rejected=VK_NULL_HANDLE;
    for(unsigned test=0;test<11;test++) {
        memcpy(edit,saved,size);size_t bytes=size;
        if(test==0) edit[0]^=1; /* envelope version/magic */
        if(test==1) edit[size-1]^=1; /* corrupt opaque driver bytes */
        if(test==2) bytes=12; /* truncated envelope */
        if(test==3) bytes=size-1; /* truncated driver data */
        if(test==4) {edit[size]=0;bytes++;} /* unexpected trailer */
        if(test==5) memset(edit+8,255,8); /* oversized length */
        if(test>=6) {
            const unsigned field[]={16,8,12,0,4};
            edit[24+field[test-6]]^=1; /* UUID/vendor/device/header size/version; valid envelope hash */
            uint64_t h=UINT64_C(14695981039346656037);
            for(size_t i=24;i<size;i++) h=(h^edit[i])*UINT64_C(1099511628211);
            for(unsigned i=0;i<8;i++) edit[16+i]=(unsigned char)(h>>(8*i));
        }
        file=fopen(path,"wb");CHECK(file && fwrite(edit,1,bytes,file)==bytes);
        CHECK(!fclose(file));file=NULL;
        loaded=123;
        CHECK(ch_vk_pipeline_cache_file_load(gpu,device,path,&rejected,&loaded)==VK_SUCCESS && !loaded);
        vkDestroyPipelineCache(device,rejected,NULL);rejected=VK_NULL_HANDLE;
        CHECK(ch_vk_pipeline_cache_memory_load(gpu,device,edit,bytes,&rejected,&loaded)==VK_SUCCESS && !loaded);
        vkDestroyPipelineCache(device,rejected,NULL);rejected=VK_NULL_HANDLE;
    }
    CHECK(!ch_vk_pipeline_cache_file_save(device,cold,directory)); /* atomic rename fails safely */
    failed=0;puts("PASS: borrowed pipeline cache survives backend destruction; persisted cache preserves exact guest memory; corrupt/truncated/oversized/incompatible data falls back to empty cache");
done:
    if(file) fclose(file);
    free(saved);free(edit);if(*path) unlink(path);rmdir(directory);
    if(cold) vkDestroyPipelineCache(device,cold,NULL);
    if(warm) vkDestroyPipelineCache(device,warm,NULL);
    if(rejected) vkDestroyPipelineCache(device,rejected,NULL);
    return failed;
#undef CHECK
}
int ch_test_texture_watch(VkPhysicalDevice,VkDevice,VkQueue,uint32_t);
int ch_test_render_textures(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    if(gpu_mailbox(gpu,device,queue,family) || gpu_backend_present(gpu,device,queue,family)) return 1;
    if(ch_test_texture_watch(gpu,device,queue,family)) return 1;
    if(sampler_collisions(gpu,device,queue,family)) return 1;
    if(cold_eviction(gpu,device,queue,family)) return 1;
    if(queued_depth_copies(gpu,device,queue,family) || cache_roundtrip(gpu,device,queue,family) || paired_readbacks(gpu,device,queue,family) || queued_uploads(gpu,device,queue,family) || snapshots(gpu,device,queue,family)) return 1;
    unsigned char *reference=malloc(sizeof memory),*candidate=malloc(sizeof memory);
    if(!reference || !candidate) {free(reference);free(candidate);return 1;}
    int failed=0;
    for(unsigned test=0;test<20 && !failed;test++) {
        unsigned control=0,fast=0;
        failed=scenario(gpu,device,queue,family,0,test,reference,&control,VK_NULL_HANDLE) ||
            scenario(gpu,device,queue,family,1,test,candidate,&fast,VK_NULL_HANDLE);
        if(!failed && (memcmp(reference,candidate,sizeof memory) ||
            (test<10 && ((test==3 || test==4 || test==6 || test==7)?!fast:test==8?fast>=control:fast!=0)))) {
            fprintf(stderr,"render texture integration mismatch case%u decoder counts %u/%u\n",test,control,fast);failed=1;
        }
        if(!failed && test>=10) {
            failed=scenario(gpu,device,queue,family,2,test,candidate,&fast,VK_NULL_HANDLE);
            if(!failed && memcmp(reference,candidate,sizeof memory)) {
                fprintf(stderr,"fragment-depth backend storage mismatch case%u\n",test);failed=1;
            }
        }
    }
    free(reference);free(candidate);
    if(!failed) puts("PASS: strict-vs-GPU render-target sampling preserves exact guest memory for pitched/Morton targets, CPU edits, self sampling, resize, sampler changes, format/subview rejection and budget/pin fallback; eligible cases avoid CPU decoding");
    return failed;
}
