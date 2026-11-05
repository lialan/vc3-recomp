#include "vk_overlay.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "../../xr/quest_gun_geometry.inc"
#include "../../xr/quest_rifle_geometry.inc"
#include "../../xr/quest_overlay_math.inc"
#include "../../xr/quest_overlay_effects.inc"
/* The rifle into the shared mesh (all of it rides the controller; no slide),
 * its scope at scale s (QoFrame.scope_scale). */
#define RIFLE_MESH (-2)
static float rifle_scale;
static float scope_scale(const QoFrame *f) {return f->scope_scale>0?f->scope_scale:1.f;}
static void build_rifle(float s) {
    int n=0;
    Box scope[RIFLE_SCOPE_N]; rifle_scope(s,scope);
    ADD(&n,RIFLE_BODY); add_boxes(&n,scope,RIFLE_SCOPE_N);
    slide_verts=0; gun_verts=n; cur_sight=RIFLE_MESH; rifle_scale=s;
}
VkResult ch_vk_overlay_gun_read(ChVkTarget *target,const QoFrame *f,const float vp[16],void *pixels,VkDeviceSize bytes) {
    if(!target || !f || !vp || !pixels) return VK_ERROR_INITIALIZATION_FAILED;
    if(!f->gun_valid || !f->show_gun) return ch_vk_target_clear_aspects_read(target,0,NULL,0,0,pixels,bytes);
    for(unsigned i=0;i<16;i++) if(!isfinite(vp[i])) return VK_ERROR_INITIALIZATION_FAILED;
    float norm=0;
    for(unsigned i=0;i<4;i++) { if(!isfinite(f->gun_q[i])) return VK_ERROR_INITIALIZATION_FAILED; norm+=f->gun_q[i]*f->gun_q[i]; }
    if(fabsf(norm-1)>.001f || !isfinite(f->kick) || f->kick<0 || f->kick>1) return VK_ERROR_INITIALIZATION_FAILED;
    for(unsigned i=0;i<3;i++) if(!isfinite(f->gun_p[i])) return VK_ERROR_INITIALIZATION_FAILED;
    int sight=f->sight;
    if(sight<0 || sight>=QO_SIGHT_COUNT) sight=QO_SIGHT_IRON;
    if(f->weapon==QO_WEAPON_RIFLE) { if(cur_sight!=RIFLE_MESH || rifle_scale!=scope_scale(f)) build_rifle(scope_scale(f)); }
    else if(cur_sight!=sight) build_gun(sight);
    (void)SIGHT_Y; (void)GLASS;
    ChVkColorVertex *vertices=calloc((size_t)gun_verts,sizeof(*vertices));
    uint16_t *indices=malloc((size_t)gun_verts*sizeof(*indices));
    if(!vertices || !indices) { free(vertices); free(indices); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    float model[16],slide[16],kick[16],back[16]={0};
    pose_matrix(f->gun_q,f->gun_p,model); kick_matrix(f->kick,kick); mul(model,model,kick);
    back[0]=back[5]=back[10]=back[15]=1;
    back[14]=.022f*(f->kick>.6f?(1-f->kick)/.4f:f->kick/.6f);
    mul(slide,model,back);
    VkResult result=VK_ERROR_INITIALIZATION_FAILED;
    for(int i=0;i<gun_verts;i++) {
        const float *m=i<slide_verts?slide:model,*v=mesh+i*9;
        float world[3],clip[4],normal[3]; apply(m,v,world);
        float length=0;
        for(unsigned k=0;k<3;k++) { normal[k]=m[k]*v[3]+m[4+k]*v[4]+m[8+k]*v[5]; length+=normal[k]*normal[k]; }
        length=sqrtf(length); if(!(length>0)) goto done;
        for(unsigned k=0;k<3;k++) normal[k]/=length;
        float diffuse=.38f+.62f*fmaxf(0,(normal[0]*.3f+normal[1]+normal[2]*.4f)/sqrtf(1.25f));
        float spec=powf(fmaxf(0,(-normal[0]*.2f+normal[1]+normal[2]*.6f)/sqrtf(1.4f)),24)*.18f;
        for(unsigned k=0;k<4;k++) clip[k]=vp[k]*world[0]+vp[4+k]*world[1]+vp[8+k]*world[2]+vp[12+k];
        if(clip[3]==0) goto done;
        vertices[i].xy[0]=clip[0]; vertices[i].xy[1]=-clip[1];
        vertices[i].xy[2]=(clip[2]+clip[3])*.5f; vertices[i].xy[3]=clip[3];
        for(unsigned k=0;k<4;k++) if(!isfinite(vertices[i].xy[k])) goto done;
        for(unsigned k=0;k<3;k++) vertices[i].rgba[k]=v[6+k]*diffuse+spec;
        vertices[i].rgba[3]=1; indices[i]=(uint16_t)i;
    }
    result=ch_vk_target_clear_aspects_read(target,2,NULL,1,0,pixels,bytes);
    if(result!=VK_SUCCESS) goto done;
    const Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=1,.depth_write=1,.depth_func=0x201};
    result=ch_vk_target_draw_state_read(target,NULL,vertices,(uint32_t)gun_verts,indices,(uint32_t)gun_verts,&state,pixels,bytes);
done:
    free(vertices); free(indices); return result;
}

