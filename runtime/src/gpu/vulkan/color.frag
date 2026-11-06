#version 450
#extension GL_EXT_scalar_block_layout : require
#ifdef DEPTH_QUANT_FRAGMENT
#extension GL_GOOGLE_include_directive : require
#include "depth_quant.glsl"
#endif
layout(location=0) in vec4 diffuse;
layout(location=0) out vec4 color;
layout(push_constant) uniform Alpha { uint enabled; uint func; uint reference; uint effect_kind; vec4 effect_color; vec4 effect_shape; vec4 effect_tint; vec4 effect_reticle; } alpha;
#ifdef TEXTURED
layout(set=0,binding=0) uniform sampler2D image0;
layout(location=1) in vec2 uv;
#endif
#ifdef EFFECT
layout(location=1) in vec2 uv;
#endif
#ifdef COMBINER
#extension GL_GOOGLE_include_directive : require
#ifdef SPECIALIZED
#extension GL_EXT_control_flow_attributes : require
#define CH_RC_UNROLL [[unroll]]
#else
#define CH_RC_UNROLL
#endif
#include "combiner.glsl"
#ifdef SPECIALIZED
#include "combiner_program.glsl"
layout(constant_id=100) const uint ch_sample_static=0u;
layout(constant_id=101) const uint ch_sample_types=0u;
#endif
#define CH_NV_SHADER_GLSL 1
#include "../nv2a_stage_fetch.h"
CH_NV_STAGE_FETCH_FUNCTION
layout(std430,set=0,binding=1) uniform Combiner { ChRc rc; uint pad[2]; vec4 fog_color; uvec4 stage_on; } params;   // uniform: constant registers
layout(set=0,binding=0) uniform sampler2D stage_images[4];
layout(set=0,binding=2) uniform samplerCube cube_images[4];
layout(location=2) in vec4 spec;
layout(location=3) in float fog;
layout(location=4) in vec4 tc[4];
#endif
void main() {
    vec4 shaded=diffuse;
#ifdef TEXTURED
    shaded*=texture(image0,uv);
#endif
#ifdef COMBINER
    ChRc program=params.rc;
#ifdef SPECIALIZED
    program=ch_rc_program(program);
#endif
    uvec4 stage_on=params.stage_on;
#ifdef SPECIALIZED
    if(ch_sample_static!=0u) stage_on=uvec4(ch_sample_types&3u,(ch_sample_types>>2)&3u,
        (ch_sample_types>>4)&3u,(ch_sample_types>>6)&3u);
#endif
    vec4 texels[4];
    CH_RC_UNROLL
    for(uint i=0;i<4;i++) {
        uint mode=(program.stage_program>>(i*5))&31u;
        texels[i]=vec4(1);
        if(mode>=1 && mode<=3) {
            float q=mode==3 || tc[i].w==0?1:tc[i].w;
            vec2 uv=tc[i].xy/q;
            if(stage_on[i]==0) texels[i]=vec4(1);
            else if((stage_on[i]&2u)!=0) {
                if(i==0) texels[i]=texture(cube_images[0],tc[i].xyz);
                else if(i==1) texels[i]=texture(cube_images[1],tc[i].xyz);
                else if(i==2) texels[i]=texture(cube_images[2],tc[i].xyz);
                else texels[i]=texture(cube_images[3],tc[i].xyz);
            }
            else if(i==0) texels[i]=texture(stage_images[0],uv);
            else if(i==1) texels[i]=texture(stage_images[1],uv);
            else if(i==2) texels[i]=texture(stage_images[2],uv);
            else texels[i]=texture(stage_images[3],uv);
        }
        texels[i]=ch_nv_stage_fetch(mode,tc[i],texels[i],stage_on[i]);
    }
    shaded=ch_rc_eval(program,diffuse,spec,vec4(params.fog_color.rgb,fog),texels);
#endif
#ifdef EFFECT
    if(alpha.effect_kind==1) {
        float d=length(uv),a=alpha.effect_shape.x<0?1:
            alpha.effect_shape.y>0?clamp(1-abs(d-alpha.effect_shape.x)/alpha.effect_shape.y,0,1):clamp(1-d,0,1);
        a=alpha.effect_color.a*(alpha.effect_shape.x<0?1:a*a);
        shaded=vec4(alpha.effect_color.rgb*a,a);
    } else if(alpha.effect_kind==3) {
        /* a dot: a disc of radius shape.x (the quad's half size 1) in
         * effect_color, tint.rgb at its centre fading to it by shape.y, in
         * a dark rim out to 1 (tint.a its opacity); edges a pixel soft */
        float d=length(uv),aa=max(fwidth(d),1e-4);
        float disc=(1-smoothstep(alpha.effect_shape.x-aa,alpha.effect_shape.x+aa,d))*alpha.effect_color.a;
        float rim=(1-smoothstep(1-2*aa,1,d))*alpha.effect_tint.a;
        vec3 c=mix(alpha.effect_tint.rgb,alpha.effect_color.rgb,smoothstep(0,alpha.effect_shape.y,d));
        shaded=vec4(c*disc,disc+(1-disc)*rim);
    } else {
        /* bare glass: rounded top corners, tint, sheen and a bright rim;
         * nothing outside the outline, so the reticle is clipped to it */
        vec4 box=vec4(-alpha.effect_shape.w,alpha.effect_reticle.z,alpha.effect_shape.w,alpha.effect_reticle.w);
        vec2 mid=.5*(box.xy+box.zw),hs=.5*(box.zw-box.xy);
        float rad=uv.y>mid.y?.0035:.0008;
        vec2 q=abs(uv-mid)-hs+rad;
        float sd=length(max(q,0))+min(max(q.x,q.y),0)-rad;
        float aa=max(fwidth(sd),1e-5);
        float cov=1-smoothstep(-aa,aa,sd);
        if(cov<=0) discard;
        float d=length(uv-alpha.effect_reticle.xy);
        float dot_a=1-smoothstep(alpha.effect_shape.x*.55,alpha.effect_shape.x,d);
        float ring_a=alpha.effect_shape.y>0?1-smoothstep(alpha.effect_shape.z*.4,alpha.effect_shape.z,abs(d-alpha.effect_shape.y)):0;
        float a=max(dot_a,ring_a)*alpha.effect_color.a;
        vec2 u=(uv-box.xy)/(box.zw-box.xy);
        float sheen=exp(-pow((u.x+u.y*.6-.95)*7,2))*.07;
        float rim=(1-smoothstep(0,.0006,-sd))*.30;
        float ta=alpha.effect_tint.a*(.8+.4*(1-u.y))+sheen+rim;
        vec3 tc=(alpha.effect_tint.rgb*alpha.effect_tint.a+vec3(.92,.96,1)*(sheen+rim))/ta;
        ta=min(ta,.6)*(1-a);
        shaded=vec4(alpha.effect_color.rgb*a+tc*ta,a+ta)*cov;
    }
#endif
#ifndef NOAT
    if(alpha.enabled != 0) {
        float a=floor(clamp(shaded.a,0,1)*255.0+0.5);
        float r=float(alpha.reference);
        bool pass=false;
        switch(alpha.func) {
        case 0x200: pass=false; break;
        case 0x201: pass=a<r; break;
        case 0x202: pass=a==r; break;
        case 0x203: pass=a<=r; break;
        case 0x204: pass=a>r; break;
        case 0x205: pass=a!=r; break;
        case 0x206: pass=a>=r; break;
        case 0x207: pass=true; break;
        }
        if(!pass) discard;
    }
#endif
#ifdef DEPTH_QUANT_FRAGMENT
    // Native D24 reference conversion: binary32 scaling, nearest-even integer.
    // Distinct from the legacy CPU double-precision post-draw pack contract.
    precise float scaled_depth=clamp(gl_FragCoord.z,0.0,1.0)*16777215.0;
    gl_FragDepth=ch_depth_unpack(uint(roundEven(scaled_depth)));
#endif
    color=shaded;
}
