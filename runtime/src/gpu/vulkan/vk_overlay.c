#include "vk_overlay.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vk_jpeg.h"
#include "../../xr/quest_gun_geometry.inc"
#include "../../xr/quest_rifle_geometry.inc"
#include "../../xr/quest_shotgun_geometry.inc"
#include "../../xr/quest_overlay_math.inc"
#include "../../xr/quest_overlay_effects.inc"
#include "../../xr/quest_rifle_model.inc"
#include "../../xr/quest_rifle_scope.inc"
/* The box rifle (the fallback) into the shared mesh (all of it rides the
 * controller; no slide), its scope at scale s (QoFrame.scope_scale). */
#define RIFLE_MESH (-2)
static float rifle_scale;
static float scope_scale(const QoFrame *f) {return f->scope_scale>0?f->scope_scale:1.f;}
static void build_rifle(float s) {
    int n=0;
    Box scope[RIFLE_SCOPE_N]; rifle_scope(s,scope);
    ADD(&n,RIFLE_BODY); add_boxes(&n,scope,RIFLE_SCOPE_N);
    slide_verts=0; gun_verts=n; cur_sight=RIFLE_MESH; rifle_scale=s;
}

/* The rifle: the M700 (third_party/rifle, CC0) textured, and the scope
 * lathed round the lens (quest_rifle_scope.inc) -- or the boxes when the
 * model cannot be had (said once). The mesh is unpacked, its colour decoded
 * and uploaded (mipmapped, filtered linearly: it does not shimmer) once, on
 * the device of the first target that wants it (ch_vk_overlay_prepare, at
 * start-up). The vertices: the M700's, then the scope's (rebuilt when the
 * scale changes); texture coordinates and alpha set once, position and
 * light written each eye in one pass (rifle_shade). */
static struct {
    int state;                  /* 0: not tried; 1: ready; -1: failed, the boxes */
    VkDevice device;
    RifleModel m;
    ChVkTexture *tex;
    ChVkColorVertex *v;         /* m.nv, then up to SCOPE_MAX_V */
    float scope_s;              /* the scale the scope was built at (0: none) */
} s_m700;
static RifleScopeMesh s_m700_scope;
static void m700_fail(const char *why) {
    fprintf(stderr,"[overlay] rifle model: %s -- the box rifle instead\n",why);
    if(s_m700.tex) ch_vk_texture_destroy(s_m700.tex);
    rifle_model_free(&s_m700.m); free(s_m700.v);
    s_m700.tex=NULL; s_m700.v=NULL; s_m700.state=-1;
}
/* A model's colour (a JPEG in the binary) decoded and uploaded to target's
 * device, mipmapped, with a linear sampler of its own (every other
 * texture's is as it was): VK_SUCCESS and *tex, its size and levels; or
 * why not, *tex NULL. */
