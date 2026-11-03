// Shared NV2A vertex-program arithmetic. Words and instruction count are
// pipeline specialization constants; c[] remains per-draw state. CXT_WRITE
// programs must be rejected by the executor before reaching this path.
#include "vertex_program.glsl"
vec4 vp_constant(int index) {
    if(index<0 || index>=192) index=0;
    return vec4(params.p.constants[index][0],params.p.constants[index][1],params.p.constants[index][2],params.p.constants[index][3]);
}
vec4 vp_source(uint mux,uint reg,uint swz,uint neg,uint input_reg,int ci,vec4 v[16],vec4 R[13]) {
    vec4 x=mux==1u?R[reg<=12u?reg:0u]:mux==2u?v[input_reg]:vp_constant(ci);
    x=vec4(x[(swz>>6)&3u],x[(swz>>4)&3u],x[(swz>>2)&3u],x[swz&3u]);
    return neg!=0u?-x:x;
}
void vp_write(inout vec4 dst,vec4 src,uint mask) {
    if((mask&8u)!=0u) dst.x=src.x;
    if((mask&4u)!=0u) dst.y=src.y;
    if((mask&2u)!=0u) dst.z=src.z;
    if((mask&1u)!=0u) dst.w=src.w;
}
vec4 vp_ilu(uint op,vec4 c) {
    float x=c.x,inf=uintBitsToFloat(0x7f800000u);
    if(op==1u) return c;
    if(op==2u) return vec4(x==0?inf:1/x);
    if(op==3u) {
        float f=x==0?uintBitsToFloat((floatBitsToUint(x)&0x80000000u)|0x7f800000u):1/x;
        if(abs(f)<5.42101e-20) f=f<0?-5.42101e-20:5.42101e-20;
        if(abs(f)>1.884467e19) f=f<0?-1.884467e19:1.884467e19;
        return vec4(f);
    }
    if(op==4u) return vec4(x==0?inf:1/sqrt(abs(x)));
    if(op==5u) {float fl=floor(x);return vec4(exp2(fl),x-fl,exp2(x),1);}
    if(op==6u) {
        float ax=abs(x);if(ax==0) return vec4(-inf,1,-inf,1);
        float e=floor(log2(ax));return vec4(e,ax/exp2(e),log2(ax),1);
    }
    if(op==7u) {
        float nl=c.x>0?c.x:0,nh=c.y>0?c.y:0;
        float p=c.w<-127.9961?-127.9961:c.w>127.9961?127.9961:c.w;
        float s=p==0 || nh==1?1:isnan(p)?p:nh>0?pow(nh,p):p>0?0:inf;
        return vec4(1,nl,c.x>0?s:0,1);
    }
    return vec4(0);
}
void vp_step(uvec4 ins,vec4 v[16],inout vec4 R[13],inout vec4 O[13],inout int a0) {
    uint y=ins.y,z=ins.z,w=ins.w;
        uint mac=(y>>21)&15u,ilu=(y>>25)&7u,td=(w>>20)&15u;
        int ci=int((y>>13)&255u)+((w&2u)!=0u?a0:0);
        uint input_reg=(y>>9)&15u;
        vec4 a=vp_source((z>>26)&3u,z>>28,y&255u,(y>>8)&1u,input_reg,ci,v,R);
        vec4 b=vp_source((z>>11)&3u,(z>>13)&15u,(z>>17)&255u,(z>>25)&1u,input_reg,ci,v,R);
        vec4 c=vp_source((w>>28)&3u,((z&3u)<<2)|(w>>30),(z>>2)&255u,(z>>10)&1u,input_reg,ci,v,R);
        precise vec4 m=vec4(0);
        switch(mac) {
        case 0u:break;
        case 1u:m=a;break;
        case 2u:m=a*b;break;
        case 3u:m=a+c;break;
        case 4u:m=a*b+c;break;
        case 5u:m=vec4(a.x*b.x+a.y*b.y+a.z*b.z);break;
        case 6u:m=vec4(a.x*b.x+a.y*b.y+a.z*b.z+b.w);break;
        case 7u:m=vec4(a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w);break;
        case 8u:m=vec4(1,a.y*b.y,a.z,b.w);break;
        case 9u:m=mix(b,a,lessThan(a,b));break;
        case 10u:m=mix(b,a,greaterThanEqual(a,b));break;
        case 11u:m=vec4(lessThan(a,b));break;
        case 12u:m=vec4(greaterThanEqual(a,b));break;
        case 13u:a0=int(floor(a.x+0.001));m=a;break;
        default:m=a;break;
        }
        vec4 ir=vp_ilu(ilu,c);
        if(mac!=0u && mac!=13u && td<=12u) vp_write(R[td],m,(w>>24)&15u);
        uint it=mac!=0u?1u:td;
        if(ilu!=0u && it<=12u) vp_write(R[it],ir,(w>>16)&15u);
        uint oa=(w>>3)&255u;
        if((w&2048u)!=0u) {
            vec4 value=(w&4u)!=0u?ir:m;
            if(oa==0u) vp_write(R[12],value,(w>>12)&15u);
            else if(oa<13u) vp_write(O[oa],value,(w>>12)&15u);
        }
}
void vp_run(vec4 v[16],out vec4 pos,out vec4 O[13]) {
    vec4 R[13];for(int i=0;i<13;i++) {R[i]=vec4(0);O[i]=vec4(0);}
    O[3].w=O[4].w=O[9].w=O[10].w=O[11].w=O[12].w=1;
    int a0=0;
#ifdef CH_VP_LOOP
    // The words from the parameters, one vp_step in the module (Qualcomm's
    // proprietary compiler did not finish the 136 specialized steps).
    [[dont_unroll]] for(uint i=0u;i<params.p.program_count && i<136u;i++)
        vp_step(uvec4(params.p.program[i][0],params.p.program[i][1],params.p.program[i][2],params.p.program[i][3]),v,R,O,a0);
#else
#include "vertex_steps.glsl"
#endif
    pos=R[12];
}
vec4 vp_color(vec4 c) {
    return mix(mix(vec4(0),c,greaterThanEqual(c,vec4(0))),vec4(1),greaterThan(c,vec4(1)));
}
