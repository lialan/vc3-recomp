// Immersive VR: one draw, two instances, the eyes side by side in one target
// (instance = eye). Mode 1, 3D: the game-camera position back from the
// vertex's screen position s (surface pixels) and w through the game's own
// projection (vr_math.h, vr_unproject_gl), then through the eye's matrix.
// Mode 2, 2D: the guest clip position as drawn, in each eye's half. Each
// eye's clip x is squeezed into its half; gl_ClipDistance[2] keeps a
// triangle out of the other eye's half. Requires params.p (ChVkFixedParams).
void ch_imm_texcoords(inout vec2 uv,inout vec4 t[4]) {
    uint m=params.p.imm_sbs;
    if(m==0u) return;
    // mode 0 (a guest draw sampling eyes, e.g. an effect map): the left eye
    float e=params.p.imm_mode==0u?0.:float(gl_InstanceIndex&1)*.5;
    for(int i=0;i<4;i++) if((m&(1u<<uint(i)))!=0u) t[i].x=t[i].x*.5+e*(t[i].w==0?1:t[i].w);
    if((m&16u)!=0u) uv.x=uv.x*.5+e;
}
vec4 ch_imm_position(vec3 s,float w,vec4 guest_clip,out float side) {
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
    c.x=c.x*.5+(eye==0?-.5:.5)*c.w;
    side=eye==0?-c.x:c.x;
    return c;
}