static VkResult model_texture(ChVkTarget *target,const unsigned char *jpg,const unsigned char *jpg_end,
    ChVkTexture **tex,uint32_t *w,uint32_t *h,uint32_t *levels,const char **why) {
    uint32_t *argb=ch_jpeg_argb(jpg,(size_t)(jpg_end-jpg),w,h);
    *tex=NULL; *levels=1;
    if(!argb) { *why="its texture does not decode"; return VK_ERROR_INITIALIZATION_FAILED; }
    for(uint32_t e=*w>*h?*w:*h;e>1;e>>=1) (*levels)++;
    VkResult r=ch_vk_target_upload_texture_mips(target,*w,*h,argb,*levels,tex);
    if(r==VK_ERROR_FORMAT_NOT_SUPPORTED) r=ch_vk_target_upload_texture_mips(target,*w,*h,argb,*levels=1,tex);
    free(argb);
    if(r!=VK_SUCCESS) { *tex=NULL; *why="its texture does not upload"; return r; }
    const VkSamplerCreateInfo linear={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter=VK_FILTER_LINEAR,.minFilter=VK_FILTER_LINEAR,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU=VK_SAMPLER_ADDRESS_MODE_REPEAT,.addressModeV=VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW=VK_SAMPLER_ADDRESS_MODE_REPEAT,.maxLod=VK_LOD_CLAMP_NONE};
    r=ch_vk_texture_set_sampler(*tex,&linear);
    if(r!=VK_SUCCESS) { ch_vk_texture_destroy(*tex); *tex=NULL; *why="no sampler for its texture"; }
    return r;
}
/* 1: the model is ready to draw into target; 0: the boxes. */
static int m700_ready(ChVkTarget *target) {
    if(s_m700.state) return s_m700.state>0 && s_m700.device==ch_vk_target_device(target);
    if(!rifle_model_unpack(rifle_m700_mesh,(size_t)(rifle_m700_mesh_end-rifle_m700_mesh),&s_m700.m)) {
        m700_fail("its mesh does not unpack"); return 0;
    }
    uint32_t w,h,levels;
    const char *why;
    if(model_texture(target,rifle_m700_jpg,rifle_m700_jpg_end,&s_m700.tex,&w,&h,&levels,&why)!=VK_SUCCESS) {
        m700_fail(why); return 0;
    }
    s_m700.v=calloc((size_t)s_m700.m.nv+SCOPE_MAX_V,sizeof *s_m700.v);
    if(!s_m700.v) { m700_fail("out of memory"); return 0; }
    for(int i=0;i<s_m700.m.nv;i++) {
        s_m700.v[i].xy[4]=s_m700.m.uv[i*2]; s_m700.v[i].xy[5]=s_m700.m.uv[i*2+1]; s_m700.v[i].rgba[3]=1;
    }
    s_m700.device=ch_vk_target_device(target); s_m700.scope_s=0; s_m700.state=1;
    fprintf(stderr,"[overlay] rifle model: the M700, %d triangles, its texture %ux%u (%u levels)\n",
        s_m700.m.ni/3,w,h,levels);
    return 1;
}

/* The shotgun: the Remington 870 (third_party/shotgun, CC BY 4.0) textured,
 * its pump slid back along the barrel by QoFrame.pump of R870_STROKE, an
 * optic's mount strip on the receiver -- or the handgun's boxes when the
 * model cannot be had (said once). Loaded as the M700 is (m700_ready), once,
 * on the device of the first target that wants it. The vertices: the
 * body's, then the pump's (its indices after the body's, offset), then a
 * mount strip's; texture coordinates and alpha set once, position and light
 * written each eye (rifle_shade). */
extern const unsigned char shotgun_r870_mesh[],shotgun_r870_mesh_end[],shotgun_r870_pump_mesh[],
    shotgun_r870_pump_mesh_end[],shotgun_r870_jpg[],shotgun_r870_jpg_end[];
