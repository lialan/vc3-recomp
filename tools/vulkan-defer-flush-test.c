/* Actual submission ring, isolated CPU backend. Pending output must become
 * visible before guest completion writes, on the submission thread. */
#include "nv2a_pb_defer.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
static pthread_t owner;
static uint32_t pending,visible;
static unsigned draws,flushes;
void xbox_ThreadAcctName(const char *name) { if(strcmp(name,"nv2a submit")) abort(); }
uint64_t xbox_HostNowNs(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;}
uint32_t nv2a_backend_presented_va(void) {return 0;}
void nv2a_backend_presented_override(int active,uint32_t va) {(void)active;(void)va;abort();}
static void draw(const Nv2aSurface *s,const Nv2aBatch *b) {
    (void)s;(void)b;
    if(!draws) owner=pthread_self();
    else if(!pthread_equal(owner,pthread_self())) abort();
    pending=++draws;
}
static void flush(void) {
    if(!pthread_equal(owner,pthread_self())) abort();
    visible=pending;flushes++;
}
int main(int argc,char **argv) {
    const char *mode=argc>1?argv[1]:"1";
    if(strcmp(mode,"1") && strcmp(mode,"2")) return 2;
    setenv("RECOMP_PB_DECODE_THREAD",mode,1);
    Nv2aBackend real={.draw=draw,.flush=flush};
    const Nv2aBackend *proxy=nv2a_defer_wrap(&real);
    Nv2aSurface surface={.width=1,.height=1};
    Nv2aRenderState state={0};
    Nv2aVertex vertex={0};
    Nv2aBatch batch={.vertices=&vertex,.count=1,.state=&state};
    volatile uint32_t word=0;
    proxy->draw(&surface,&batch);
    if(!nv2a_defer_write(&word,17)) return 1;
    nv2a_defer_drain();
    if(word!=17 || visible!=1 || flushes!=1 || pthread_equal(owner,pthread_self())) return 1;
    proxy->draw(&surface,&batch);
    proxy->flush();
    if(visible!=2 || flushes!=2) return 1;
    printf("PASS: actual deferred ring mode %s flushes before completion writes and explicit drains on the submission thread\n",mode);
    return 0;
}
