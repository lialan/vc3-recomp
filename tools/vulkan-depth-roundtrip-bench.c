/* Game-independent full-surface depth conversion comparison. */
#include "vk_target.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int ch_test_depth_roundtrip(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
 const size_t n=640*480;ChVkTarget *t=NULL;int failed=1;
 float *z=malloc(n*4),*out=malloc(n*4);unsigned char *st=malloc(n),*so=malloc(n),*rgba=malloc(n*4);
 if(!z||!out||!st||!so||!rgba)goto done;
 for(size_t i=0;i<n;i++){z[i]=(float)((i*65537u)&0xffffffu)*0x1p-24f;st[i]=(unsigned char)i;}
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"depth roundtrip benchmark failed line %d\n",__LINE__);goto done;}}while(0)
 for(unsigned draw=0;draw<3;draw++) {
  REQUIRE((draw==2?ch_vk_target_create_input_depth:ch_vk_target_create)(gpu,device,queue,family,640,480,&t)==VK_SUCCESS);
  const float color[]={1,0,1,1};
  REQUIRE(ch_vk_target_clear_read(t,color,rgba,n*4)==VK_SUCCESS);
  REQUIRE(ch_vk_target_write_depth_stencil(t,z,st,n)==VK_SUCCESS);
  VkResult (*quantize)(ChVkTarget *)=draw==2?ch_vk_target_quantize_depth_input:draw?ch_vk_target_quantize_depth_draw:ch_vk_target_quantize_depth;
  REQUIRE(quantize(t)==VK_SUCCESS); /* Warm pipeline creation outside timing. */
  double start=now();for(unsigned i=0;i<128;i++)REQUIRE(quantize(t)==VK_SUCCESS);
  printf("depth roundtrip %s: %.3f ms/call (640x480, 128 synchronous calls)\n",draw==2?"input":draw?"draw":"compute",(now()-start)*1000/128);
  REQUIRE(ch_vk_target_read_depth_stencil(t,out,so,n)==VK_SUCCESS);
  for(size_t i=0;i<n;i++){uint32_t q=(uint32_t)((double)z[i]*16777215.0+0.5);REQUIRE(out[i]==(float)((double)q/16777215.0) && so[i]==st[i]);}
  REQUIRE(ch_vk_target_read_color(t,rgba,n*4)==VK_SUCCESS);
  for(size_t i=0;i<n*4;i++)REQUIRE(rgba[i]==(i%4==1?0:255));
  ch_vk_target_destroy(t);t=NULL;
 }
 /* Same commands and exact conversion, compare repeated pass breaks with
  * framebuffer-local barriers inside one pass. No per-draw host wait. */
 for(unsigned mode=1;mode<=4;mode+=3) {
  REQUIRE((mode==4?ch_vk_target_create_input_depth:ch_vk_target_create)(gpu,device,queue,family,640,480,&t)==VK_SUCCESS);
  const float color[]={1,0,1,1};
  REQUIRE(ch_vk_target_clear_read(t,color,rgba,n*4)==VK_SUCCESS);
  if(mode==1) REQUIRE(ch_vk_target_depth_quantization(t,4)==VK_ERROR_FEATURE_NOT_PRESENT);
  REQUIRE(ch_vk_target_depth_quantization(t,mode)==VK_SUCCESS);
  REQUIRE(ch_vk_target_batching(t,1)==VK_SUCCESS);
  ch_vk_target_defer_color_readback(t,1);
  ChVkColorVertex vertices[]={{{-1,-1,.3f,1},{1,0,1,1}},{{3,-1,.3f,1},{1,0,1,1}},{{-1,3,.3f,1},{1,0,1,1}}};
  const uint16_t indices[]={0,1,2};
  Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=1,.depth_write=1,.depth_func=0x207};
  REQUIRE(ch_vk_target_draw_combiner_read(t,NULL,vertices,3,indices,3,&state,NULL,NULL,NULL,0,NULL,rgba,n*4)==VK_SUCCESS);
  REQUIRE(ch_vk_target_finish(t)==VK_SUCCESS);
  ChVkTargetResourceStats before,after;ch_vk_target_resource_stats(t,&before);
  double start=now();
  for(unsigned i=0;i<128;i++) REQUIRE(ch_vk_target_draw_combiner_read(t,NULL,vertices,3,indices,3,&state,NULL,NULL,NULL,0,NULL,rgba,n*4)==VK_SUCCESS);
  REQUIRE(ch_vk_target_finish(t)==VK_SUCCESS);
  ch_vk_target_resource_stats(t,&after);
  REQUIRE(after.batch_submissions-before.batch_submissions==1 && after.batch_draws-before.batch_draws==128);
  printf("depth queued %s: %.3f ms/draw (640x480, 128 draws, one batch)\n",mode==4?"inline":"compute",(now()-start)*1000/128);
  REQUIRE(ch_vk_target_read_depth_stencil(t,out,so,n)==VK_SUCCESS);
  uint32_t code=(uint32_t)((double).3f*16777215.0+0.5);
  for(size_t i=0;i<n;i++) REQUIRE(out[i]==(float)((double)code/16777215.0) && so[i]==0);
  ch_vk_target_destroy(t);t=NULL;
 }
 puts("PASS: full-surface compute/buffer-draw/input-draw quantization matches exact depth, stencil and untouched color");failed=0;
done:ch_vk_target_destroy(t);free(z);free(out);free(st);free(so);free(rgba);return failed;
#undef REQUIRE
}
