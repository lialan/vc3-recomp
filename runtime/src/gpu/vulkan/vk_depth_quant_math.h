#ifndef CH_VK_DEPTH_QUANT_MATH_H
#define CH_VK_DEPTH_QUANT_MATH_H
#include <stdint.h>
#include <string.h>
#include <float.h>
_Static_assert(sizeof(float)==4 && FLT_RADIX==2 && FLT_MANT_DIG==24,
    "Depth quantization requires IEEE binary32");
/* Exact equivalents of the shared double-precision Z24 conversion, using
 * binary32 arithmetic. Callers validate finite z in [0,1] and q <= 0xffffff.
 * Keep in sync with depth_quant.glsl; the exhaustive CPU oracle covers every
 * input code and every nonnegative binary32 value through 1. */
static inline uint32_t ch_vk_depth_pack(float z) {
    float scaled=z*16777216.0f;
    uint32_t q=(uint32_t)scaled;
    if(z>0.5f) q--;
    else if(scaled-(float)q>0.5f) q++;
    return q;
}
static inline float ch_vk_depth_unpack(uint32_t q) {
    float z=(float)q*0x1p-24f;
    uint32_t bits;memcpy(&bits,&z,sizeof bits);
    bits+=(q!=0);
    memcpy(&z,&bits,sizeof z);
    return z;
}
#endif
