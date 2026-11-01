#include <stdio.h>
#include <time.h>
#include "vk_guest_layout.h"
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(int argc,char **argv){(void)argv;volatile unsigned format=(9u<<24)|(10u<<16)|0x200;ChVkGuestLayout l;ch_vk_guest_layout(format,640,480,0,4,&l);unsigned mx,my;xbox_swizzle_masks(1u<<((format>>16)&255),1u<<(format>>24),&mx,&my);volatile unsigned long long sum=0;double t=now();for(unsigned r=0;r<64;r++)for(unsigned y=0;y<480;y++)for(unsigned x=0;x<640;x++){unsigned long long at=argc>1?(unsigned long long)(swizzle_deposit(x,mx)|swizzle_deposit(y,my))*4:ch_vk_guest_offset(&l,x,y);sum+=at;}printf("%llu %.6f\n",sum,now()-t);}
