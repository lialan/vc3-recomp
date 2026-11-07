// Loops with a fixed count, unrolled when the shader is specialized
// (fixed.vert defines CH_UNROLL as [[unroll]]): SPIR-V Tools unrolls only
// loops marked so, and the constants it folds are in them.
#ifndef CH_UNROLL
#define CH_UNROLL
#endif
// Format zero is a float4 snapshot. Encoded arrays retain their original
// bytes; sign extension and NV2A normalization execute once per GPU vertex.
vec4 ch_attribute(uvec4 bits,uint format) {
    if(format==0u) return uintBitsToFloat(bits);
    uint type=(format&255u)-1u,size=format>>8;
    vec4 v=vec4(0,0,0,1);
    if(type==0u) return vec4((bits.x>>16)&255u,(bits.x>>8)&255u,bits.x&255u,bits.x>>24)/255.0;
    if(type==6u) return vec4(float(int(bits.x<<21)>>21)/1023.0,float(int(bits.x<<10)>>21)/1023.0,float(int(bits.x)>>22)/511.0,1);
    CH_UNROLL for(uint i=0u;i<size;i++) {
        if(type==2u) v[i]=uintBitsToFloat(bits[i]);
        else if(type==4u) v[i]=float((bits.x>>(i*8u))&255u)/255.0;
        else if(type==1u || type==5u) {
            int n=int(((bits[i/2u]>>((i&1u)*16u))&65535u)<<16)>>16;
            v[i]=type==1u?float(n)/32767.0:float(n);
        }
    }
    return v;
}
