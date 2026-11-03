#version 450
#extension GL_GOOGLE_include_directive : require
#define CH_NV_SHADER_GLSL 1
#include "../nv2a_fixed_shader.h"
#include "fixed_layout.glsl"
#include "attribute_decode.glsl"
layout(std430,set=0,binding=3) readonly buffer FixedParams { ChVkFixedParams p; } params;
layout(constant_id=0) const uint ff_specialized=0u;
layout(constant_id=1) const uint ff_format_0=0u;
layout(constant_id=2) const uint ff_format_1=0u;
layout(constant_id=3) const uint ff_format_2=0u;
layout(constant_id=4) const uint ff_format_3=0u;
layout(constant_id=5) const uint ff_format_4=0u;
layout(constant_id=6) const uint ff_format_5=0u;
layout(constant_id=7) const uint ff_format_6=0u;
layout(constant_id=8) const uint ff_format_7=0u;
layout(constant_id=9) const uint ff_skin_mode=0u;
layout(constant_id=10) const uint ff_use_composite=0u;
layout(constant_id=11) const uint ff_lighting=0u;
layout(constant_id=12) const uint ff_flip_normals=0u;
layout(constant_id=13) const uint ff_has_normal=0u;
layout(constant_id=14) const uint ff_has_color=0u;
layout(constant_id=15) const uint ff_colmat_amb=0u;
layout(constant_id=16) const uint ff_colmat_dif=0u;
layout(constant_id=17) const uint ff_texgen_on_0=0u;
layout(constant_id=18) const uint ff_texgen_on_1=0u;
layout(constant_id=19) const uint ff_texgen_on_2=0u;
layout(constant_id=20) const uint ff_texgen_on_3=0u;
uint ff_texgen_on(int slot) {
    switch(slot) {
    case 0:return ff_texgen_on_0;
    case 1:return ff_texgen_on_1;
    case 2:return ff_texgen_on_2;
    case 3:return ff_texgen_on_3;
    } return 0u;
}
layout(constant_id=21) const uint ff_texmat_on_0=0u;
layout(constant_id=22) const uint ff_texmat_on_1=0u;
layout(constant_id=23) const uint ff_texmat_on_2=0u;
layout(constant_id=24) const uint ff_texmat_on_3=0u;
uint ff_texmat_on(int slot) {
    switch(slot) {
    case 0:return ff_texmat_on_0;
    case 1:return ff_texmat_on_1;
    case 2:return ff_texmat_on_2;
    case 3:return ff_texmat_on_3;
    } return 0u;
}
uint ff_format(uint slot) {
    switch(slot) {
    case 0u:return ff_format_0;
    case 1u:return ff_format_1;
    case 2u:return ff_format_2;
    case 3u:return ff_format_3;
    case 4u:return ff_format_4;
    case 5u:return ff_format_5;
    case 6u:return ff_format_6;
    case 7u:return ff_format_7;
    } return 0u;
}
#define CH_ATTRIBUTE_FORMAT(slot) (ff_specialized!=0u?ff_format(slot):params.p.attribute_format[slot])
#include "attribute_stream.glsl"
vec4 a_pos;
vec4 a_wt;
vec4 a_nrm;
vec4 a_col;
vec4 a_t0;
vec4 a_t1;
vec4 a_t2;
vec4 a_t3;
layout(location=0) out vec4 v_col;
layout(location=1) out vec2 v_uv;
layout(location=2) out vec4 v_spec;
layout(location=3) out float v_fog;
layout(location=4) out vec4 tc[4];
#define v_tc0 tc[0]
#define v_tc1 tc[1]
#define v_tc2 tc[2]
#define v_tc3 tc[3]
vec4 o_screen;
mat4 u_comp,u_mv[4],u_proj,u_tm[4];
vec4 u_c59,u_samb,u_light[40],u_ascale[8];
uvec4 u_texgen[4];
int u_tgon[4],u_tmon[4],u_lkind[8],u_usecomp,u_lighting,u_flipn,u_hasnrm,u_hascol;
ivec2 u_colmat;
uint u_skin,u_fogmode;
int u_packed,u_bgra,u_fogen;
float u_matalpha;
vec2 u_aa,u_uvs0,u_texsize,u_fogp;
mat4 matrix(float m[16]) { return transpose(mat4(m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7],m[8],m[9],m[10],m[11],m[12],m[13],m[14],m[15])); }
void load_state() {
    u_comp=matrix(params.p.state.composite);u_proj=matrix(params.p.state.proj);
    for(int k=0;k<4;k++) {
        u_mv[k]=matrix(params.p.state.mv[k]);u_tm[k]=matrix(params.p.state.texmat[k]);
        u_texgen[k]=uvec4(params.p.state.texgen[k][0],params.p.state.texgen[k][1],params.p.state.texgen[k][2],params.p.state.texgen[k][3]);
        u_tgon[k]=int(ff_specialized!=0u?ff_texgen_on(k):params.p.state.texgen_on[k]);
        u_tmon[k]=int(ff_specialized!=0u?ff_texmat_on(k):params.p.state.texmat_on[k]);
    }
    u_c59=vec4(params.p.state.c59[0],params.p.state.c59[1],params.p.state.c59[2],params.p.state.c59[3]);
    u_skin=(ff_specialized!=0u?ff_skin_mode:params.p.state.skin_mode);u_usecomp=int((ff_specialized!=0u?ff_use_composite:params.p.state.use_composite));
    u_lighting=int((ff_specialized!=0u?ff_lighting:params.p.state.lighting));u_flipn=int((ff_specialized!=0u?ff_flip_normals:params.p.state.flip_normals));
    u_hasnrm=int((ff_specialized!=0u?ff_has_normal:params.p.state.has_normal));u_hascol=int((ff_specialized!=0u?ff_has_color:params.p.state.has_color));
    u_colmat=ivec2((ff_specialized!=0u?ff_colmat_amb:params.p.state.colmat_amb),(ff_specialized!=0u?ff_colmat_dif:params.p.state.colmat_dif));
    u_samb=vec4(params.p.state.scene_amb[0],params.p.state.scene_amb[1],params.p.state.scene_amb[2],params.p.state.scene_amb[3]);
    u_matalpha=params.p.state.mat_alpha;
    for(int k=0;k<8;k++) {
        u_lkind[k]=int(params.p.state.light_kind[k]);u_ascale[k]=vec4(1);
        for(int j=0;j<5;j++) for(int c=0;c<4;c++) u_light[k*5+j][c]=params.p.state.light[k][j*4+c];
    }
    u_packed=0;u_bgra=0; // Attributes have already been decoded, never transformed.
    u_aa=vec2(params.p.aa[0],params.p.aa[1]);
    u_uvs0=vec2(params.p.uv_scale0[0],params.p.uv_scale0[1]);u_texsize=vec2(params.p.texture[0],params.p.texture[1]);
    u_fogen=int(params.p.fog_enable);u_fogmode=params.p.fog_mode;u_fogp=vec2(params.p.fog_param[0],params.p.fog_param[1]);
}
CH_NV_FIXED_SHADER_FUNCTIONS
#include "immersive.glsl"
void main() {
    a_pos=ch_attribute(ch_stream_attribute(0u,32u),CH_ATTRIBUTE_FORMAT(0u));
    a_wt=ch_attribute(ch_stream_attribute(1u,32u),CH_ATTRIBUTE_FORMAT(1u));
    a_nrm=ch_attribute(ch_stream_attribute(2u,32u),CH_ATTRIBUTE_FORMAT(2u));
    a_col=ch_attribute(ch_stream_attribute(3u,32u),CH_ATTRIBUTE_FORMAT(3u));
    a_t0=ch_attribute(ch_stream_attribute(4u,32u),CH_ATTRIBUTE_FORMAT(4u));
    a_t1=ch_attribute(ch_stream_attribute(5u,32u),CH_ATTRIBUTE_FORMAT(5u));
    a_t2=ch_attribute(ch_stream_attribute(6u,32u),CH_ATTRIBUTE_FORMAT(6u));
    a_t3=ch_attribute(ch_stream_attribute(7u,32u),CH_ATTRIBUTE_FORMAT(7u));
    load_state();ch_nv_fixed_vertex();
    vec3 s=o_screen.xyz;float w=o_screen.w;
    bool bad=abs(w)<=1e-6 || any(isnan(o_screen)) || any(isinf(o_screen));
    vec2 ndc=s.xy/vec2(params.p.surface[0],params.p.surface[1])*2-1;
    float zn=(s.z-params.p.depth[0])/(params.p.depth[1]-params.p.depth[0]);
    vec4 guest_clip=vec4(ndc*w,zn*w,w);float side=1;
    if(params.p.imm_mode!=0u && !bad) guest_clip=ch_imm_position(s,w,guest_clip,side);
    gl_Position=bad?vec4(0,0,0,1):guest_clip;
    gl_ClipDistance[2]=bad?-1e30:side;
    gl_ClipDistance[0]=bad?-1e30:w-1e-3;
    gl_ClipDistance[1]=bad?-1e30:params.p.near_z!=0u?(s.z-params.p.vp_off_z)*w:1;
    v_tc0.xy*=vec2(params.p.stage_scale[0][0],params.p.stage_scale[0][1]);
    v_tc1.xy*=vec2(params.p.stage_scale[1][0],params.p.stage_scale[1][1]);
    v_tc2.xy*=vec2(params.p.stage_scale[2][0],params.p.stage_scale[2][1]);
    v_tc3.xy*=vec2(params.p.stage_scale[3][0],params.p.stage_scale[3][1]);
    ch_imm_texcoords(v_uv,tc);
}
