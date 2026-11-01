#include "vk_texture_cache.h"
#include <stdio.h>
#include <string.h>
static void unpin(ChVkSampleImage *image) {
    if(image->gpu && image->release) image->release(image->release_context,image->gpu);
    memset(image,0,sizeof *image);
}
static int pixel(ChVkSampleImage *image,unsigned r,unsigned g,unsigned b) {
    unsigned char rgba[64];
    return ch_vk_texture_read_rgba(image->gpu,rgba,sizeof rgba)==VK_SUCCESS &&
        rgba[0]==r && rgba[1]==g && rgba[2]==b && rgba[3]==255;
}
int ch_test_texture_cache(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family,
    void *guest,size_t bytes,ChVkDecodeTexture decode) {
    ChVkTextureCache *cache=NULL;
    ChVkTextureCacheConfig config={.physical=gpu,.device=device,.queue=queue,.family=family,
        .guest_base=guest,.guest_bytes=bytes,.decode=decode,.max_entries=2,.max_bytes=4096};
    if(bytes<4096 || ch_vk_texture_cache_create(&config,&cache)!=VK_SUCCESS) return 1;
    unsigned char *memory=guest;
    uint32_t red=0xffff0000,green=0xff00ff00;
    for(unsigned i=0;i<16;i++) {memcpy(memory+256+i*4,&red,4);memcpy(memory+512+i*4,&green,4);}
    Nv2aTexture tx={.offset=256,.width=4,.height=4,.pitch=16,.color=0x12,.levels=1,
        .filter=0x01010000,.addr_u=3,.addr_v=3};
    ChVkSampleImage a={0},b={0},c={0};
    int failed=0;
#define REQUIRE(x) do {if(!(x)) {fprintf(stderr,"texture cache failure line %d: %s\n",__LINE__,#x);failed=1;goto done;}}while(0)
    REQUIRE(ch_vk_texture_cache_resolve(cache,&tx,&a)==VK_SUCCESS && pixel(&a,255,0,0));
    REQUIRE(ch_vk_texture_cache_resolve(cache,&tx,&b)==VK_SUCCESS && a.gpu==b.gpu);
    unpin(&b);
    /* Same VA/dimensions with one changed encoded byte must create a new
     * immutable image while the old red image is still pinned. */
    memory[256]=255;
    REQUIRE(ch_vk_texture_cache_resolve(cache,&tx,&b)==VK_SUCCESS && a.gpu!=b.gpu);
    REQUIRE(pixel(&a,255,0,0) && pixel(&b,255,0,255));
    Nv2aTexture other=tx;other.offset=512;
    REQUIRE(ch_vk_texture_cache_resolve(cache,&other,&c)==VK_NOT_READY && !c.gpu);
    unpin(&a);
    REQUIRE(ch_vk_texture_cache_resolve(cache,&other,&c)==VK_SUCCESS && pixel(&c,0,255,0));
    REQUIRE(pixel(&b,255,0,255));
    unpin(&b);unpin(&c);
    tx.filter=0x02020000;
    REQUIRE(ch_vk_texture_cache_resolve(cache,&tx,&a)==VK_SUCCESS);
    VkDescriptorImageInfo linear=ch_vk_texture_descriptor(a.gpu);
    tx.filter=0x01010000;
    REQUIRE(ch_vk_texture_cache_resolve(cache,&tx,&b)==VK_SUCCESS);
    REQUIRE(linear.sampler!=ch_vk_texture_descriptor(b.gpu).sampler && a.gpu!=b.gpu);
    unpin(&a);unpin(&b);
    /* P8 palette edits invalidate even when all index bytes are unchanged. */
    memset(memory+768,0,16);memcpy(memory+1024,&red,4);
    tx=(Nv2aTexture){.offset=768,.width=4,.height=4,.color=0x0b,.palette=1024,
        .filter=0x01010000,.levels=1,.addr_u=3,.addr_v=3};
    REQUIRE(ch_vk_texture_cache_resolve(cache,&tx,&a)==VK_SUCCESS && pixel(&a,255,0,0));
    memcpy(memory+1024,&green,4);
    REQUIRE(ch_vk_texture_cache_resolve(cache,&tx,&b)==VK_SUCCESS && pixel(&b,0,255,0));
    REQUIRE(pixel(&a,255,0,0));
    unpin(&a);unpin(&b);
    /* An odd YUY2 width still reads the last pair's shared V byte. */
    tx=(Nv2aTexture){.offset=1536,.width=1,.height=1,.pitch=4,.color=0x24,.levels=1,
        .filter=0x01010000,.addr_u=3,.addr_v=3};
    const unsigned char yuy2[]={16,128,16,128};memcpy(memory+1536,yuy2,4);
    size_t span,palette;
    REQUIRE(ch_vk_texture_storage_span(&tx,&span,&palette)==VK_SUCCESS && span==4 && !palette);
    REQUIRE(ch_vk_texture_cache_resolve(cache,&tx,&a)==VK_SUCCESS && pixel(&a,0,0,0));
    memory[1539]=255;
    REQUIRE(ch_vk_texture_cache_resolve(cache,&tx,&b)==VK_SUCCESS && a.gpu!=b.gpu && pixel(&b,203,0,0));
    REQUIRE(pixel(&a,0,0,0));
    unpin(&a);unpin(&b);
    tx=(Nv2aTexture){.offset=768,.width=4,.height=4,.color=0x0b,.palette=1024,
        .filter=0x01010000,.levels=1,.addr_u=3,.addr_v=3};
    tx.offset=(uint32_t)bytes-1;
    REQUIRE(ch_vk_texture_cache_resolve(cache,&tx,&c)==VK_ERROR_INITIALIZATION_FAILED && !c.gpu);
    tx.offset=768;tx.palette=(uint32_t)bytes-8;
    REQUIRE(ch_vk_texture_cache_resolve(cache,&tx,&c)==VK_ERROR_INITIALIZATION_FAILED && !c.gpu);
    ChVkTextureCacheStats stats;ch_vk_texture_cache_stats(cache,&stats);
    REQUIRE(stats.hits>=1 && stats.uploads>=7 && stats.evictions>=5 && stats.bytes<=config.max_bytes);
done:
    unpin(&a);unpin(&b);unpin(&c);ch_vk_texture_cache_destroy(cache);
    if(!failed) puts("PASS: immutable texture cache hits, same-VA texel/palette edits, sampler keys, pinned eviction/budget fallback and guest bounds");
    return failed;
#undef REQUIRE
}