static VkResult effect_read(ChVkTarget *target,const float vp[16],const float q[6][5],
    const ChVkEffect *effect,int depth,void *pixels,VkDeviceSize bytes) {
    ChVkColorVertex vertices[6]={0};
    const uint16_t indices[6]={0,1,2,3,4,5};
    for(unsigned i=0;i<6;i++) {
        float clip[4];
        for(unsigned k=0;k<4;k++) clip[k]=vp[k]*q[i][0]+vp[4+k]*q[i][1]+vp[8+k]*q[i][2]+vp[12+k];
        if(clip[3]==0) return VK_ERROR_INITIALIZATION_FAILED;
        vertices[i].xy[0]=clip[0]; vertices[i].xy[1]=-clip[1];
        vertices[i].xy[2]=(clip[2]+clip[3])*.5f; vertices[i].xy[3]=clip[3];
        for(unsigned k=0;k<4;k++) if(!isfinite(vertices[i].xy[k])) return VK_ERROR_INITIALIZATION_FAILED;
        vertices[i].xy[4]=q[i][3]; vertices[i].xy[5]=q[i][4];
    }
    const Nv2aRenderState state={.color_mask=0x01010101,.blend_enable=1,
        .blend_src=1,.blend_dst=0x303,.blend_eq=0x8006,
        .depth_test_enable=(uint32_t)depth,.depth_write=0,.depth_func=0x203};
    return ch_vk_target_draw_effect_read(target,vertices,6,indices,6,&state,effect,pixels,bytes);
}

/* A point of the lens's plane (x, y in lens radii from its centre, dz toward
 * the eye) into a vertex. */
static int lens_vertex(const float model[16],const float vp[16],const float c[3],float r,float x,float y,float dz,
    ChVkColorVertex *out) {
    const float local[3]={c[0]+x*r,c[1]+y*r,c[2]+dz};
    float world[3],clip[4]; apply(model,local,world);
    for(unsigned k=0;k<4;k++) clip[k]=vp[k]*world[0]+vp[4+k]*world[1]+vp[8+k]*world[2]+vp[12+k];
    if(clip[3]==0) return 0;
    out->xy[0]=clip[0]; out->xy[1]=-clip[1];
    out->xy[2]=(clip[2]+clip[3])*.5f; out->xy[3]=clip[3];
    for(unsigned k=0;k<4;k++) if(!isfinite(out->xy[k])) return 0;
    return 1;
}
/* The crosshair at the lens's centre, as the game's reticle (it is not in
 * the picture: vr_lens_crop moves the picture under it): thin dark lines
 * across, thick posts toward the rim, a red dot. Rectangles in lens radii
 * (x0, y0, x1, y1), the last red; just in front of the glass. */
