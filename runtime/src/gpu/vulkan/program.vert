#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_control_flow_attributes : require
#include "fixed_layout.glsl"
#include "attribute_decode.glsl"
layout(std430,set=0,binding=3) uniform RawParams {ChVkFixedParams p;} params;   // uniform: constant registers
#include "vertex_program.glsl"
#define CH_ATTRIBUTE_FORMAT(slot) (vp_fetch_specialized!=0u?vp_attribute_format(slot):params.p.attribute_format[slot])
#include "attribute_stream.glsl"
layout(location=0) out vec4 v_col;
layout(location=1) out vec2 v_uv;
layout(location=2) out vec4 v_spec;
layout(location=3) out float v_fog;
layout(location=4) out vec4 tc[4];
#include "program.glsl"
#include "immersive.glsl"
float fog_factor(float d) {
    if(params.p.fog_enable==0u) return 1;
    float px=params.p.fog_param[0],py=params.p.fog_param[1],f;
    uint mode=params.p.fog_mode;
    if(mode==0x800u || mode==0x802u) f=px+exp2(d*py*16)-1.5;
    else if(mode==0x801u || mode==0x803u) f=px+exp2(-d*d*py*py*32)-1.5;
    else f=px+d*py-1;
    if(mode==0x802u || mode==0x803u || mode==0x804u) f=abs(f);
    return isnan(f)?1:clamp(f,0,1);
}
void main() {
    vec4 attributes[16];
    uint used=vp_fetch_specialized!=0u?vp_input_mask:0xffffu;
    [[unroll]] for(uint i=0u;i<16u;i++) {
        attributes[i]=vec4(0);
        if((used&(1u<<i))!=0u) attributes[i]=ch_attribute(ch_stream_attribute(i,64u),CH_ATTRIBUTE_FORMAT(i));
    }
    vec4 pos,O[13];vp_run(attributes,pos,O);
    if(floatBitsToUint(pos.w)==0u) pos.w=1;
    vec3 s=vec3(pos.xy*vec2(params.p.aa[0],params.p.aa[1]),pos.z);float w=pos.w;
    bool bad=abs(w)<=1e-6 || any(isnan(pos)) || any(isinf(pos));
    vec2 ndc=s.xy/vec2(params.p.surface[0],params.p.surface[1])*2-1;
    float zn=(s.z-params.p.depth[0])/(params.p.depth[1]-params.p.depth[0]);
    vec4 guest_clip=vec4(ndc*w,zn*w,w);float side=1,low=1;
    if(params.p.imm_mode!=0u && !bad) guest_clip=ch_imm_position(s,w,guest_clip,side,low);
    gl_Position=bad?vec4(0,0,0,1):guest_clip;
    gl_ClipDistance[2]=bad?-1e30:side;
    gl_ClipDistance[3]=bad?-1e30:low;
    gl_ClipDistance[0]=bad?-1e30:w-1e-3;
    gl_ClipDistance[1]=bad?-1e30:params.p.near_z!=0u?(s.z-params.p.vp_off_z)*w:1;
    v_col=vp_color(O[3]);v_spec=vp_color(O[4]);v_fog=fog_factor(O[5].x);
    v_uv=O[9].xy*vec2(params.p.uv_scale0[0],params.p.uv_scale0[1])/vec2(params.p.texture[0],params.p.texture[1]);
    for(int i=0;i<4;i++) {tc[i]=O[9+i];tc[i].xy*=vec2(params.p.stage_scale[i][0],params.p.stage_scale[i][1]);}
    ch_imm_texcoords(v_uv,tc);
}
