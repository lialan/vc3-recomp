/* CPU exhaustive oracle for a proposed float32-only GPU depth quantizer.
 * This does not validate GPU execution or enable resident D32 depth. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint32_t bits(float f){uint32_t b;memcpy(&b,&f,4);return b;}
static float value(uint32_t b){float f;memcpy(&f,&b,4);return f;}
int main(void){
for(uint32_t q=0;q<0x1000000;q++){
float expected=(float)((double)q/16777215.0);
float actual=value(bits((float)q/16777216.0)+(q!=0));
if(bits(expected)!=bits(actual)){printf("normalize mismatch %x\n",q);return 1;}}
for(uint32_t b=0;b<=0x3f800000;b++){
float z=value(b),scaled=z*16777216.0f;uint32_t q=(uint32_t)scaled;
if(z>.5f)q--;else if(scaled-(float)q>.5f)q++;
uint32_t ref=(uint32_t)((double)z*16777215.0+0.5);
if(q!=ref){printf("quant mismatch %x %x %x\n",b,q,ref);return 2;}}
puts("PASS: all 16777216 depth codes normalize exactly; every float bit pattern in [0,1] quantizes exactly");}
