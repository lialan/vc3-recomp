#include "vk_texture_cache.h"
#include "../nv2a_texture_cube.h"
#include <stdio.h>
#include <string.h>
static void unpin(ChVkSampleImage *image) {
    if(image->gpu && image->release) image->release(image->release_context,image->gpu);
    memset(image,0,sizeof *image);
}
static VkResult single_plane(void *context,const Nv2aTexture *tx,ChVkSampleImage *out) {
    (void)context;(void)tx;static const uint32_t white=0xffffffff;
    *out=(ChVkSampleImage){.width=1,.height=1,.argb=&white};return VK_SUCCESS;
}
int ch_test_cube_cache(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family,
    void *guest,size_t bytes,ChVkDecodeTexture decode) {
    enum {S=16};
    ChVkTextureCache *cache=NULL;ChVkTarget *target=NULL;ChVkSampleImage a={0},b={0},c={0};int failed=0;
    Nv2aTexture tx={.offset=256,.width=S,.height=S,.color=0x0f,.levels=3,.cube=1,
        .filter=0x02022000,.addr_u=1,.addr_v=1};
    unsigned char *ram=guest,rgba[6*S*S*4],pixels[4*4*4];
    ChVkTextureCacheConfig config={.physical=gpu,.device=device,.queue=queue,.family=family,
        .guest_base=guest,.guest_bytes=bytes,.decode=decode,.max_entries=2,.max_bytes=32768};
#define REQUIRE(x) do {if(!(x)) {fprintf(stderr,"cube cache failure line %d: %s\n",__LINE__,#x);failed=1;goto done;}}while(0)
    size_t stride=ch_nv2a_cube_face_bytes(&tx);REQUIRE(stride==384 && bytes>=256+stride*6);
    const uint16_t colors[]={0xf800,0x07e0,0x001f,0xffe0,0x07ff,0xf81f};
    const unsigned char expected[][3]={{255,0,0},{0,255,0},{0,0,255},{255,255,0},{0,255,255},{255,0,255}};
    memset(ram+256,0xcd,stride*6);
    for(unsigned f=0;f<6;f++) for(unsigned block=0;block<16;block++) {
        unsigned char *p=ram+256+stride*f+block*16;memset(p,0,16);p[0]=p[1]=255;
        memcpy(p+8,&colors[f],2);memcpy(p+10,&colors[f],2);
    }
    REQUIRE(ch_vk_texture_cache_create(&config,&cache)==VK_SUCCESS);
    REQUIRE(ch_vk_texture_cache_resolve(cache,&tx,&a)==VK_SUCCESS && ch_vk_texture_is_cube(a.gpu));
    REQUIRE(ch_vk_texture_cache_resolve(cache,&tx,&b)==VK_SUCCESS && a.gpu==b.gpu);unpin(&b);
    REQUIRE(ch_vk_texture_read_rgba(a.gpu,rgba,sizeof rgba)==VK_SUCCESS);
    for(unsigned f=0;f<6;f++) for(unsigned i=0;i<S*S;i++) REQUIRE(!memcmp(rgba+(f*S*S+i)*4,expected[f],3) && rgba[(f*S*S+i)*4+3]==255);
    for(unsigned block=0;block<16;block++) memset(ram+256+stride*5+block*16+8,255,4);
    REQUIRE(ch_vk_texture_cache_resolve(cache,&tx,&b)==VK_SUCCESS && b.gpu!=a.gpu);
    REQUIRE(ch_vk_texture_read_rgba(b.gpu,rgba,sizeof rgba)==VK_SUCCESS && rgba[5*S*S*4+1]==255);
    REQUIRE(ch_vk_texture_read_rgba(a.gpu,rgba,sizeof rgba)==VK_SUCCESS && rgba[5*S*S*4+1]==0);
    unpin(&a);unpin(&b);
    Nv2aTexture invalid=tx;invalid.offset=(uint32_t)(bytes-stride*6+1);
    REQUIRE(ch_vk_texture_cache_resolve(cache,&invalid,&c)==VK_ERROR_INITIALIZATION_FAILED && !c.gpu);
    /* Stage one uses a direction, including with the uncached decoder path. */
    REQUIRE(ch_vk_target_create(gpu,device,queue,family,4,4,&target)==VK_SUCCESS);
    const float black[]={0,0,0,0};REQUIRE(ch_vk_target_clear_read(target,black,pixels,sizeof pixels)==VK_SUCCESS);
    Nv2aVertex v[6]={{0,0,0,1,0xffffffff,0,0},{4,0,0,1,0xffffffff,0,0},{4,4,0,1,0xffffffff,0,0},
        {0,0,0,1,0xffffffff,0,0},{4,4,0,1,0xffffffff,0,0},{0,4,0,1,0xffffffff,0,0}};
    Nv2aVertexEx ex[6]={0};for(unsigned i=0;i<6;i++) {ex[i].tex[1][2]=1;ex[i].tex[1][3]=1;}
    Nv2aCombiner rc={.control=1,.stage_program=3<<5};rc.color_icw[0]=0x09040000;rc.alpha_icw[0]=0x19140000;rc.color_ocw[0]=rc.alpha_ocw[0]=0xc0;
    Nv2aSurface surface={.width=4,.height=4,.pitch=16,.bytes_per_pixel=4};Nv2aRenderState state={.color_mask=0x01010101};
    Nv2aBatch batch={.vertices=v,.count=6,.state=&state,.rc=&rc,.ext=ex,.stage={NULL,&tx}};
    REQUIRE(ch_vk_nv2a_draw_decoded_read(target,&surface,&batch,decode,pixels,sizeof pixels)==VK_SUCCESS);
    for(unsigned i=0;i<16;i++) REQUIRE(pixels[i*4]==0 && pixels[i*4+1]==255 && pixels[i*4+2]==255 && pixels[i*4+3]==255);
    REQUIRE(ch_vk_nv2a_draw_resolved_read(target,&surface,&batch,decode,ch_vk_texture_cache_resolve,cache,pixels,sizeof pixels)==VK_SUCCESS);
    for(unsigned i=0;i<16;i++) REQUIRE(pixels[i*4]==0 && pixels[i*4+1]==255 && pixels[i*4+2]==255 && pixels[i*4+3]==255);
    REQUIRE(ch_vk_nv2a_draw_resolved_read(target,&surface,&batch,decode,single_plane,NULL,pixels,sizeof pixels)==VK_SUCCESS);
    for(unsigned i=0;i<16;i++) REQUIRE(pixels[i*4]==0 && pixels[i*4+1]==255 && pixels[i*4+2]==255 && pixels[i*4+3]==255);
    rc.stage_program=6<<5;
    REQUIRE(ch_vk_nv2a_draw_decoded_read(target,&surface,&batch,decode,pixels,sizeof pixels)==VK_SUCCESS);
    for(unsigned i=0;i<sizeof pixels;i++) REQUIRE(!pixels[i]);
done:
    unpin(&a);unpin(&b);unpin(&c);ch_vk_target_destroy(target);ch_vk_texture_cache_destroy(cache);
    if(!failed) puts("PASS: actual DXT5 cube decoder, padded stored mip face strides, immutable edits, bounds and cached/uncached stage-one directional draws");
    return failed;
#undef REQUIRE
}
