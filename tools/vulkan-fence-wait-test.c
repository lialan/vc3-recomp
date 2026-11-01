#include "recomp_types.h"
#include "chihiro/chihiro_profile.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
RECOMP_TLS struct RecompRegs g_regs;
static uint32_t memory[4096];ptrdiff_t g_xbox_mem_offset;
static uint64_t clock_ns,release_at;
static uint32_t completion;
static unsigned kicks;static const char *strict;
static void kickoff(void) {kicks++;g_esp+=8;}
static const ChihiroProfile profile={.d3d={.device_ptr=0x100,.kickoff_ret=0x1234,.kickoff=kickoff}};
const ChihiroProfile *chihiro_profile(void) {return &profile;}
const char *ch_env(const char *name) {return !strcmp(name,"STRICT_GPU_FENCES")?strict:NULL;}
void nv2a_pb_set_semaphore_target(uint32_t va) {if(va!=0x600) abort();}
uint64_t xbox_HostNowNs(void) {return clock_ns;}
void xbox_HostSleepNs(uint64_t ns) {
    clock_ns+=ns;
    if(clock_ns>=release_at) ST32(0x600,completion);
}
void vc3_d3d_block_on_time(void);
static void setup(uint32_t head,uint32_t requested,uint32_t sem) {
    memset(memory,0,sizeof memory);g_xbox_mem_offset=(ptrdiff_t)memory;g_esp=0x1000;g_eax=0xdead;
    ST32(0x100,0x200);ST32(0x200+0x2c,head);ST32(0x200+0x30,0x600);ST32(0x600,sem);
    ST32(g_esp+4,requested);clock_ns=1000;release_at=400000000;completion=requested;kicks=0;
}
int main(void) {
    setup(0x20,0x20,0);strict="0";vc3_d3d_block_on_time();
    if(clock_ns>=release_at || MEM32(0x600)!=0 || kicks!=1 || g_esp!=0x100c || g_eax) return 1;
    setup(0x20,0x20,0);strict="1";vc3_d3d_block_on_time();
    if(clock_ns<release_at || MEM32(0x600)!=0x20 || kicks!=1 || g_esp!=0x100c || g_eax) return 2;
    setup(0x10,0xfffffff8,0xfffffff0);completion=8;vc3_d3d_block_on_time();
    if(clock_ns<release_at || MEM32(0x600)!=8 || kicks || g_esp!=0x100c) return 3;
    setup(0x30,0x20,0x20);vc3_d3d_block_on_time();if(clock_ns!=1000 || kicks) return 4;
    setup(0x20,0x20,0);strict=NULL;vc3_d3d_block_on_time();
#ifdef CHIHIRO_RENDERER_VULKAN
    if(clock_ns<release_at || MEM32(0x600)!=0x20) return 7;
#else
    if(clock_ns>=release_at || MEM32(0x600)!=0) return 7;
#endif
    strict="1";
    pid_t child=fork();if(child<0) return 5;
    if(!child) {
        struct rlimit limit={0,0};setrlimit(RLIMIT_CORE,&limit);
        setup(0x20,0x20,0);release_at=UINT64_MAX;vc3_d3d_block_on_time();_exit(9);
    }
    int status;if(waitpid(child,&status,0)!=child || !WIFSIGNALED(status) || WTERMSIG(status)!=SIGABRT) return 6;
    puts("PASS: actual BlockOnTime strict wait retains a slow GPU fence, preserves wraparound/ABI/kickoff, and fails closed on a stuck semaphore; legacy 250-ms control unchanged");
    return 0;
}