static const float CROSS[][4]={
    {-.995f,-.006f,.995f,.006f},{-.006f,-.995f,.006f,.995f},
    {.5f,-.02f,.995f,.02f},{-.995f,-.02f,-.5f,.02f},{-.02f,.5f,.02f,.995f},{-.02f,-.995f,.02f,-.5f},
    {-.018f,-.018f,.018f,.018f},
};
#define CROSS_N (int)(sizeof CROSS/sizeof *CROSS)
static VkResult crosshair_read(ChVkTarget *target,const float model[16],const float vp[16],const float c[3],float r,
    void *pixels,VkDeviceSize bytes) {
    ChVkColorVertex v[CROSS_N*4]={0};
    uint16_t idx[CROSS_N*6];
    for(int q=0;q<CROSS_N;q++) {
        const float *b=CROSS[q],x[4]={b[0],b[2],b[2],b[0]},y[4]={b[1],b[1],b[3],b[3]};
        const int red=q==CROSS_N-1;
        for(int k=0;k<4;k++) {
            ChVkColorVertex *o=&v[q*4+k];
            if(!lens_vertex(model,vp,c,r,x[k],y[k],.0005f,o)) return VK_ERROR_INITIALIZATION_FAILED;
            o->rgba[0]=red?.9f:.02f; o->rgba[1]=red?.08f:.02f; o->rgba[2]=red?.05f:.02f; o->rgba[3]=1;
        }
        const uint16_t base=(uint16_t)(q*4),tri[6]={0,1,2,0,2,3};
        for(int k=0;k<6;k++) idx[q*6+k]=(uint16_t)(base+tri[k]);
    }
    const Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=1,.depth_write=1,.depth_func=0x203};
    return ch_vk_target_draw_state_read(target,NULL,v,CROSS_N*4,idx,CROSS_N*6,&state,pixels,bytes);
}
/* The rifle's lens: a 32-segment disc on the scope's rear face, facing +z (the
 * eye), the picture's crop (QoFrame.lens_crop; all of it by default) on its
 * inscribed circle -- or dark glass when there is no fresh picture. The
 * picture is drawn opaque: its alpha is not 1 (the game's X8R8G8B8 target),
 * so the colour goes in without alpha (and without blending) and a second
 * pass writes alpha 1 alone. */
static VkResult lens_read(ChVkTarget *target,const QoFrame *f,const float model[16],const float vp[16],
    void *pixels,VkDeviceSize bytes) {
    enum { N=32 };
    ChVkColorVertex v[N+1]={0};
    uint16_t idx[N*3];
    const int textured=f->lens_fresh && f->lens_tex;
    const int crop=f->lens_crop[2]>0 && f->lens_crop[2]<=.5f && isfinite(f->lens_crop[0]) && isfinite(f->lens_crop[1]);
    const float cu=crop?f->lens_crop[0]:.5f,cv=crop?f->lens_crop[1]:.5f,h=crop?f->lens_crop[2]:.5f;
    float c[3],radius; rifle_lens(scope_scale(f),c,&radius);
    for(int i=0;i<=N;i++) {
        float a=(float)(2*3.14159265358979*i/N),dx=i<N?cosf(a):0,dy=i<N?sinf(a):0;
        if(!lens_vertex(model,vp,c,radius,dx,dy,0,&v[i])) return VK_ERROR_INITIALIZATION_FAILED;
        v[i].xy[4]=cu+h*dx; v[i].xy[5]=cv-h*dy;
        v[i].rgba[0]=v[i].rgba[1]=v[i].rgba[2]=textured?1.f:.03f; v[i].rgba[3]=1;
    }
    for(int i=0;i<N;i++) { idx[i*3]=(uint16_t)N; idx[i*3+1]=(uint16_t)i; idx[i*3+2]=(uint16_t)((i+1)%N); }
    Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=1,.depth_write=1,.depth_func=0x203};
    if(!textured) return ch_vk_target_draw_state_read(target,NULL,v,N+1,idx,N*3,&state,pixels,bytes);
    state.color_mask=0x00010101;
    VkResult r=ch_vk_target_draw_texture_read(target,NULL,v,N+1,idx,N*3,&state,(const ChVkTexture *)f->lens_tex,pixels,bytes);
    if(r==VK_SUCCESS && f->lens_cross) r=crosshair_read(target,model,vp,c,radius,pixels,bytes);
    if(r!=VK_SUCCESS) return r;
    /* Alpha 1 over the whole disc with no depth test: it cannot depend on the colour pass's depth
     * matching. Anything nearer is the opaque gun, whose alpha is 1 already. */
    state.color_mask=0x01000000; state.depth_test_enable=0; state.depth_write=0;
    return ch_vk_target_draw_state_read(target,NULL,v,N+1,idx,N*3,&state,pixels,bytes);
}

