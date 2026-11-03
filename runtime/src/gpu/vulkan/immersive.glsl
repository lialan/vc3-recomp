// Immersive VR: one draw, two instances, the eyes side by side in one target
// (instance = eye). Mode 1, 3D: the game-camera position back from the
// vertex's screen position s (surface pixels) and w through the game's own
// projection (vr_math.h, vr_unproject_gl), then through the eye's matrix.
// Mode 2, 2D: the guest clip position as drawn, in each eye's half. Each
// eye's clip x is squeezed into its half; gl_ClipDistance[2] keeps a
// triangle out of the other eye's half. Requires params.p (ChVkFixedParams).
// Dynamic resolution (imm_scale k): each eye's picture fills k of its half,
// the left eye's from the top-left corner, the right eye's from the
// top-right; gl_ClipDistance[2] (side) and [3] (low) keep everything out of
// the rest, which the headset never shows.
void ch_imm_texcoords(inout vec2 uv,inout vec4 t[4]) {
    uint m=params.p.imm_sbs;
    if(m==0u) return;
    float k=params.p.imm_scale;
    // mode 0 (a guest draw sampling eyes, e.g. an effect map): the left eye
    float e=params.p.imm_mode==0u?0.:float(gl_InstanceIndex&1);
    float off=e*(1.-.5*k);
    for(int i=0;i<4;i++) if((m&(1u<<uint(i)))!=0u) {
        float q=t[i].w==0?1:t[i].w;
        t[i].x=t[i].x*.5*k+off*q;t[i].y*=k;
    }
    if((m&16u)!=0u) {uv.x=uv.x*.5*k+off;uv.y*=k;}
}
vec4 ch_imm_position(vec3 s,float w,vec4 guest_clip,out float side,out float low) {
    uint mode=params.p.imm_mode;
    int eye=gl_InstanceIndex&1;
    vec4 c=guest_clip;
    if(mode==1u) {
        float ipz=params.p.imm_gp[4],ez=w*ipz;
        vec3 v=vec3((s.x-params.p.imm_gp[2])/params.p.imm_gp[0]*ez,
                    (s.y-params.p.imm_gp[3])/params.p.imm_gp[1]*ez,-w*abs(ipz));
        mat4 m;
        for(int k=0;k<4;k++)
            m[k]=vec4(params.p.imm_vp[eye][k*4],params.p.imm_vp[eye][k*4+1],
                      params.p.imm_vp[eye][k*4+2],params.p.imm_vp[eye][k*4+3]);
        c=m*vec4(v,1);
        // The surfaces hold the picture top row first: screen y down.
        c.y=-c.y;
        c.z=(c.z+c.w)*.5;   // GL clip depth to Vulkan's [0, w]
    }
    float k=params.p.imm_scale;
    c.x=c.x*.5*k+(eye==0?.5*k-1.:1.-.5*k)*c.w;
    side=eye==0?(k-1.)*c.w-c.x:c.x-(1.-k)*c.w;
    c.y=c.y*k+(k-1.)*c.w;
    low=k<1.?(2.*k-1.)*c.w-c.y:1.;
    return c;
}
