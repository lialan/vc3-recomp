#include "vk_target.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000.+t.tv_nsec/1e6;}
static int compare(const void*a,const void*b){double x=*(const double*)a,y=*(const double*)b;return (x>y)-(x<y);}
int ch_test_depth_fragment(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family){
 enum{W=640,H=480,N=W*H};uint32_t *z=malloc(N*4),*out=malloc(N*4),*color=malloc(N*4);ChVkTarget*t=NULL;
 if(!z||!out||!color)return 1;
 for(unsigned i=0;i<N;i++)z[i]=i*0x17234567u;
 for(unsigned run=0;run<6;run++){
  int direct=run%2;setenv("CHIHIRO_VULKAN_DIRECT_COLOR_READ",direct?"1":"0",1);setenv("CHIHIRO_VULKAN_DIRECT_DEPTH_READ",direct?"1":"0",1);
#define CHECK(x) do{if((x)!=VK_SUCCESS){fprintf(stderr,"failure line %d\n",__LINE__);return 1;}}while(0)
  CHECK(ch_vk_target_create(gpu,device,queue,family,W,H,&t));CHECK(ch_vk_target_color_layout(t,1));
  const float clear[]={1,0,1,1};CHECK(ch_vk_target_clear_read(t,clear,color,N*4));CHECK(ch_vk_target_write_z24s8(t,z,N));
  double values[240];
  for(unsigned i=0;i<256;i++){
   double start=now();CHECK(ch_vk_target_read_depth_color(t,out,NULL,N,1,color,N*4));
   if(i>=16)values[i-16]=now()-start;
  }
  if(memcmp(z,out,N*4))return 2;
  for(unsigned i=0;i<N;i++)if(color[i]!=0xffff00ffu)return 3;
  double sum=0;for(unsigned i=0;i<240;i++)sum+=values[i];qsort(values,240,sizeof(double),compare);
  printf("run=%u direct=%d mean_ms=%.6f p50_ms=%.6f p95_ms=%.6f\n",run,direct,sum/240,values[120],values[227]);fflush(stdout);
  ch_vk_target_destroy(t);t=NULL;
 }
 free(z);free(out);free(color);return 0;
}