static struct {
    int state;                  /* 0: not tried; 1: ready; -1: failed, the handgun */
    VkDevice device;
    RifleModel body,pump;
    ChVkTexture *tex;
    ChVkColorVertex *v;         /* body.nv, pump.nv, then R870_BOX_V */
    uint16_t *idx;              /* body.ni, then pump.ni */
} s_r870;
static void r870_fail(const char *why) {
    fprintf(stderr,"[overlay] shotgun model: %s -- the handgun instead\n",why);
    if(s_r870.tex) ch_vk_texture_destroy(s_r870.tex);
    rifle_model_free(&s_r870.body); rifle_model_free(&s_r870.pump); free(s_r870.v); free(s_r870.idx);
    s_r870.tex=NULL; s_r870.v=NULL; s_r870.idx=NULL; s_r870.state=-1;
}
/* 1: the model is ready to draw into target; 0: the handgun. */
static int r870_ready(ChVkTarget *target) {
    if(s_r870.state) return s_r870.state>0 && s_r870.device==ch_vk_target_device(target);
    if(!rifle_model_unpack(shotgun_r870_mesh,(size_t)(shotgun_r870_mesh_end-shotgun_r870_mesh),&s_r870.body) ||
       !rifle_model_unpack(shotgun_r870_pump_mesh,(size_t)(shotgun_r870_pump_mesh_end-shotgun_r870_pump_mesh),&s_r870.pump)) {
        r870_fail("its mesh does not unpack"); return 0;
    }
    const int nv=s_r870.body.nv+s_r870.pump.nv,ni=s_r870.body.ni+s_r870.pump.ni;
    if(nv>65535) { r870_fail("too many vertices for 16-bit indices"); return 0; }
    uint32_t w,h,levels;
    const char *why;
    if(model_texture(target,shotgun_r870_jpg,shotgun_r870_jpg_end,&s_r870.tex,&w,&h,&levels,&why)!=VK_SUCCESS) {
        r870_fail(why); return 0;
    }
    s_r870.v=calloc((size_t)nv+R870_BOX_V,sizeof *s_r870.v);
    s_r870.idx=malloc((size_t)ni*sizeof *s_r870.idx);
    if(!s_r870.v || !s_r870.idx) { r870_fail("out of memory"); return 0; }
    const RifleModel *part[2]={&s_r870.body,&s_r870.pump};
    for(int k=0,base=0,at=0;k<2;base+=part[k]->nv,at+=part[k]->ni,k++) {
        for(int i=0;i<part[k]->nv;i++) {
            ChVkColorVertex *o=&s_r870.v[base+i];
            o->xy[4]=part[k]->uv[i*2]; o->xy[5]=part[k]->uv[i*2+1]; o->rgba[3]=1;
        }
        for(int i=0;i<part[k]->ni;i++) s_r870.idx[at+i]=(uint16_t)(part[k]->idx[i]+base);
    }
    for(int i=0;i<R870_BOX_V;i++) s_r870.v[nv+i].rgba[3]=1;
    s_r870.device=ch_vk_target_device(target); s_r870.state=1;
    fprintf(stderr,"[overlay] shotgun model: the Remington 870, %d triangles (its pump %d), its texture %ux%u (%u levels)\n",
        ni/3,s_r870.pump.ni/3,w,h,levels);
    return 1;
}
void ch_vk_overlay_prepare(ChVkTarget *target,int weapon) {
    if(target && weapon==QO_WEAPON_RIFLE) (void)m700_ready(target);
    if(target && weapon==QO_WEAPON_SHOTGUN) (void)r870_ready(target);
}
/* The M700 and its scope, opaque and depth-tested, into target (its depth
 * cleared by the caller, as for the box gun: the headset loop clears all of
 * the target as its pass begins; a clear here was a full-target clear inside
 * the pass, every tile of it, 120 times a second). */
static VkResult m700_read(ChVkTarget *target,const QoFrame *f,const float vp[16],void *pixels,VkDeviceSize bytes) {
    ChVkColorVertex *sv=s_m700.v+s_m700.m.nv;
    const float s=scope_scale(f);
    if(s_m700.scope_s!=s) {
        if(!rifle_scope_mesh(s,&s_m700_scope)) return VK_ERROR_INITIALIZATION_FAILED;
        for(int i=0;i<s_m700_scope.nv;i++) sv[i].rgba[3]=1;
        s_m700.scope_s=s;
    }
    float model[16],kick[16],mvp[16];
    pose_matrix(f->gun_q,f->gun_p,model); kick_matrix(f->kick,kick); mul(model,model,kick); mul(mvp,vp,model);
    const int stride=(int)(sizeof(ChVkColorVertex)/sizeof(float)),col=(int)(offsetof(ChVkColorVertex,rgba)/sizeof(float));
    if(!rifle_shade(s_m700.m.p,s_m700.m.n,NULL,s_m700.m.nv,mvp,model,(float *)(void *)s_m700.v,stride,col) ||
       !rifle_shade(&s_m700_scope.p[0][0],&s_m700_scope.n[0][0],&s_m700_scope.c[0][0],s_m700_scope.nv,mvp,model,
           (float *)(void *)sv,stride,col))
        return VK_ERROR_INITIALIZATION_FAILED;
    const Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=1,.depth_write=1,.depth_func=0x201};
    VkResult result=ch_vk_target_draw_texture_read(target,NULL,s_m700.v,(uint32_t)s_m700.m.nv,s_m700.m.idx,(uint32_t)s_m700.m.ni,
        &state,s_m700.tex,pixels,bytes);
    if(result!=VK_SUCCESS) return result;
    return ch_vk_target_draw_state_read(target,NULL,sv,(uint32_t)s_m700_scope.nv,s_m700_scope.idx,(uint32_t)s_m700_scope.ni,
        &state,pixels,bytes);
}
/* The 870, its pump where QoFrame.pump puts it, and the sight's mount strip
 * (none for the bead), opaque and depth-tested as the M700. */
