#include "vk_scene.h"
#include <stdio.h>
#include <string.h>
static ChVkDecodeTexture fallback_decode;
static int use_cpu_image;
static uint32_t cpu_image[12];
static int scene_decode(const Nv2aTexture *tx,uint32_t *pixels) {
    if(use_cpu_image && tx->offset==2048 && tx->width==4 && tx->height==3 && tx->color==0x12) {
        memcpy(pixels,cpu_image,sizeof cpu_image); return 1;
    }
    return fallback_decode(tx,pixels);
}
int ch_test_scene(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family,ChVkDecodeTexture decode) {
    ChVkScene *scene=NULL; fallback_decode=decode; use_cpu_image=0;
    ChVkSceneConfig config={.physical=gpu,.device=device,.queue=queue,.family=family,
        .eye_width=37,.eye_height=19,.hud_width=8,.hud_height=6,.decode=scene_decode};
    if(ch_vk_scene_create(&config,&scene)!=VK_SUCCESS) return 1;
    int failed=0; ChVkSceneFrame frame;
    Vc3VrView view={.on=1,.id=17};
    const float q[]={0,0,0,1},fov[]={-1,1,1,-1};
    for(unsigned eye=0;eye<2;eye++) { float p[]={eye?.3f:-.3f,0,0}; vr_eye_matrix(q,p,fov,1,.1f,100,view.vp[eye]); }
    Nv2aSurface surface={.color_va=2048,.width=4,.height=3,.pitch=16,.bytes_per_pixel=4,.aa_sx=1,.aa_sy=1};
    Nv2aRenderState state={.color_mask=0x01010101};
    Nv2aVertex world[]={
        {.x=1.25f,.y=2.25f,.rhw=.5f,.diffuse=0xffff0000},
        {.x=2.75f,.y=2.25f,.rhw=.5f,.diffuse=0xffff0000},
        {.x=2,.y=.75f,.rhw=.5f,.diffuse=0xffff0000}};
    Nv2aBatch batch={.vertices=world,.count=3,.state=&state};
    Nv2aFixedState ff={0}; ff.proj[0]=2;ff.proj[5]=-2;ff.proj[10]=1;ff.proj[14]=1;ff.c59[0]=2;ff.c59[1]=1.5f;
    Nv2aRawGeometry raw={.kind=NV2A_RAW_FIXED,.ff=&ff}; Nv2aBatch metadata={.raw=&raw};
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"scene check failed line %d\n",__LINE__); failed=1; goto done; } } while(0)
    CHECK(ch_vk_scene_begin(scene,&view,surface.color_va)==VK_SUCCESS);
    CHECK(ch_vk_scene_begin(scene,&view,surface.color_va)==VK_ERROR_INITIALIZATION_FAILED);
    CHECK(ch_vk_scene_observe(scene,&surface,&metadata)==VK_SUCCESS);
    CHECK(ch_vk_scene_draw(scene,&surface,&batch)==VK_SUCCESS);
    CHECK(ch_vk_scene_end(scene,surface.color_va,&frame)==VK_SUCCESS && !frame.immersive && frame.world_batches==1);
    CHECK(ch_vk_scene_begin(scene,&view,surface.color_va)==VK_SUCCESS);
    CHECK(ch_vk_scene_clear(scene,&surface,&state,0xf3,0xff0000ff,0xffffff00)==VK_SUCCESS);
    uint32_t visibility=123; batch.zpass_out=&visibility;
    CHECK(ch_vk_scene_draw(scene,&surface,&batch)==VK_SUCCESS && visibility==123); batch.zpass_out=NULL;
    Nv2aSurface auxiliary=surface; auxiliary.color_va=3072;
    ff.proj[0]=200;
    CHECK(ch_vk_scene_observe(scene,&auxiliary,&metadata)==VK_SUCCESS); ff.proj[0]=2;
    CHECK(ch_vk_scene_draw(scene,&surface,&batch)==VK_SUCCESS);
    Nv2aVertex hud[]={
        {.x=1,.y=1,.rhw=1,.diffuse=0x80ff0000}, {.x=3,.y=1,.rhw=1,.diffuse=0x80ff0000}, {.x=3,.y=2,.rhw=1,.diffuse=0x80ff0000},
        {.x=1,.y=1,.rhw=1,.diffuse=0x80ff0000}, {.x=3,.y=2,.rhw=1,.diffuse=0x80ff0000}, {.x=1,.y=2,.rhw=1,.diffuse=0x80ff0000}};
    Nv2aRenderState hs={.color_mask=0x01010101,.blend_enable=1,.blend_src=0x302,.blend_dst=0x303,
        .depth_test_enable=1,.depth_func=0x200,.depth_write=1,.stencil_enable=1};
    Nv2aBatch panel={.vertices=hud,.count=6,.state=&hs};
    CHECK(ch_vk_scene_draw(scene,&surface,&panel)==VK_SUCCESS);
    CHECK(ch_vk_scene_draw(scene,&surface,&panel)==VK_SUCCESS);
    CHECK(ch_vk_scene_end(scene,surface.color_va,&frame)==VK_SUCCESS && frame.immersive && frame.hud && frame.world_batches==1 && frame.view.id==17 && frame.projection.kx==2);
    unsigned char pixels[37*19*4],other[sizeof pixels],hp[8*6*4];
    CHECK(ch_vk_target_clear_aspects_read(frame.eyes[0],0,NULL,0,0,pixels,sizeof pixels)==VK_SUCCESS);
    CHECK(ch_vk_target_clear_aspects_read(frame.eyes[1],0,NULL,0,0,other,sizeof other)==VK_SUCCESS && memcmp(pixels,other,sizeof pixels));
    CHECK(ch_vk_target_clear_aspects_read(frame.hud_target,0,NULL,0,0,hp,sizeof hp)==VK_SUCCESS);
    for(unsigned y=0;y<6;y++) for(unsigned x=0;x<8;x++) {
        unsigned i=(y*8+x)*4; int inside=x>=2 && x<6 && y>=2 && y<4;
        CHECK(hp[i]==(inside?192:0) && hp[i+1]==0 && hp[i+2]==0 && hp[i+3]==(inside?192:0));
    }
    ChVkTarget *hud_target=frame.hud_target;
    ChVkTarget *source_eyes[2]={frame.eyes[0],frame.eyes[1]};
    CHECK(ch_vk_scene_begin(scene,&view,surface.color_va)==VK_SUCCESS);
    CHECK(ch_vk_scene_draw(scene,&surface,&batch)==VK_SUCCESS);
    Nv2aTexture native_rt={.offset=surface.color_va,.width=4,.height=3,.pitch=16,.color=0x12,.levels=1,.addr_u=3,.addr_v=3,.filter=0x01010000};
    Nv2aVertex quad[]={
        {.x=0,.y=0,.rhw=1,.diffuse=0xffffffff,.u=0,.v=0}, {.x=4,.y=0,.rhw=1,.diffuse=0xffffffff,.u=4,.v=0}, {.x=4,.y=3,.rhw=1,.diffuse=0xffffffff,.u=4,.v=3},
        {.x=0,.y=0,.rhw=1,.diffuse=0xffffffff,.u=0,.v=0}, {.x=4,.y=3,.rhw=1,.diffuse=0xffffffff,.u=4,.v=3}, {.x=0,.y=3,.rhw=1,.diffuse=0xffffffff,.u=0,.v=3}};
    Nv2aBatch copy={.vertices=quad,.count=6,.state=&state,.texture=&native_rt};
    CHECK(ch_vk_scene_draw(scene,&auxiliary,&copy)==VK_SUCCESS);
    /* Sample both stages from the same eye; binary RGB makes multiplication
     * preserve every source pixel while still detecting wrong-eye selection. */
    Nv2aCombiner rc={.control=1,.stage_program=1|(1<<5)};
    rc.color_icw[0]=0x08090000; rc.alpha_icw[0]=0x18190000;
    rc.color_ocw[0]=rc.alpha_ocw[0]=0xc0;
    Nv2aVertexEx ex[6]={0};
    for(unsigned v=0;v<6;v++) for(unsigned stage=0;stage<2;stage++) {
        ex[v].tex[stage][0]=quad[v].u; ex[v].tex[stage][1]=quad[v].v; ex[v].tex[stage][3]=1;
    }
    Nv2aBatch composite={.vertices=quad,.count=6,.state=&state,.rc=&rc,.ext=ex,.stage={&native_rt,&native_rt}};
    CHECK(ch_vk_scene_draw(scene,&auxiliary,&composite)==VK_SUCCESS);
    /* Feedback must snapshot each eye before overwriting that eye's target. */
    Nv2aTexture feedback=native_rt; feedback.offset=auxiliary.color_va; copy.texture=&feedback;
    CHECK(ch_vk_scene_draw(scene,&auxiliary,&copy)==VK_SUCCESS);
    CHECK(ch_vk_scene_end(scene,auxiliary.color_va,&frame)==VK_SUCCESS && frame.immersive && frame.projection.kx==2);
    for(unsigned eye=0;eye<2;eye++) {
        CHECK(ch_vk_target_clear_aspects_read(source_eyes[eye],0,NULL,0,0,pixels,sizeof pixels)==VK_SUCCESS);
        CHECK(ch_vk_target_clear_aspects_read(frame.eyes[eye],0,NULL,0,0,other,sizeof other)==VK_SUCCESS);
        CHECK(memcmp(pixels,other,sizeof pixels)==0);
    }
    Nv2aRenderState rectangle={.clear_rect_set=3,.clear_rect_h=(1u<<16)|1u,.clear_rect_v=(1u<<16)|1u};
    CHECK(ch_vk_nv2a_clear_panel_read(frame.eyes[0],&surface,&rectangle,0xf0,0xff00ff00,0,pixels,sizeof pixels)==VK_SUCCESS);
    /* Original 4x3 guest rectangle maps proportionally into a 37x19 eye. */
    for(unsigned y=0;y<19;y++) for(unsigned x=0;x<37;x++) {
        unsigned i=(y*37+x)*4;
        if(x>=10 && x<=17 && y>=7 && y<=11) CHECK(pixels[i]==0 && pixels[i+1]==255 && pixels[i+2]==0 && pixels[i+3]==255);
    }
    /* HUD must clear on next guest frame, and a movie/menu has no projection. */
    CHECK(ch_vk_scene_begin(scene,&view,surface.color_va)==VK_SUCCESS);
    CHECK(ch_vk_scene_clear(scene,&surface,&state,0xf3,0,0xffffff00)==VK_SUCCESS);
    CHECK(ch_vk_scene_draw(scene,&surface,&panel)==VK_SUCCESS);
    CHECK(ch_vk_scene_end(scene,surface.color_va,&frame)==VK_SUCCESS && !frame.immersive && frame.world_batches==0);
    CHECK(ch_vk_target_clear_aspects_read(hud_target,0,NULL,0,0,hp,sizeof hp)==VK_SUCCESS);
    CHECK(hp[(2*8+2)*4]==128 && hp[(2*8+2)*4+3]==128); /* cleared, not accumulated across frames */
    CHECK(ch_vk_scene_begin(scene,&view,surface.color_va)==VK_SUCCESS);
    CHECK(ch_vk_scene_draw(scene,&surface,&batch)==VK_SUCCESS);
    CHECK(ch_vk_scene_end(scene,surface.color_va,&frame)==VK_SUCCESS && !frame.immersive);
    CHECK(ch_vk_scene_begin(scene,&view,surface.color_va)==VK_SUCCESS);
    CHECK(ch_vk_scene_draw(scene,&surface,&batch)==VK_SUCCESS);
    unsigned char cpu_rgba[48];
    for(unsigned k=0;k<12;k++) {
        cpu_image[k]=0xff0b1621;
        cpu_rgba[k*4]=11; cpu_rgba[k*4+1]=22; cpu_rgba[k*4+2]=33; cpu_rgba[k*4+3]=255;
    }
    use_cpu_image=1;
    CHECK(ch_vk_scene_import(scene,&surface,cpu_rgba,sizeof cpu_rgba,0)==VK_SUCCESS);
    copy.texture=&native_rt;
    CHECK(ch_vk_scene_draw(scene,&auxiliary,&copy)==VK_SUCCESS);
    CHECK(ch_vk_scene_end(scene,auxiliary.color_va,&frame)==VK_SUCCESS && frame.immersive);
    for(unsigned eye=0;eye<2;eye++) {
        CHECK(ch_vk_target_clear_aspects_read(frame.eyes[eye],0,NULL,0,0,pixels,sizeof pixels)==VK_SUCCESS);
        for(unsigned k=0;k<37*19;k++) CHECK(pixels[k*4]==11 && pixels[k*4+1]==22 && pixels[k*4+2]==33 && pixels[k*4+3]==255);
    }
    CHECK(ch_vk_scene_begin(scene,&view,surface.color_va)==VK_SUCCESS);
    CHECK(ch_vk_scene_draw(scene,&surface,&batch)==VK_SUCCESS);
    CHECK(ch_vk_scene_import(scene,&surface,cpu_rgba,sizeof cpu_rgba,0)==VK_SUCCESS);
    CHECK(ch_vk_scene_end(scene,surface.color_va,&frame)==VK_SUCCESS && !frame.immersive);
    CHECK(ch_vk_scene_begin(scene,&view,surface.color_va)==VK_SUCCESS);
    CHECK(ch_vk_scene_draw(scene,&surface,&batch)==VK_SUCCESS);
    CHECK(ch_vk_scene_end(scene,surface.color_va,&frame)==VK_SUCCESS && frame.immersive);
    CHECK(ch_vk_target_clear_aspects_read(frame.eyes[0],0,NULL,0,0,pixels,sizeof pixels)==VK_SUCCESS);
    CHECK(ch_vk_target_clear_aspects_read(frame.eyes[1],0,NULL,0,0,other,sizeof other)==VK_SUCCESS && memcmp(pixels,other,sizeof pixels));
    CHECK(pixels[0]==11 && pixels[1]==22 && pixels[2]==33 && pixels[3]==255);
    CHECK(ch_vk_scene_begin(scene,&view,surface.color_va)==VK_SUCCESS);
    Nv2aSurface seeded=surface; seeded.color_va=4096;
    CHECK(ch_vk_scene_import(scene,&seeded,cpu_rgba,sizeof cpu_rgba,1)==VK_SUCCESS);
    CHECK(ch_vk_scene_draw(scene,&seeded,&batch)==VK_SUCCESS);
    CHECK(ch_vk_scene_end(scene,seeded.color_va,&frame)==VK_SUCCESS && frame.immersive);
    CHECK(ch_vk_target_clear_aspects_read(frame.eyes[0],0,NULL,0,0,pixels,sizeof pixels)==VK_SUCCESS);
    CHECK(pixels[0]==11 && pixels[1]==22 && pixels[2]==33 && pixels[3]==255);
    CHECK(ch_vk_scene_begin(scene,&view,surface.color_va)==VK_SUCCESS);
    Nv2aTexture rt=native_rt; rt.offset=surface.color_va+4; batch.texture=&rt;
    CHECK(ch_vk_scene_draw(scene,&surface,&batch)==VK_ERROR_FEATURE_NOT_PRESENT);
    rt.offset=surface.color_va; rt.pitch+=4;
    CHECK(ch_vk_scene_draw(scene,&surface,&batch)==VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(ch_vk_scene_end(scene,surface.color_va,&frame)==VK_SUCCESS);
    if(!failed) puts("PASS: scene routing/projection, native per-eye RT copy, multi-stage combiner sampling, self-feedback snapshots, CPU-owned source decoding/presentation/recovery, scaled HUD coverage and transitions");
done:
    ch_vk_scene_destroy(scene);return failed;
}
