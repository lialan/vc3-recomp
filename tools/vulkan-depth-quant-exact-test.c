/* Exhaustive double-precision guest-store and binary32 nearest-even attachment
 * oracles. GPU execution is covered separately by Vulkan fixtures. */
#include "../runtime/src/gpu/vulkan/vk_depth_quant_math.h"
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static uint32_t bits(float f){uint32_t b;memcpy(&b,&f,4);return b;}
static float value(uint32_t b){float f;memcpy(&f,&b,4);return f;}
int main(void){
for(uint32_t q=0;q<0x1000000;q++){
float expected=(float)((double)q/16777215.0);
float actual=ch_vk_depth_unpack(q);
if(ch_vk_depth_pack_attachment(actual)!=q){printf("attachment code roundtrip mismatch %x\n",q);return 3;}
if(bits(expected)!=bits(actual)){printf("normalize mismatch %x\n",q);return 1;}}
for(uint32_t b=0;b<=0x3f800000;b++){
float z=value(b);uint32_t q=ch_vk_depth_pack(z);
uint32_t ref=(uint32_t)((double)z*16777215.0+0.5);
if(ch_vk_depth_pack_attachment(z)!=(uint32_t)nearbyintf(z*16777215.0f)){printf("attachment pack mismatch %x\n",b);return 4;}
if(q!=ref){printf("quant mismatch %x %x %x\n",b,q,ref);return 2;}}
puts("PASS: all 16777216 depth codes normalize exactly; every float bit pattern in [0,1] matches both distinct pack oracles exactly");}
