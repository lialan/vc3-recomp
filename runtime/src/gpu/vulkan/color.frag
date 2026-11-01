#version 450
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
#include "combiner.glsl"
layout(std430,set=0,binding=1) readonly buffer Combiner { ChRc rc; uint pad[2]; vec4 fog_color; uvec4 stage_on; } params;
layout(set=0,binding=0) uniform sampler2D stage_images[4];
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
    vec4 texels[4];
    for(uint i=0;i<4;i++) {
        uint mode=(params.rc.stage_program>>(i*5))&31u;
        texels[i]=vec4(0,0,0,1);
        if(mode==4) texels[i]=clamp(tc[i],0,1);
        else if(mode>=1 && mode<=3) {
            float q=mode==3 || tc[i].w==0?1:tc[i].w;
            vec2 uv=tc[i].xy/q;
            if(params.stage_on[i]==0) texels[i]=vec4(1);
            else if(i==0) texels[i]=texture(stage_images[0],uv);
            else if(i==1) texels[i]=texture(stage_images[1],uv);
            else if(i==2) texels[i]=texture(stage_images[2],uv);
            else texels[i]=texture(stage_images[3],uv);
        }
    }
    shaded=ch_rc_eval(params.rc,diffuse,spec,vec4(params.fog_color.rgb,fog),texels);
#endif
#ifdef EFFECT
    if(alpha.effect_kind==1) {
        float d=length(uv),a=alpha.effect_shape.x<0?1:
            alpha.effect_shape.y>0?clamp(1-abs(d-alpha.effect_shape.x)/alpha.effect_shape.y,0,1):clamp(1-d,0,1);
        a=alpha.effect_color.a*(alpha.effect_shape.x<0?1:a*a);
        shaded=vec4(alpha.effect_color.rgb*a,a);
    } else {
        float d=length(uv-alpha.effect_reticle.xy);
        float dot_a=1-smoothstep(alpha.effect_shape.x*.55,alpha.effect_shape.x,d);
        float ring_a=alpha.effect_shape.y>0?1-smoothstep(alpha.effect_shape.z*.4,alpha.effect_shape.z,abs(d-alpha.effect_shape.y)):0;
        float a=max(dot_a,ring_a)*alpha.effect_color.a;
        float ta=alpha.effect_tint.a*(1-a);
        shaded=vec4(alpha.effect_color.rgb*a+alpha.effect_tint.rgb*ta,a+ta);
    }
#endif
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
    color=shaded;
}
