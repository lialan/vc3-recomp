/* Real deferred replay: owned inline data, preserved overlap, ring reuse and
 * semaphore ordering. Guest-backed streams retain the original contract. */
#include "kernel/nv2a_pb_defer.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
uint64_t xbox_HostNowNs(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
void xbox_ThreadAcctName(const char *name) {(void)name;}
uint32_t nv2a_backend_presented_va(void) {return 0;}
void nv2a_backend_presented_override(int on,uint32_t va) {(void)on;(void)va;}
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static int release_consumer,entered,failed;
static unsigned draws,flushes;
static uint8_t stream[256],separate[32],guest[32];
static uint8_t pattern(unsigned round,unsigned at) {return (uint8_t)(round*19+at*37);}
static void clear(const Nv2aSurface *s,const Nv2aRenderState *rs,uint32_t f,uint32_t c,uint32_t z) {
 (void)s;(void)rs;(void)f;(void)c;(void)z;
 pthread_mutex_lock(&mutex);entered=1;pthread_cond_signal(&cv);
 while(!release_consumer)pthread_cond_wait(&cv,&mutex);
 pthread_mutex_unlock(&mutex);
}
static int draw(const Nv2aSurface *s,const Nv2aBatch *b) {
 const Nv2aRawGeometry *g=b->raw;const unsigned offsets[]={0,12,0,128};
 for(unsigned a=0;a<4;a++)for(unsigned i=0;i<g->attr[a].bytes;i++)
  if(((const uint8_t *)g->attr[a].data)[i]!=pattern(s->color_va,offsets[a]+i))failed=1;
 if((const uint8_t *)g->attr[1].data!=(const uint8_t *)g->attr[0].data+12 ||
    g->attr[2].data!=g->attr[0].data ||
    (const uint8_t *)g->attr[3].data!=(const uint8_t *)g->attr[0].data+128 ||
    g->attr[0].data==stream || g->attr[4].data==separate ||
    g->attr[5].data!=guest || g->attr[6].data || g->attr[6].value[3]!=.75f)failed=1;
 for(unsigned i=0;i<32;i++)if(((const uint8_t *)g->attr[4].data)[i]!=(uint8_t)(s->color_va+i))failed=1;
 draws++;return 1;
}
static void flush(void) {flushes++;}
int main(void) {
 setenv("RECOMP_PB_DECODE_THREAD","1",1);
 const Nv2aBackend real={.clear=clear,.draw_raw=draw,.flush=flush};
 const Nv2aBackend *proxy=nv2a_defer_wrap(&real);
 Nv2aSurface surface={0};Nv2aRawGeometry raw={.kind=NV2A_RAW_FIXED};
 uint16_t indices[]={0,1,2};raw.indices=indices;raw.index_count=3;
 const unsigned offsets[]={0,12,0,128},sizes[]={256,244,32,128};
 for(unsigned a=0;a<4;a++) {raw.attr[a].data=stream+offsets[a];raw.attr[a].bytes=sizes[a];raw.attr[a].stride=16;raw.attr[a].type=2;raw.attr[a].size=4;}
 raw.attr[4].data=separate;raw.attr[4].bytes=sizeof separate;
 raw.attr[5].data=guest;raw.attr[5].bytes=sizeof guest;raw.attr[5].guest_va=0x1000;
 raw.attr[6].value[3]=.75f;
 Nv2aBatch batch={.raw=&raw};volatile uint32_t sem=0;
 proxy->clear(&surface,NULL,0,0,0);
 pthread_mutex_lock(&mutex);while(!entered)pthread_cond_wait(&cv,&mutex);pthread_mutex_unlock(&mutex);
 for(unsigned round=0;round<1000;round++) {
  for(unsigned i=0;i<256;i++)stream[i]=pattern(round,i);
  for(unsigned i=0;i<32;i++)separate[i]=(uint8_t)(round+i);
  surface.color_va=round;
  for(unsigned i=0;i<8;i++)if(!proxy->draw_raw(&surface,&batch))return 1;
  memset(stream,0xee,sizeof stream);memset(separate,0xdd,sizeof separate);
  if(!round) {pthread_mutex_lock(&mutex);release_consumer=1;pthread_cond_signal(&cv);pthread_mutex_unlock(&mutex);}
  if(!nv2a_defer_write(&sem,round+1))return 1;
  nv2a_defer_drain();
  if(sem!=round+1 || draws!=(round+1)*8 || flushes!=round+1 || failed)return 1;
 }
 puts("PASS: 8000 deferred draws preserve overlapping inline streams, source mutation isolation, guest pointers, ring reuse and ordered semaphore publication");
 return 0;
}
