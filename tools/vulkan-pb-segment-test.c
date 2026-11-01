#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static uint32_t memory[2048],reg;static unsigned calls,fast,dma;
struct Event {uint32_t s,m,p;} events[256],reference[256];
ptrdiff_t xbox_GetMemoryOffset(void){return (ptrdiff_t)memory-0x80000000u;}
void nv2a_pb_exec_method(uint32_t s,uint32_t m,uint32_t p){if(calls>=256) abort();events[calls++]=(struct Event){s,m,p};}
int nv2a_pb_exec_data_run(uint32_t s,uint32_t m,const uint32_t*p,uint32_t n){if(!fast)return 0;for(unsigned j=0;j<n;j++)nv2a_pb_exec_method(s,m,p[j]);return 1;}
int nv2a_pb_exec_vsh_run(uint32_t s,uint32_t m,int i,const uint32_t*p,uint32_t n){if(!fast)return 0;for(unsigned j=0;j<n;j++)nv2a_pb_exec_method(s,m+(i?j*4:0),p[j]);return 1;}
uint32_t nv2a_pb_exec_reg_run(uint32_t s,uint32_t m,const uint32_t*p,uint32_t n){if(!fast)return 0;unsigned count=n/2;for(unsigned j=0;j<count;j++)nv2a_pb_exec_method(s,m+j*4,p[j]);return count;}
void nv2a_pb_exec_report(void){}
void xbox_Nv2aAckBusyBits(void){}
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t o){return &reg;}
void nv2a_pb_exec_walk_begin(void){}
void nv2a_pb_scan_segment(uint32_t,uint32_t);

void nv2a_pb_resync(uint32_t);void nv2a_pb_scan(uint32_t);
static void scan(unsigned a,unsigned b){
 if(dma){if(a==0||a==100){reg=a*4;nv2a_pb_resync(reg);}nv2a_pb_scan(b*4);}
 else nv2a_pb_scan_segment(0x80000000u+a*4,0x80000000u+b*4);
}
int main(int argc,char **argv){
 (void)argv;dma=argc>1;
 setenv("RECOMP_PB_EXEC","1",1);
 unsigned n=0;
 for(unsigned k=0;k<4;k++){
   unsigned method=k==0?0x354:k==1?0x1800:k==2?0xb00:0x400;
   memory[n++]=(7u<<18)|(3u<<13)|method|(k==1?0x40000000u:0);
   for(unsigned j=0;j<7;j++)memory[n++]=j%2?0xff000000u+j:0x41d70u+j;
 }
 /* Nested scan for a CALL must not consume top-level continuation state. */
 memory[n++]=0x1002;memory[n++]=(1u<<18)|0x354;memory[n++]=0x207;
 memory[1024]=(2u<<18)|0x400;memory[1025]=123;memory[1026]=456;memory[1027]=0x20000;
 scan(0,n);unsigned expected=calls;memcpy(reference,events,sizeof events);
 for(fast=0;fast<2;fast++)for(unsigned chunk=1;chunk<=n;chunk++){
   calls=0;for(unsigned i=0;i<n;i+=chunk)scan(i,i+chunk<n?i+chunk:n);
   if(calls!=expected||memcmp(reference,events,calls*sizeof *events)){fprintf(stderr,"split failure fast=%u chunk=%u calls=%u expected=%u\n",fast,chunk,calls,expected);return 1;}
 }
 /* A discontinuous restart starts at a header, not old payload. */
 calls=0;scan(0,2);memory[100]=(1u<<18)|0x354;memory[101]=0x207;scan(100,102);
 if(calls!=2||events[1].m!=0x354||events[1].p!=0x207)return 2;
 if(dma)for(unsigned style=0;style<2;style++){
   calls=0;memory[200]=(1u<<18)|0x354;memory[201]=0x207;
   memory[202]=(style?0x20000000u:1u)|(50u*4);
   memory[50]=(1u<<18)|0x358;memory[51]=123;
   reg=200*4;nv2a_pb_resync(reg);nv2a_pb_scan(52*4);
   if(calls!=2||events[0].m!=0x354||events[1].m!=0x358||events[1].p!=123||reg!=52*4)return 3;
 }
 puts("PASS: every segment split matches unsplit methods/parameters, scalar and bulk paths, CALL/RETURN and discontinuous restart");return 0;
}
