#include "vk_overlay.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "../../xr/quest_gun_geometry.inc"
#include "../../xr/quest_overlay_math.inc"
#include "../../xr/quest_overlay_effects.inc"
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
    if(cur_sight!=sight) build_gun(sight);
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
    const float local_muzzle[3]={0,0,MUZZLE_Z}; apply(model,local_muzzle,muzzle);
    if(f->show_gun && sight!=QO_SIGHT_IRON) {
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
