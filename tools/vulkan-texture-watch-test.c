#define _GNU_SOURCE
#include "vk_texture_cache.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
static ChGuestWriteWatch *watch;
static unsigned char *memory;
static void fault(int sig,siginfo_t *info,void *context) {
    (void)sig;
    if(!ch_guest_write_watch_fault(watch,(uintptr_t)info->si_addr,ch_guest_write_fault_is_write(context))) _exit(98);
}
static int decode(const Nv2aTexture *t,uint32_t *out) {
    if(t->color==0x12) memcpy(out,memory+t->offset,4);
    else if(t->color==0x0b) memcpy(out,memory+t->palette+memory[t->offset]*4,4);
    else return 0;
    return 1;
}
static void release(ChVkSampleImage *i) {
    if(i->release) i->release(i->release_context,i->gpu);
    memset(i,0,sizeof *i);
}
static int pixel(ChVkSampleImage *i,unsigned r,unsigned g) {
    unsigned char p[4];return ch_vk_texture_read_rgba(i->gpu,p,4)==VK_SUCCESS && p[0]==r && p[1]==g && !p[2] && p[3]==255;
}
int ch_test_texture_watch(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    size_t page=(size_t)sysconf(_SC_PAGESIZE),bytes=page*4;
    char path[]="/tmp/ch-texture-watch-XXXXXX";int fd=mkstemp(path);
    if(fd<0) return 1;
    unlink(path);if(ftruncate(fd,(off_t)bytes)) {close(fd);return 1;}
    memory=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    unsigned char *alias=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);close(fd);
    if(memory==MAP_FAILED || alias==MAP_FAILED) return 1;
    ChGuestWriteView views[]={{memory,bytes},{alias,bytes}};
    ChVkTextureCache *cache=NULL;ChVkSampleImage old={0},current={0},probe={0};
    struct sigaction sa={0},segv,bus;int installed=0,failed=1;
#define REQUIRE(x) do {if(!(x)) {fprintf(stderr,"texture watch line %d: %s\n",__LINE__,#x);goto done;}}while(0)
    watch=ch_guest_write_watch_create(views,2);REQUIRE(watch);
    sa.sa_sigaction=fault;sa.sa_flags=SA_SIGINFO;sigemptyset(&sa.sa_mask);
    REQUIRE(!sigaction(SIGSEGV,&sa,&segv));
    if(sigaction(SIGBUS,&sa,&bus)) {sigaction(SIGSEGV,&segv,NULL);goto done;} installed=1;
    ChVkTextureCacheConfig cfg={.physical=gpu,.device=device,.queue=queue,.family=family,
        .guest_base=memory,.guest_bytes=bytes,.decode=decode,.guest_write_watch=watch,.reuse_uploads=1,.queued_uploads=1};
    REQUIRE(ch_vk_texture_cache_create(&cfg,&cache)==VK_SUCCESS);
    uint32_t red=0xffff0000,green=0xff00ff00;
    for(unsigned palette=0;palette<2;palette++) {
        Nv2aTexture t={.offset=(uint32_t)page,.palette=(uint32_t)(2*page),.width=1,.height=1,.pitch=4,
            .color=palette?0x0b:0x12,.levels=1,.filter=0x01010000,.addr_u=3,.addr_v=3};
        unsigned char *color=alias+(palette?2*page:page);
        if(palette) alias[page]=0;
        memcpy(color,&red,4);
        REQUIRE(ch_vk_texture_cache_resolve(cache,&t,&old)==VK_SUCCESS && pixel(&old,255,0));
        for(unsigned n=0;n<3;n++) {REQUIRE(ch_vk_texture_cache_resolve(cache,&t,&probe)==VK_SUCCESS && probe.gpu==old.gpu);release(&probe);}
        memcpy(color,&green,4);
        REQUIRE(ch_vk_texture_cache_resolve(cache,&t,&current)==VK_SUCCESS && pixel(&current,0,255) && pixel(&old,255,0));
        /* Repeated failed comparisons against pinned old snapshots must stay failed. */
        for(unsigned n=0;n<3;n++) {REQUIRE(ch_vk_texture_cache_resolve(cache,&t,&probe)==VK_SUCCESS && probe.gpu==current.gpu && pixel(&probe,0,255));release(&probe);}
        memcpy(color,&red,4);
        REQUIRE(ch_vk_texture_cache_resolve(cache,&t,&probe)==VK_SUCCESS && pixel(&probe,255,0));release(&probe);
        ch_guest_write_watch_begin(watch,color,4);
        memcpy(color,&green,4);
        REQUIRE(ch_vk_texture_cache_resolve(cache,&t,&probe)==VK_SUCCESS && pixel(&probe,0,255));release(&probe);
        memcpy(color,&red,4);
        REQUIRE(ch_vk_texture_cache_resolve(cache,&t,&probe)==VK_SUCCESS && pixel(&probe,255,0));release(&probe);
        ch_guest_write_watch_end(watch,color,4);
        ChVkTexture *reuse=current.gpu;release(&current);
        uint32_t yellow=0xffffff00;memcpy(color,&yellow,4);
        REQUIRE(ch_vk_texture_cache_resolve(cache,&t,&probe)==VK_SUCCESS && probe.gpu==reuse && pixel(&probe,255,255));
        REQUIRE(pixel(&old,255,0));release(&probe);
        /* A second mutation must not inherit the old generation comparison. */
        memcpy(color,&green,4);
        REQUIRE(ch_vk_texture_cache_resolve(cache,&t,&probe)==VK_SUCCESS && probe.gpu==reuse && pixel(&probe,0,255));
        release(&probe);release(&old);
    }
    ChVkTextureCacheStats stats;ch_vk_texture_cache_stats(cache,&stats);REQUIRE(stats.tracked_checks>=4 && stats.cpu_changes && stats.cpu_reuses>=4);
    ch_guest_write_watch_invalidate(watch,alias+page,page);
    Nv2aTexture t={.offset=(uint32_t)page,.width=1,.height=1,.pitch=4,.color=0x12,.levels=1,.filter=0x01010000,.addr_u=3,.addr_v=3};
    for(unsigned n=0;n<4;n++) {
        memcpy(alias+page,n&1?&green:&red,4);
        REQUIRE(ch_vk_texture_cache_resolve(cache,&t,&probe)==VK_SUCCESS && pixel(&probe,n&1?0:255,n&1?255:0));release(&probe);
    }
    puts("PASS: texture write tracking, aliases, palettes, pinned snapshots, repeated mismatch, restored bytes, active write leases and invalidation fallback");failed=0;
done:
    release(&probe);release(&current);release(&old);ch_vk_texture_cache_destroy(cache);
    if(watch && !ch_guest_write_watch_destroy(watch)) abort();watch=NULL;
    if(installed) {sigaction(SIGSEGV,&segv,NULL);sigaction(SIGBUS,&bus,NULL);}
    munmap(memory,bytes);munmap(alias,bytes);return failed;
#undef REQUIRE
}