static VkResult r870_read(ChVkTarget *target,const QoFrame *f,int sight,const float vp[16],void *pixels,VkDeviceSize bytes) {
    float model[16],kick[16],mvp[16],back[16]={0},pump[16],pump_mvp[16];
    pose_matrix(f->gun_q,f->gun_p,model); kick_matrix(f->kick,kick); mul(model,model,kick); mul(mvp,vp,model);
    back[0]=back[5]=back[10]=back[15]=1; back[14]=R870_STROKE*f->pump;
    mul(pump,model,back); mul(pump_mvp,vp,pump);
    const int stride=(int)(sizeof(ChVkColorVertex)/sizeof(float)),col=(int)(offsetof(ChVkColorVertex,rgba)/sizeof(float));
    const int nv=s_r870.body.nv+s_r870.pump.nv,ni=s_r870.body.ni+s_r870.pump.ni;
    if(!rifle_shade(s_r870.body.p,s_r870.body.n,NULL,s_r870.body.nv,mvp,model,(float *)(void *)s_r870.v,stride,col) ||
       !rifle_shade(s_r870.pump.p,s_r870.pump.n,NULL,s_r870.pump.nv,pump_mvp,pump,
           (float *)(void *)(s_r870.v+s_r870.body.nv),stride,col))
        return VK_ERROR_INITIALIZATION_FAILED;
    const Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=1,.depth_write=1,.depth_func=0x201};
    VkResult result=ch_vk_target_draw_texture_read(target,NULL,s_r870.v,(uint32_t)nv,s_r870.idx,(uint32_t)ni,
        &state,s_r870.tex,pixels,bytes);
    if(result!=VK_SUCCESS || sight==QO_SIGHT_IRON) return result;
    float p[R870_BOX_V][3],n[R870_BOX_V][3],c[R870_BOX_V][3];
    uint16_t idx[R870_BOX_V];
    r870_box(&R870_MOUNT[sight],p,n,c);
    for(int i=0;i<R870_BOX_V;i++) idx[i]=(uint16_t)i;
    if(!rifle_shade(&p[0][0],&n[0][0],&c[0][0],R870_BOX_V,mvp,model,(float *)(void *)(s_r870.v+nv),stride,col))
        return VK_ERROR_INITIALIZATION_FAILED;
    return ch_vk_target_draw_state_read(target,NULL,s_r870.v+nv,R870_BOX_V,idx,R870_BOX_V,&state,pixels,bytes);
}
VkResult ch_vk_overlay_gun_read(ChVkTarget *target,const QoFrame *f,const float vp[16],void *pixels,VkDeviceSize bytes) {
    if(!target || !f || !vp || !pixels) return VK_ERROR_INITIALIZATION_FAILED;
    if(!f->gun_valid || !f->show_gun) return ch_vk_target_clear_aspects_read(target,0,NULL,0,0,pixels,bytes);
    for(unsigned i=0;i<16;i++) if(!isfinite(vp[i])) return VK_ERROR_INITIALIZATION_FAILED;
    float norm=0;
    for(unsigned i=0;i<4;i++) { if(!isfinite(f->gun_q[i])) return VK_ERROR_INITIALIZATION_FAILED; norm+=f->gun_q[i]*f->gun_q[i]; }
    if(fabsf(norm-1)>.001f || !isfinite(f->kick) || f->kick<0 || f->kick>1) return VK_ERROR_INITIALIZATION_FAILED;
    if(!isfinite(f->pump) || f->pump<0 || f->pump>1) return VK_ERROR_INITIALIZATION_FAILED;
    for(unsigned i=0;i<3;i++) if(!isfinite(f->gun_p[i])) return VK_ERROR_INITIALIZATION_FAILED;
    int sight=f->sight;
    if(sight<0 || sight>=QO_SIGHT_COUNT) sight=QO_SIGHT_IRON;
    if(f->weapon==QO_WEAPON_RIFLE && m700_ready(target)) return m700_read(target,f,vp,pixels,bytes);
    if(f->weapon==QO_WEAPON_SHOTGUN && r870_ready(target)) return r870_read(target,f,sight,vp,pixels,bytes);
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
    const Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=1,.depth_write=1,.depth_func=0x201};
    result=ch_vk_target_draw_state_read(target,NULL,vertices,(uint32_t)gun_verts,indices,(uint32_t)gun_verts,&state,pixels,bytes);   /* depth: the caller's clear */
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
 * (x0, y0, x1, y1), the last red; just in front of the glass (depth 0: over
 * it by order alone). */
