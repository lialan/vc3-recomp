#version 450
#extension GL_GOOGLE_include_directive : require
#define CH_NV_SHADER_GLSL 1
#include "../nv2a_fixed_shader.h"
#include "fixed_layout.glsl"
#include "attribute_decode.glsl"
layout(std430,set=0,binding=3) readonly buffer FixedParams { ChVkFixedParams p; } params;
layout(location=0) in uvec4 a_pos_bits;
vec4 a_pos;
layout(location=1) in uvec4 a_wt_bits;
vec4 a_wt;
layout(location=2) in uvec4 a_nrm_bits;
vec4 a_nrm;
layout(location=3) in uvec4 a_col_bits;
vec4 a_col;
layout(location=4) in uvec4 a_t0_bits;
vec4 a_t0;
layout(location=5) in uvec4 a_t1_bits;
vec4 a_t1;
layout(location=6) in uvec4 a_t2_bits;
vec4 a_t2;
layout(location=7) in uvec4 a_t3_bits;
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
        u_tgon[k]=int(params.p.state.texgen_on[k]);u_tmon[k]=int(params.p.state.texmat_on[k]);
    }
    u_c59=vec4(params.p.state.c59[0],params.p.state.c59[1],params.p.state.c59[2],params.p.state.c59[3]);
    u_skin=params.p.state.skin_mode;u_usecomp=int(params.p.state.use_composite);
    u_lighting=int(params.p.state.lighting);u_flipn=int(params.p.state.flip_normals);
    u_hasnrm=int(params.p.state.has_normal);u_hascol=int(params.p.state.has_color);
    u_colmat=ivec2(params.p.state.colmat_amb,params.p.state.colmat_dif);
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
void main() {
    a_pos=ch_attribute(a_pos_bits,params.p.attribute_format[0]);
    a_wt=ch_attribute(a_wt_bits,params.p.attribute_format[1]);
    a_nrm=ch_attribute(a_nrm_bits,params.p.attribute_format[2]);
    a_col=ch_attribute(a_col_bits,params.p.attribute_format[3]);
    a_t0=ch_attribute(a_t0_bits,params.p.attribute_format[4]);
    a_t1=ch_attribute(a_t1_bits,params.p.attribute_format[5]);
    a_t2=ch_attribute(a_t2_bits,params.p.attribute_format[6]);
    a_t3=ch_attribute(a_t3_bits,params.p.attribute_format[7]);
    load_state();ch_nv_fixed_vertex();
    vec3 s=o_screen.xyz;float w=o_screen.w;
    bool bad=abs(w)<=1e-6 || any(isnan(o_screen)) || any(isinf(o_screen));
    vec2 ndc=s.xy/vec2(params.p.surface[0],params.p.surface[1])*2-1;
    float zn=(s.z-params.p.depth[0])/(params.p.depth[1]-params.p.depth[0]);
    gl_Position=bad?vec4(0,0,0,1):vec4(ndc*w,zn*w,w);
    gl_ClipDistance[0]=bad?-1e30:w-1e-3;
    gl_ClipDistance[1]=bad?-1e30:params.p.near_z!=0u?(s.z-params.p.vp_off_z)*w:1;
    v_tc0.xy*=vec2(params.p.stage_scale[0][0],params.p.stage_scale[0][1]);
    v_tc1.xy*=vec2(params.p.stage_scale[1][0],params.p.stage_scale[1][1]);
    v_tc2.xy*=vec2(params.p.stage_scale[2][0],params.p.stage_scale[2][1]);
    v_tc3.xy*=vec2(params.p.stage_scale[3][0],params.p.stage_scale[3][1]);
}
