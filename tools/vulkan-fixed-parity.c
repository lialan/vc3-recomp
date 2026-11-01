/* Actual reference GL VS_FF against the native Vulkan vertex pipeline. */
#include <GLES3/gl3.h>
#include "vk_target.h"
#include "vk_nv2a.h"
#include "vk_backend.h"
#include "../nv2a_fixed_shader.h"
#include "fixed_gl_reference.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static GLuint compile(GLenum kind,const char *source) {
    GLuint shader=glCreateShader(kind);glShaderSource(shader,1,&source,NULL);glCompileShader(shader);
    GLint ok=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
    if(!ok) {char log[4096];glGetShaderInfoLog(shader,sizeof log,NULL,log);fprintf(stderr,"fixed reference shader: %s\n",log);glDeleteShader(shader);return 0;}
    return shader;
}
static GLint loc(GLuint p,const char *name) {return glGetUniformLocation(p,name);}
static void uniform_matrix(GLuint p,const char *name,unsigned count,const float *row_major) {
    float columns[4*16];
    for(unsigned n=0;n<count;n++) for(unsigned r=0;r<4;r++) for(unsigned c=0;c<4;c++) columns[n*16+c*4+r]=row_major[n*16+r*4+c];
    glUniformMatrix4fv(loc(p,name),count,GL_FALSE,columns);
}
static void uniforms(GLuint p,const ChVkFixedParams *v) {
    const Nv2aFixedState *f=&v->state;
    glUniform2fv(loc(p,"u_surf"),1,v->surface);glUniform2fv(loc(p,"u_depth"),1,v->depth);
    glUniform2fv(loc(p,"u_texsize"),1,v->texture);glUniform2fv(loc(p,"u_aa"),1,v->aa);
    glUniform2fv(loc(p,"u_uvs0"),1,v->uv_scale0);glUniform2f(loc(p,"u_xs"),1,0);
    glUniform2f(loc(p,"u_eye"),0,0);glUniform1i(loc(p,"u_st2"),0);glUniform2f(loc(p,"u_gw"),0,0);
    uniform_matrix(p,"u_comp",1,f->composite);uniform_matrix(p,"u_proj",1,f->proj);
    uniform_matrix(p,"u_mv",4,f->mv[0]);uniform_matrix(p,"u_tm",4,f->texmat[0]);
    glUniform4fv(loc(p,"u_c59"),1,f->c59);glUniform1ui(loc(p,"u_skin"),f->skin_mode);
    glUniform1i(loc(p,"u_usecomp"),f->use_composite);glUniform1i(loc(p,"u_lighting"),f->lighting);
    glUniform1i(loc(p,"u_flipn"),f->flip_normals);glUniform1i(loc(p,"u_hasnrm"),f->has_normal);
    glUniform1i(loc(p,"u_hascol"),f->has_color);glUniform2i(loc(p,"u_colmat"),f->colmat_amb,f->colmat_dif);
    glUniform4fv(loc(p,"u_samb"),1,f->scene_amb);glUniform1f(loc(p,"u_matalpha"),f->mat_alpha);
    GLint kinds[8],on[4],matrix[4];float scale[8*4];
    for(unsigned i=0;i<8;i++) kinds[i]=f->light_kind[i];
    for(unsigned i=0;i<4;i++) {on[i]=f->texgen_on[i];matrix[i]=f->texmat_on[i];}
    for(unsigned i=0;i<32;i++) scale[i]=1;
    glUniform1iv(loc(p,"u_lkind"),8,kinds);glUniform4fv(loc(p,"u_light"),40,f->light[0]);
    glUniform4uiv(loc(p,"u_texgen"),4,f->texgen[0]);glUniform1iv(loc(p,"u_tgon"),4,on);
    glUniform1iv(loc(p,"u_tmon"),4,matrix);glUniform4fv(loc(p,"u_ascale"),8,scale);
    glUniform1i(loc(p,"u_packed"),0);glUniform1i(loc(p,"u_bgra"),0);
    glUniform1f(loc(p,"u_vpz"),v->vp_off_z);glUniform1i(loc(p,"u_nearz"),v->near_z);
    glUniform1i(loc(p,"u_fogen"),v->fog_enable);glUniform1ui(loc(p,"u_fogmode"),v->fog_mode);
    glUniform2fv(loc(p,"u_fogp"),1,v->fog_param);
}
static void identity(float m[16]) {memset(m,0,16*sizeof(float));for(unsigned i=0;i<4;i++) m[i*5]=1;}
static int unused_decode(const Nv2aTexture *tx,uint32_t *pixels) {(void)tx;(void)pixels;return 0;}
static int registered_fixed(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    unsigned char guest[4096]={0};ChVkBackend *backend=NULL;int failed=1;
    ChVkBackendConfig config={.physical=gpu,.device=device,.queue=queue,.family=family,
        .guest_base=guest,.guest_bytes=sizeof guest,.decode=unused_decode,.raw_fixed=1,.precise_queries=1,.resident_color=1,.batch_draws=1};
    if(ch_vk_backend_create(&config,&backend)!=VK_SUCCESS || ch_vk_backend_attach(backend)!=VK_SUCCESS) goto done;
    const Nv2aBackend *cb=ch_vk_backend_callbacks();
    Nv2aFixedState ff={.use_composite=1,.tex0_slot=9};identity(ff.composite);
    ff.composite[0]=ff.composite[5]=8;ff.c59[0]=ff.c59[1]=8;
    const float positions[][4]={{-1,-1,.3f,1},{1,-1,.3f,1},{1,1,.3f,1},{-1,1,.3f,1}};
    const uint16_t strip[]={0,1,3,2},fan[]={0,1,2,3};
    Nv2aRawGeometry raw={.prim=6,.indices=strip,.index_count=4,.min_index=37,.max_index=40,
        .kind=NV2A_RAW_FIXED,.ff=&ff,.aa_sx=1,.aa_sy=1,.uv_scale0={1,1}};
    raw.attr[0]=(Nv2aAttrib){.data=positions,.type=2,.size=4,.stride=16,.bytes=sizeof positions};
    Nv2aSurface surface={.color_va=128,.width=16,.height=16,.pitch=64,.bytes_per_pixel=4};
    Nv2aRenderState state={.color_mask=0x01010101,.depth_max=1};uint32_t samples=0;
    Nv2aBatch batch={.raw=&raw,.state=&state,.zpass_out=&samples};
    for(unsigned primitive=6;primitive<=7;primitive++) {
        raw.prim=primitive;raw.indices=primitive==6?strip:fan;samples=0;
        cb->clear(&surface,&state,0xf3,0,0xffffff00);
        if(!cb->draw_raw || cb->draw_raw(&surface,&batch)!=1 || samples!=256 || !cb->flush) goto done;
        cb->flush();
        for(unsigned i=0;i<256;i++) {uint32_t pixel;memcpy(&pixel,guest+128+i*4,4);if(pixel!=0xffffffff) goto done;}
    }
    batch.zpass_out=NULL;ff.has_color=1;ff.color_slot=3;
    raw.attr[3]=(Nv2aAttrib){.value={1,0,0,1}};
    if(cb->draw_raw(&surface,&batch)!=1) goto done;
    ff.has_color=0; /* Shader constants must already have been snapshotted. */
    for(unsigned i=0;i<256;i++) {uint32_t pixel;memcpy(&pixel,guest+128+i*4,4);if(pixel!=0xffffffff) goto done;}
    cb->flush();
    for(unsigned i=0;i<256;i++) {uint32_t pixel;memcpy(&pixel,guest+128+i*4,4);if(pixel!=0xffff0000) goto done;}
    batch.zpass_out=&samples;
    raw.kind=NV2A_RAW_PROGRAM;samples=19;
    if(cb->draw_raw(&surface,&batch)!=0 || samples!=19) goto done;
    raw.kind=NV2A_RAW_FIXED;raw.attr[0].bytes=4;
    if(cb->draw_raw(&surface,&batch)!=0 || samples!=19) goto done;
    puts("PASS: registered raw fixed GPU callbacks, rebased strip/fan indices, precise query256, resident guest flush; unsupported program/bounds decline before drawing");failed=0;
done:
    ch_vk_backend_destroy(backend);return failed;
}
int ch_test_fixed_parity(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    enum {W=32,H=32};unsigned char reference[W*H*4],native[sizeof reference];
    ChVkTarget *target=NULL;GLuint vs=0,fs=0,program=0,vao=0,buffer=0;int failed=1;
#define REQUIRE(x) do {if(!(x)) {fprintf(stderr,"fixed parity line %d: %s GL=%x\n",__LINE__,#x,glGetError());goto done;}}while(0)
    VkPhysicalDeviceFeatures supported;vkGetPhysicalDeviceFeatures(gpu,&supported);
    if(!supported.shaderClipDistance || !supported.depthClamp) {puts("SKIP: fixed GPU parity requires clip distances/depth clamp");return 0;}
    vs=compile(GL_VERTEX_SHADER,CH_FF_REFERENCE);
    fs=compile(GL_FRAGMENT_SHADER,"#version 320 es\nprecision highp float;in vec4 v_col;in vec4 v_tc0,v_tc1,v_tc2,v_tc3;in float v_fog;uniform int mode;out vec4 color;void main(){color=mode==0?v_col:mode==1?clamp(v_tc0,0.0,1.0):mode==2?clamp(v_tc1,0.0,1.0):mode==3?clamp(v_tc2,0.0,1.0):mode==4?clamp(v_tc3,0.0,1.0):vec4(vec3(v_fog),1.0);}");
    REQUIRE(vs && fs);program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);
    GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);REQUIRE(linked);glUseProgram(program);
    glGenVertexArrays(1,&vao);glBindVertexArray(vao);glGenBuffers(1,&buffer);glBindBuffer(GL_ARRAY_BUFFER,buffer);
    for(unsigned i=0;i<8;i++) {glEnableVertexAttribArray(i);glVertexAttribPointer(i,4,GL_FLOAT,GL_FALSE,sizeof(ChVkFixedVertex),(void *)(uintptr_t)(i*16));}
    REQUIRE(ch_vk_target_create(gpu,device,queue,family,W,H,&target)==VK_SUCCESS);
    REQUIRE(ch_vk_target_enable_queries(target)==VK_SUCCESS);
    const float black[]={0,0,0,0};const uint16_t indices[]={0,1,2,0,2,3};
    glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,W,H);glDisable(GL_DEPTH_TEST);glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);glDisable(GL_CULL_FACE);glDisable(GL_DITHER);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glEnable(0x864f);glEnable(0x3000);glEnable(0x3001); // DEPTH_CLAMP and CLIP_DISTANCE0/1
    for(unsigned test=0;test<48;test++) {
        ChVkFixedParams p={.surface={W,H},.depth={0,1},.texture={1,1},.aa={1,1},.uv_scale0={1,1}};
        Nv2aFixedState *f=&p.state;
        identity(f->composite);f->composite[0]=W*.5f;f->composite[5]=H*.5f;f->composite[10]=.4f;
        identity(f->proj);f->proj[0]=W*.5f;f->proj[5]=H*.5f;f->proj[10]=.4f;
        f->c59[0]=W*.5f;f->c59[1]=H*.5f;f->has_color=1;f->has_normal=1;f->use_composite=1;
        for(unsigned k=0;k<4;k++) {identity(f->mv[k]);identity(f->texmat[k]);p.stage_scale[k][0]=p.stage_scale[k][1]=1;}
        ChVkFixedVertex vertices[4]={0};
        const float positions[4][2]={{-.67f,-.63f},{.61f,-.63f},{.61f,.73f},{-.67f,.73f}};
        for(unsigned i=0;i<4;i++) {
            float (*a)[4]=vertices[i].attributes;a[0][0]=positions[i][0];a[0][1]=positions[i][1];a[0][2]=.3f;a[0][3]=1;
            a[1][0]=.2f;a[1][1]=.3f;a[1][2]=.1f;a[1][3]=.4f;a[2][2]=1;a[2][3]=1;
            a[3][0]=.2f+i*.1f;a[3][1]=.7f-i*.1f;a[3][2]=.8f;a[3][3]=1;
            for(unsigned k=4;k<8;k++) {a[k][0]=.17f+i*.13f;a[k][1]=.23f+i*.11f;a[k][2]=.37f;a[k][3]=1;}
        }
        unsigned mode=0;
        if(test>=4 && test<11) {f->use_composite=0;f->skin_mode=test-4;for(unsigned k=0;k<4;k++) f->mv[k][3]=k*.073f;}
        if(test>=11 && test<20) {
            f->lighting=1;f->scene_amb[0]=.1f;f->scene_amb[1]=.05f;f->mat_alpha=.8f;
            f->colmat_amb=test&1;f->colmat_dif=(test>>1)&1;f->flip_normals=(test>>2)&1;
            for(unsigned k=0;k<(test==19?8u:1u);k++) {
                f->light_kind[k]=test&1?1:2;float *l=f->light[k];l[0]=.1f;l[1]=.05f;l[5]=.4f;l[6]=.6f;
                l[10]=1;l[14]=1;l[16]=1;l[3]=100;
            }
        }
        if(test>=20 && test<40) {
            mode=1+(test%4);unsigned k=mode-1;f->texgen_on[k]=1;
            for(unsigned c=0;c<3;c++) f->texgen[k][c]=test%3==0?0x8511:test%3==1?0x8512:0x2402;
            f->texmat_on[k]=test&1;f->texmat[k][0]=.73f;f->texmat[k][5]=.81f;f->texmat[k][3]=.13f;
        }
        if(test>=40 && test<44) {mode=5;p.fog_enable=1;p.fog_mode=0x800+(test-40);p.fog_param[0]=.8f;p.fog_param[1]=.13f;}
        if(test==44) {f->composite[12]=.4f;f->composite[15]=.7f;}
        if(test==45) {p.near_z=1;f->composite[8]=.8f;f->composite[11]=-.1f;}
        if(test==46) f->composite[15]=0;
        if(test==47) {f->composite[12]=.75f;f->composite[15]=.2f;}
        if(test==1) {p.aa[0]=p.aa[1]=2;f->composite[0]*=.5f;f->composite[5]*=.5f;f->c59[0]*=.5f;f->c59[1]*=.5f;}
        if(test==2) f->has_color=0;
        if(test==3) {f->composite[0]*=.83f;f->composite[3]=1.7f;f->composite[7]=-.9f;}
        uniforms(program,&p);glUniform1i(loc(program,"mode"),mode);
        glBufferData(GL_ARRAY_BUFFER,sizeof vertices,vertices,GL_STREAM_DRAW);glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_FAN,0,4);glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,reference);
        REQUIRE(ch_vk_target_clear_read(target,black,native,sizeof native)==VK_SUCCESS);
        Nv2aRenderState state={.color_mask=0x01010101,.depth_max=1};Nv2aCombiner rc={0};
        if(mode) {
            rc.control=1;rc.stage_program=mode<5?4u<<((mode-1)*5):0;
            unsigned reg=mode<5?8+mode-1:0x13;
            rc.color_icw[0]=(reg<<24)|0x00200000;rc.alpha_icw[0]=mode<5?((reg|0x10)<<24)|0x00200000:0x30200000;
            rc.color_ocw[0]=rc.alpha_ocw[0]=0xc0;
        }
        uint32_t samples=0;
        f->color_slot=3;f->tex0_slot=9;
        Nv2aRawGeometry raw={.prim=5,.indices=indices,.index_count=6,.min_index=17,.max_index=20,
            .kind=NV2A_RAW_FIXED,.ff=f,.aa_sx=p.aa[0],.aa_sy=p.aa[1],.near_z=p.near_z,.vp_off_z=p.vp_off_z,
            .fog_enable=p.fog_enable,.fog_mode=p.fog_mode,.uv_scale0={1,1}};
        memcpy(raw.fog_param,p.fog_param,sizeof raw.fog_param);
        const unsigned roles[]={0,1,2,3,9,10,11,12};
        for(unsigned i=0;i<8;i++) raw.attr[roles[i]]=(Nv2aAttrib){.data=vertices[0].attributes[i],
            .type=2,.size=4,.stride=sizeof(ChVkFixedVertex),.bytes=3*sizeof(ChVkFixedVertex)+16};
        Nv2aSurface surface={.width=W,.height=H,.pitch=W*4,.bytes_per_pixel=4};
        Nv2aBatch batch={.raw=&raw,.state=&state,.rc=mode?&rc:NULL,.zpass_out=&samples};
        ChVkFixedGeometry fixed;
        REQUIRE(ch_vk_fixed_prepare(&raw,&fixed)==VK_SUCCESS);
        VkResult draw_result=ch_vk_nv2a_draw_fixed_resolved_read(target,&surface,&batch,&fixed,NULL,NULL,NULL,native,sizeof native);
        ch_vk_fixed_release(&fixed);REQUIRE(draw_result==VK_SUCCESS);
        unsigned maximum=0,covered=0;
        for(unsigned i=0;i<sizeof native;i++) {unsigned delta=(unsigned)abs(reference[i]-native[i]);if(delta>maximum) maximum=delta;}
        for(unsigned i=0;i<W*H;i++) covered+=reference[i*4+3]!=0;
        printf("fixed vertex parity case=%u mode=%u maximum=%u samples=%u covered=%u\n",test,mode,maximum,samples,covered);
        if(maximum>3) {
            for(unsigned i=0;i<W*H;i++) if(memcmp(reference+i*4,native+i*4,4)) {
                fprintf(stderr,"fixed mismatch pixel%u ref=%u/%u/%u/%u native=%u/%u/%u/%u\n",i,
                    reference[i*4],reference[i*4+1],reference[i*4+2],reference[i*4+3],native[i*4],native[i*4+1],native[i*4+2],native[i*4+3]);break;
            }
        }
        REQUIRE(glGetError()==GL_NO_ERROR && maximum<=3 && samples==covered);
    }
    REQUIRE(registered_fixed(gpu,device,queue,family)==0);
    puts("PASS: reference GL/native Vulkan fixed vertex transforms, all skin modes, lighting/materials, four-stage texgen/matrices, fog, w/near clipping and precise visibility");failed=0;
done:
    ch_vk_target_destroy(target);glDisable(0x864f);glDisable(0x3000);glDisable(0x3001);glUseProgram(0);
    if(buffer) glDeleteBuffers(1,&buffer);
    if(vao) glDeleteVertexArrays(1,&vao);
    if(program) glDeleteProgram(program);
    if(vs) glDeleteShader(vs);
    if(fs) glDeleteShader(fs);
    return failed;
#undef REQUIRE
}