static const float CROSS[][4]={
    {-.995f,-.006f,.995f,.006f},{-.006f,-.995f,.006f,.995f},
    {.5f,-.02f,.995f,.02f},{-.995f,-.02f,-.5f,.02f},{-.02f,.5f,.02f,.995f},{-.02f,-.995f,.02f,-.5f},
    {-.018f,-.018f,.018f,.018f},
};
#define CROSS_N (int)(sizeof CROSS/sizeof *CROSS)
static VkResult crosshair_read(ChVkTarget *target,const float model[16],const float vp[16],const float c[3],float r,
    int depth,void *pixels,VkDeviceSize bytes) {
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
    const Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=(uint32_t)depth,.depth_write=(uint32_t)depth,
        .depth_func=0x203};
    return ch_vk_target_draw_state_read(target,NULL,v,CROSS_N*4,idx,CROSS_N*6,&state,pixels,bytes);
}
/* A lens's picture: a 32-segment disc of radius radius round c in model's
 * space, facing +z (the eye), the picture's crop (QoFrame.lens_crop; all of
 * it by default) on its inscribed circle -- or dark glass when there is no
 * fresh picture -- with its crosshair; depth tested (the rifle's lens, behind
 * nothing of the gun), or with depth 0 over whatever is drawn already. The
 * picture is drawn opaque: its alpha is not 1 (the game's X8R8G8B8 target),
 * so the colour goes in without alpha (and without blending) and a second
 * pass writes alpha 1 alone. */
static VkResult lens_disc_read(ChVkTarget *target,const QoFrame *f,const float model[16],const float vp[16],
    const float c[3],float radius,int depth,void *pixels,VkDeviceSize bytes) {
    enum { N=32 };
    ChVkColorVertex v[N+1]={0};
    uint16_t idx[N*3];
    const int textured=f->lens_fresh && f->lens_tex;
    const int crop=f->lens_crop[2]>0 && f->lens_crop[2]<=.5f && isfinite(f->lens_crop[0]) && isfinite(f->lens_crop[1]);
    const float cu=crop?f->lens_crop[0]:.5f,cv=crop?f->lens_crop[1]:.5f,h=crop?f->lens_crop[2]:.5f;
    for(int i=0;i<=N;i++) {
        float a=(float)(2*3.14159265358979*i/N),dx=i<N?cosf(a):0,dy=i<N?sinf(a):0;
        if(!lens_vertex(model,vp,c,radius,dx,dy,0,&v[i])) return VK_ERROR_INITIALIZATION_FAILED;
        v[i].xy[4]=cu+h*dx; v[i].xy[5]=cv-h*dy;
        v[i].rgba[0]=v[i].rgba[1]=v[i].rgba[2]=textured?1.f:.03f; v[i].rgba[3]=1;
    }
    for(int i=0;i<N;i++) { idx[i*3]=(uint16_t)N; idx[i*3+1]=(uint16_t)i; idx[i*3+2]=(uint16_t)((i+1)%N); }
    Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=(uint32_t)depth,.depth_write=(uint32_t)depth,
        .depth_func=0x203};
    if(!textured) return ch_vk_target_draw_state_read(target,NULL,v,N+1,idx,N*3,&state,pixels,bytes);
    state.color_mask=0x00010101;
    VkResult r=ch_vk_target_draw_texture_read(target,NULL,v,N+1,idx,N*3,&state,(const ChVkTexture *)f->lens_tex,pixels,bytes);
    if(r==VK_SUCCESS && f->lens_cross) r=crosshair_read(target,model,vp,c,radius,depth,pixels,bytes);
    if(r!=VK_SUCCESS) return r;
    /* Alpha 1 over the whole disc with no depth test: it cannot depend on the colour pass's depth
     * matching. Anything nearer is the opaque gun, whose alpha is 1 already. */
    state.color_mask=0x01000000; state.depth_test_enable=0; state.depth_write=0;
    return ch_vk_target_draw_state_read(target,NULL,v,N+1,idx,N*3,&state,pixels,bytes);
}
/* The rifle's lens: on the scope's rear face, depth tested against the gun. */
static VkResult lens_read(ChVkTarget *target,const QoFrame *f,const float model[16],const float vp[16],
    void *pixels,VkDeviceSize bytes) {
    float c[3],radius; rifle_lens(scope_scale(f),c,&radius);
    return lens_disc_read(target,f,model,vp,c,radius,1,pixels,bytes);
}
/* A dark ring round a disc of radius r (1 to 1.04 of it), opaque, over what is drawn. */
static VkResult rim_read(ChVkTarget *target,const float model[16],const float vp[16],const float c[3],float r,
    void *pixels,VkDeviceSize bytes) {
    enum { N=64 };
    ChVkColorVertex v[N*2]={0};
    uint16_t idx[N*6];
    for(int i=0;i<N;i++) {
        const float a=(float)(2*3.14159265358979*i/N),x=cosf(a),y=sinf(a);
        if(!lens_vertex(model,vp,c,r,x,y,0,&v[i*2]) || !lens_vertex(model,vp,c,r,x*1.04f,y*1.04f,0,&v[i*2+1]))
            return VK_ERROR_INITIALIZATION_FAILED;
        for(int k=0;k<2;k++) { v[i*2+k].rgba[0]=v[i*2+k].rgba[1]=v[i*2+k].rgba[2]=.02f; v[i*2+k].rgba[3]=1; }
        const uint16_t in0=(uint16_t)(i*2),out0=(uint16_t)(i*2+1),in1=(uint16_t)((i+1)%N*2),out1=(uint16_t)((i+1)%N*2+1);
        const uint16_t q[6]={in0,out0,out1,in0,out1,in1};
        memcpy(&idx[i*6],q,sizeof q);
    }
    const Nv2aRenderState state={.color_mask=0x01010101};
    return ch_vk_target_draw_state_read(target,NULL,v,N*2,idx,N*6,&state,pixels,bytes);
}
/* The rifle's 2D scope (QoFrame.scope_2d; no rifle drawn): the lens's
 * picture on a disc round the aim as this eye sees it -- SCOPE_2D_DIST out
 * from the eye along the aim, facing back along it, its up the world's -- so
 * the two eyes' discs are parallel and it is seen at infinity, as a
 * riflescope shows the far scene; a dark rim round it. Over everything drawn
 * before it: the beam runs up to its rim. */