VkResult ch_vk_overlay_read(ChVkTarget *target,const QoFrame *f,const float vp[16],
    const float eye[3],float sight_height,void *pixels,VkDeviceSize bytes) {
    if(!target || !f || !vp || !eye || !pixels || !isfinite(sight_height) || sight_height<=0)
        return VK_ERROR_INITIALIZATION_FAILED;
    if(!f->gun_valid) return ch_vk_target_clear_aspects_read(target,0,NULL,0,0,pixels,bytes);
    for(unsigned i=0;i<3;i++) if(!isfinite(eye[i]) || !isfinite(f->end[i]) || !isfinite(f->gun_p[i]))
        return VK_ERROR_INITIALIZATION_FAILED;
    for(unsigned i=0;i<16;i++) if(!isfinite(vp[i])) return VK_ERROR_INITIALIZATION_FAILED;
    float norm=0;
    for(unsigned i=0;i<4;i++) { if(!isfinite(f->gun_q[i])) return VK_ERROR_INITIALIZATION_FAILED; norm+=f->gun_q[i]*f->gun_q[i]; }
    if(fabsf(norm-1)>.001f || !isfinite(f->kick) || f->kick<0 || f->kick>1) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result=ch_vk_overlay_gun_read(target,f,vp,pixels,bytes);
    if(result!=VK_SUCCESS || !f->gun_valid) return result;
    int sight=f->sight;
    if(sight<0 || sight>=QO_SIGHT_COUNT) sight=QO_SIGHT_IRON;
    float model[16],kick[16],slide[16],back[16]={0},muzzle[3],q[6][5];
    pose_matrix(f->gun_q,f->gun_p,model); kick_matrix(f->kick,kick); mul(model,model,kick);
    back[0]=back[5]=back[10]=back[15]=1;
    back[14]=.022f*(f->kick>.6f?(1-f->kick)/.4f:f->kick/.6f); mul(slide,model,back);
    const int rifle=f->weapon==QO_WEAPON_RIFLE;
    const float local_muzzle[3]={0,0,rifle?RIFLE_MUZZLE_Z:MUZZLE_Z}; apply(model,local_muzzle,muzzle);
    if(rifle && f->show_gun) {
        result=lens_read(target,f,model,vp,pixels,bytes);
        if(result!=VK_SUCCESS) return result;
    }
    if(!rifle && f->show_gun && sight!=QO_SIGHT_IRON) {
        float center[2],radii[3],box[4];
        qo_glass_vertices(q,center,radii,box,slide,sight,sight_height,eye,f->end);
        /* The pane is symmetric about x = 0: shape.w its half width,
         * reticle.zw its bottom and top. */
        ChVkEffect effect={.kind=2,.color={1,.09f,.05f,1},.tint={.55f,.75f,.85f,.06f},
            .shape={radii[0],radii[1],radii[2],box[2]},.reticle={center[0],center[1],box[1],box[3]}};
        result=effect_read(target,vp,(const float (*)[5])q,&effect,1,pixels,bytes);
        if(result!=VK_SUCCESS) return result;
    }
    ChVkEffect effect={.kind=1,.color={1,.12f,.08f,.85f}};
    if(f->beam && qo_beam_vertices(q,eye,muzzle,f->end,.0015f)) {
        result=effect_read(target,vp,(const float (*)[5])q,&effect,0,pixels,bytes);
        if(result!=VK_SUCCESS) return result;
    }
    float d[3]={f->end[0]-eye[0],f->end[1]-eye[1],f->end[2]-eye[2]};
    float dist=sqrtf(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
    effect.color[1]=.15f; effect.color[2]=.1f; effect.color[3]=1;
    if(f->dot && qo_spot_vertices(q,eye,f->end,.006f*dist)) {
        result=effect_read(target,vp,(const float (*)[5])q,&effect,0,pixels,bytes);
        if(result!=VK_SUCCESS) return result;
    }
    effect.color[1]=.75f; effect.color[2]=.3f; effect.color[3]=.9f;
    if(f->flash && qo_spot_vertices(q,eye,muzzle,.035f))
        result=effect_read(target,vp,(const float (*)[5])q,&effect,0,pixels,bytes);
    return result;
}
