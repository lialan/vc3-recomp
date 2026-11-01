#define _GNU_SOURCE
#include "vk_backend.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
static ChGuestWriteWatch *test_watch;
static void watch_fault(int sig,siginfo_t *info,void *context) {
    (void)sig;
    if(!ch_guest_write_watch_fault(test_watch,(uintptr_t)info->si_addr,ch_guest_write_fault_is_write(context))) _exit(98);
}
static uint32_t word(const unsigned char *p) {uint32_t n;memcpy(&n,p,4);return n;}
static void put(unsigned char *p,uint32_t n) {memcpy(p,&n,4);}
int ch_test_write_watch(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family,ChVkDecodeTexture decode) {
    size_t page=(size_t)sysconf(_SC_PAGESIZE),bytes=page*8;
    const char *temp=getenv("TMPDIR");
#if defined(__ANDROID__)
    if(!temp) temp="/data/local/tmp";
#else
    if(!temp) temp="/tmp";
#endif
    char path[4096];int n=snprintf(path,sizeof path,"%s/vc3-vulkan-watch-XXXXXX",temp);
    if(n<0 || (size_t)n>=sizeof path) return 1;
    int fd=mkstemp(path);if(fd<0) return 1;
    unlink(path);
    if(ftruncate(fd,(off_t)bytes)) {close(fd);return 1;}
    unsigned char *a=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    unsigned char *alias=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    close(fd);
    if(a==MAP_FAILED || alias==MAP_FAILED) {
        if(a!=MAP_FAILED) munmap(a,bytes);
        if(alias!=MAP_FAILED) munmap(alias,bytes);
        return 1;
    }
    ChGuestWriteView views[]={{a,bytes},{alias,bytes}};
    ChVkBackend *backend=NULL;int failed=1,installed=0;
    struct sigaction old_segv,old_bus,sa={0};
#define REQUIRE(x) do {if(!(x)) {fprintf(stderr,"GPU write watch failure line %d: %s\n",__LINE__,#x);goto done;}}while(0)
    test_watch=ch_guest_write_watch_create(views,2);REQUIRE(test_watch);
    sa.sa_sigaction=watch_fault;sa.sa_flags=SA_SIGINFO;sigemptyset(&sa.sa_mask);
    REQUIRE(!sigaction(SIGSEGV,&sa,&old_segv));
    if(sigaction(SIGBUS,&sa,&old_bus)) {sigaction(SIGSEGV,&old_segv,NULL);goto done;}
    installed=1;
    ChVkBackendConfig config={.physical=gpu,.device=device,.queue=queue,.family=family,
        .guest_base=a,.guest_bytes=bytes,.decode=decode,.resident_color=1,.resident_depth=1,
        .batch_draws=1,.precise_queries=1,.guest_write_watch=test_watch};
    REQUIRE(ch_vk_backend_create(&config,&backend)==VK_SUCCESS && ch_vk_backend_attach(backend)==VK_SUCCESS);
    const Nv2aBackend *cb=ch_vk_backend_callbacks();
    Nv2aSurface surface={.color_va=(uint32_t)page,.width=4,.height=4,.pitch=16,.bytes_per_pixel=4};
    Nv2aRenderState rs={.surface_format=0x128,.zeta_va=(uint32_t)(3*page),.zeta_pitch=16,
        .color_mask=0x01010101,.depth_max=1};
    cb->clear(&surface,&rs,0xf3,0xffff0000,0x40000007);cb->flush();
    for(unsigned i=0;i<16;i++) REQUIRE(word(a+page+i*4)==0xffff0000);
    put(alias+page,0xff00ff00);put(alias+3*page+4,0x8000002a);
    Nv2aVertex vertices[6]={0};
    const float xy[6][2]={{0,0},{4,0},{4,4},{0,0},{4,4},{0,4}};
    for(unsigned i=0;i<6;i++) {vertices[i].x=xy[i][0];vertices[i].y=xy[i][1];
        vertices[i].z=.5f;vertices[i].rhw=1;vertices[i].diffuse=0xff100000;}
    rs.color_mask=0x10000;rs.depth_test_enable=1;rs.depth_func=0x207;
    Nv2aBatch batch={.vertices=vertices,.count=6,.state=&rs};
    for(unsigned i=0;i<100;i++) cb->draw(&surface,&batch);
    cb->flush();
    REQUIRE(word(a+page)==0xff10ff00);
    for(unsigned i=1;i<16;i++) REQUIRE(word(a+page+i*4)==0xff100000);
    REQUIRE(word(a+3*page+4)==0x8000002a);
    rs.depth_func=0x202;uint32_t samples=0;batch.zpass_out=&samples;
    cb->draw(&surface,&batch);REQUIRE(samples==1);batch.zpass_out=NULL;
    /* CPU edit while a new GPU color is pending preserves GPU neighbors. */
    cb->clear(&surface,&rs,0xf0,0xff0000ff,0);
    put(alias+page+8,0xffffff00);rs.depth_test_enable=0;
    cb->draw(&surface,&batch);cb->flush();
    for(unsigned i=0;i<16;i++) REQUIRE(word(a+page+i*4)==(i==2?0xff10ff00:0xff1000ff));
    /* An explicit protection change disables this page's shortcut. Edits via
     * its still-writable canonical mapping retain exact fallback coherence. */
    ch_guest_write_watch_invalidate(test_watch,alias+page,64);
    REQUIRE(!mprotect(alias+page,page,PROT_READ));
    put(a+page+12,0xff00ff00);cb->draw(&surface,&batch);cb->flush();
    REQUIRE(word(a+page+12)==0xff10ff00);
    puts("PASS: write-tracked resident Vulkan color/depth, 100 unchanged draws, alias CPU edits, exact EQUAL visibility, pending GPU neighbor merge and protection-change fallback");
    failed=0;
done:
    ch_vk_backend_destroy(backend);
    if(test_watch && !ch_guest_write_watch_destroy(test_watch)) abort();
    test_watch=NULL;
    if(installed) {sigaction(SIGSEGV,&old_segv,NULL);sigaction(SIGBUS,&old_bus,NULL);}
    munmap(a,bytes);munmap(alias,bytes);return failed;
#undef REQUIRE
}