#define SCOPE_2D_DIST 2.0f
/* The rifle's shot dot (QoFrame.shot_dot), degrees across: the red disc, its
 * dark rim (on the Quest's ~15-17 texels a degree: ~5 and ~9 texels; the
 * rim ~2 texels wide, so it shows on red and on white) */
#define SHOT_DOT_DEG .30f
#define SHOT_RIM_DEG .56f
static VkResult scope_2d_read(ChVkTarget *target,const QoFrame *f,const float vp[16],const float eye[3],
    void *pixels,VkDeviceSize bytes) {
    float d[3]={f->end[0]-f->gun_p[0],f->end[1]-f->gun_p[1],f->end[2]-f->gun_p[2]};
    const float n=sqrtf(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
    if(!(n>0) || !isfinite(n) || !(f->scope_2d>0) || !(f->scope_2d<1.4f)) return VK_ERROR_INITIALIZATION_FAILED;
    for(unsigned k=0;k<3;k++) d[k]/=n;
    /* right: the aim across the world's up (+x when it is straight up or down); up: right across the aim */
    float r[3]={-d[2],0,d[0]},rn=sqrtf(r[0]*r[0]+r[2]*r[2]);
    if(rn<1e-4f) { r[0]=1; r[2]=0; rn=1; }
    r[0]/=rn; r[2]/=rn;
    const float u[3]={r[1]*d[2]-r[2]*d[1],r[2]*d[0]-r[0]*d[2],r[0]*d[1]-r[1]*d[0]};
    const float model[16]={r[0],r[1],r[2],0,u[0],u[1],u[2],0,-d[0],-d[1],-d[2],0,
        eye[0]+d[0]*SCOPE_2D_DIST,eye[1]+d[1]*SCOPE_2D_DIST,eye[2]+d[2]*SCOPE_2D_DIST,1};
    static const float c[3]={0,0,0};
    const float radius=SCOPE_2D_DIST*tanf(f->scope_2d);
    VkResult result=lens_disc_read(target,f,model,vp,c,radius,0,pixels,bytes);
    return result==VK_SUCCESS?rim_read(target,model,vp,c,radius,pixels,bytes):result;
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
    const int rifle=f->weapon==QO_WEAPON_RIFLE,m700=rifle && m700_ready(target),bare=rifle && !f->show_gun;
    const int r870=f->weapon==QO_WEAPON_SHOTGUN && f->show_gun && r870_ready(target);
    /* the beam and the flash from the drawn gun's muzzle: the M700's, the box rifle's, the 870's;
     * with no rifle drawn (its 2D scope), from the controller; with no shotgun drawn, from where
     * the handgun's would be */
    const float local_muzzle[3]={0,m700 && !bare?RIFLE_MUZZLE_Y:r870?R870_MUZZLE_Y:0,
        bare?0:m700?RIFLE_MUZZLE_Z:rifle?RIFLE_BOX_MUZZLE_Z:r870?R870_MUZZLE_Z:MUZZLE_Z};
    apply(model,local_muzzle,muzzle);
    if(rifle && f->show_gun) {
        result=lens_read(target,f,model,vp,pixels,bytes);
        if(result!=VK_SUCCESS) return result;
    }
    if(!rifle && f->show_gun && sight!=QO_SIGHT_IRON) {
        float center[2],radii[3],box[4];
        /* the handgun's glass rides its slide; the shotgun's its receiver */
        qo_glass_vertices(q,center,radii,box,r870?model:slide,sight,r870?R870_GLASS[sight]:GLASS[sight],
            sight_height,eye,f->end);
        /* The pane is symmetric about x = 0: shape.w its half width,
         * reticle.zw its bottom and top. */
        ChVkEffect effect={.kind=2,.color={1,.09f,.05f,1},.tint={.55f,.75f,.85f,.06f},
            .shape={radii[0],radii[1],radii[2],box[2]},.reticle={center[0],center[1],box[1],box[3]}};
        result=effect_read(target,vp,(const float (*)[5])q,&effect,1,pixels,bytes);
        if(result!=VK_SUCCESS) return result;
    }
    ChVkEffect effect={.kind=1,.color={1,.12f,.08f,.85f}};
    /* the rifle's beam behind its scope and lens (depth tested): not across the lens's picture;
     * out to its far end at least 0.0007 rad (about a pixel) of half width, never thinning away */
    if(f->beam && (rifle?qo_beam_vertices_far(q,eye,muzzle,f->end,.0015f,.0007f)
                        :qo_beam_vertices(q,eye,muzzle,f->end,.0015f))) {
        result=effect_read(target,vp,(const float (*)[5])q,&effect,rifle,pixels,bytes);
        if(result!=VK_SUCCESS) return result;
    }
    float d[3]={f->end[0]-eye[0],f->end[1]-eye[1],f->end[2]-eye[2]};
    float dist=sqrtf(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
    effect.color[1]=.15f; effect.color[2]=.1f; effect.color[3]=1;
    if(f->dot && qo_spot_vertices(q,eye,f->end,.006f*dist)) {
        result=effect_read(target,vp,(const float (*)[5])q,&effect,0,pixels,bytes);
        if(result!=VK_SUCCESS) return result;
    }
    /* the rifle's dot where its shot lands: SHOT_DOT_DEG across at any
     * distance, hot at its centre, in a dark rim SHOT_RIM_DEG across that
     * keeps it seen on red and bright ground; behind the rifle (depth tested) */
    if(f->shot_dot && qo_spot_vertices(q,eye,f->end,tanf(SHOT_RIM_DEG*.5f*.0174533f)*dist)) {
        const ChVkEffect dot={.kind=3,.color={1,.05f,.03f,1},.shape={SHOT_DOT_DEG/SHOT_RIM_DEG,.3f},.tint={1,.82f,.72f,.85f}};
        result=effect_read(target,vp,(const float (*)[5])q,&dot,1,pixels,bytes);
        if(result!=VK_SUCCESS) return result;
    }
    if(bare && f->scope_2d>0) {
        result=scope_2d_read(target,f,vp,eye,pixels,bytes);
        if(result!=VK_SUCCESS) return result;
    }
    effect.color[1]=.75f; effect.color[2]=.3f; effect.color[3]=.9f;
    if(f->flash && qo_spot_vertices(q,eye,muzzle,.035f))
        result=effect_read(target,vp,(const float (*)[5])q,&effect,0,pixels,bytes);
    return result;
}
