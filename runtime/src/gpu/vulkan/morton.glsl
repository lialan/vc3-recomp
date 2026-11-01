#ifndef CH_MORTON_GLSL
#define CH_MORTON_GLSL
// Full rectangular power-of-two allocations; axes are at most 2^15.
uint ch_spread_bits(uint v) {
    v=(v|(v<<8u))&0x00ff00ffu;
    v=(v|(v<<4u))&0x0f0f0f0fu;
    v=(v|(v<<2u))&0x33333333u;
    return (v|(v<<1u))&0x55555555u;
}
uint ch_morton_index(uint x,uint y,uint width,uint height) {
    uint small_axis=min(width,height),bits=uint(findMSB(small_axis)),mask=small_axis-1u;
    uint low=ch_spread_bits(x&mask)|(ch_spread_bits(y&mask)<<1u);
    uint tail=(width>height?x:y)>>bits;
    return low|(tail<<(2u*bits));
}
#endif
