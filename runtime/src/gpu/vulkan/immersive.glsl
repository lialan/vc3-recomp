// Immersive VR (modes 1, 2) and flat 16:9 (mode 3, below).
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
    uint m=params.p.imm_sbs&31u;
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
// A 3D draw sampling a surface that has eyes (imm_sbs bits 8-11: stage i,
// bit 12: the single-texture uv) -- VC3's water: the scene behind it,
// sampled at its screen position plus a ripple. The game's coordinate is
// where the vertex is on the game's screen (s) plus an offset; in each eye
// the lookup is where the vertex is in that eye's picture (c, the eye clip
// position, in the side-by-side target: projective, exact in perspective)
// plus the same offset. A coordinate far from the screen position is a
// texture mapped onto the geometry (a monitor showing a picture): that
// eye's half, as for 2D.
void ch_imm_screen_texcoords(vec3 s,vec4 c,inout vec2 uv,inout vec4 t[4]) {
    uint m=(params.p.imm_sbs>>8)&31u;
    if(m==0u || params.p.imm_mode!=1u) return;
    float k=params.p.imm_scale;
    vec2 gp=s.xy/vec2(params.p.surface[0],params.p.surface[1]);
    vec2 p=vec2(c.x,c.y)*.5+.5*c.w;      // target-normalized position, times w
    float e=float(gl_InstanceIndex&1),off=e*(1.-.5*k);
    for(int i=0;i<4;i++) if((m&(1u<<uint(i)))!=0u) {
        float q=t[i].w==0?1:t[i].w;
        vec2 d=t[i].xy/q-gp;
        if(all(lessThan(abs(d),vec2(.25)))) {
            t[i].xy=p+vec2(d.x*.5*k,d.y*k)*c.w;
            t[i].z=t[i].z/q*c.w;t[i].w=c.w;
        } else {t[i].x=t[i].x*.5*k+off*q;t[i].y*=k;}
    }
    if((m&16u)!=0u) {
        vec2 d=uv-gp;
        if(all(lessThan(abs(d),vec2(.25)))) uv=p/c.w+vec2(d.x*.5*k,d.y*k);
        else {uv.x=uv.x*.5*k+off;uv.y*=k;}
    }
}
// Mode 1 with imm_sbs bit 16 (the profile's vr.world_behind): the world
// behind the game's camera too. The guest's own clip planes (clip w > 0, its
// near plane) are not applied, only each eye's view: the game-camera
// position back from s and w below is exact for any w but 0.
bool ch_imm_behind() {return params.p.imm_mode==1u && (params.p.imm_sbs&0x10000u)!=0u;}
vec4 ch_imm_position(vec3 s,float w,vec4 guest_clip,out float side,out float low) {
    uint mode=params.p.imm_mode;
    int eye=gl_InstanceIndex&1;
    vec4 c=guest_clip;
    // Mode 3, flat 16:9 (vk_wide.c): one instance into a wider target; clip
    // x times imm_scale plus imm_gp[0] (the guest's picture 1:1 in the
    // middle, the view continuing to the sides; or 1, 0: stretched).
    if(mode==3u) {
        side=1.;low=1.;
        c.x=c.x*params.p.imm_scale+params.p.imm_gp[0]*c.w;
        return c;
    }
    if(mode==1u) {
        float ipz=params.p.imm_gp[4],ez=w*ipz;
        vec3 v=vec3((s.x-params.p.imm_gp[2])/params.p.imm_gp[0]*ez,
                    (s.y-params.p.imm_gp[3])/params.p.imm_gp[1]*ez,-w*abs(ipz));
        // both eyes' matrices at fixed places, then the instance's: a
        // matrix indexed by the instance was read from memory per vertex
        mat4 m;
        for(int k=0;k<4;k++) {
            vec4 l=vec4(params.p.imm_vp[0][k*4],params.p.imm_vp[0][k*4+1],params.p.imm_vp[0][k*4+2],params.p.imm_vp[0][k*4+3]);
            vec4 r=vec4(params.p.imm_vp[1][k*4],params.p.imm_vp[1][k*4+1],params.p.imm_vp[1][k*4+2],params.p.imm_vp[1][k*4+3]);
            m[k]=eye==0?l:r;
        }
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
