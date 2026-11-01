#include <GLES3/gl3.h>
#include "vk_target.h"
#include "../nv2a_stage_fetch.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
static GLuint compile(GLenum type,const char *text) {
    GLuint s=glCreateShader(type);glShaderSource(s,1,&text,NULL);glCompileShader(s);
    GLint ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);if(!ok) {glDeleteShader(s);return 0;}return s;
}
int ch_test_cube_parity(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    enum {S=16,W=13,H=9};
    uint32_t argb[6*S*S];unsigned char rgba[6*S*S*4],readback[sizeof rgba],reference[W*H*4],native[W*H*4];
    ChVkTexture *cube=NULL;ChVkTarget *target=NULL;GLuint texture=0,program=0,vs=0,fs=0,vao=0;int failed=0;
#define REQUIRE(x) do {if(!(x)) {fprintf(stderr,"cube parity failure line %d: %s GL=%x\n",__LINE__,#x,glGetError());failed=1;goto done;}}while(0)
    for(unsigned f=0;f<6;f++) for(unsigned y=0;y<S;y++) for(unsigned x=0;x<S;x++) {
        size_t i=(f*S+y)*S+x;unsigned r=20+f*30+x*3,g=15+f*11+y*9,b=30+x*5+y*4,a=120+f*15;
        argb[i]=(a<<24)|(r<<16)|(g<<8)|b;rgba[i*4]=r;rgba[i*4+1]=g;rgba[i*4+2]=b;rgba[i*4+3]=a;
    }
    REQUIRE(ch_vk_texture_upload_cube(gpu,device,queue,family,S,argb,&cube)==VK_SUCCESS && ch_vk_texture_is_cube(cube));
    REQUIRE(ch_vk_texture_read_rgba(cube,readback,sizeof readback)==VK_SUCCESS && !memcmp(rgba,readback,sizeof rgba));
    Nv2aTexture tx={.cube=1,.width=S,.height=S,.levels=1};VkSamplerCreateInfo sampler;
    REQUIRE(ch_vk_sampler_state(&tx,&sampler)==VK_SUCCESS && sampler.maxLod==0 && sampler.minFilter==VK_FILTER_LINEAR);
    REQUIRE(ch_vk_texture_set_sampler(cube,&sampler)==VK_SUCCESS);
    glActiveTexture(GL_TEXTURE0);glBindSampler(0,0);glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_CUBE_MAP,texture);
    for(unsigned f=0;f<6;f++) glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+f,0,GL_RGBA8,S,S,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba+f*S*S*4);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_R,GL_CLAMP_TO_EDGE);
    vs=compile(GL_VERTEX_SHADER,"#version 300 es\nprecision highp float;uniform vec3 base,dx,dy;out highp vec3 dir;void main(){vec2 p=vec2(gl_VertexID==1?3.0:-1.0,gl_VertexID==2?3.0:-1.0);gl_Position=vec4(p,0,1);vec2 uv=p*.5+.5;dir=base+uv.x*dx+uv.y*dy;}");
    fs=compile(GL_FRAGMENT_SHADER,"#version 300 es\nprecision highp float;uniform highp uint mode;uniform highp samplerCube tex;in highp vec3 dir;out vec4 color;\n" CH_NV_STAGE_FETCH_SOURCE "\nvoid main(){color=ch_nv_stage_fetch(mode,vec4(dir,1),texture(tex,dir),1u);}");
    REQUIRE(vs && fs);program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);
    GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);REQUIRE(linked);
    glUseProgram(program);glUniform1i(glGetUniformLocation(program,"tex"),0);glGenVertexArrays(1,&vao);glBindVertexArray(vao);
    glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,W,H);glDisable(GL_DEPTH_TEST);glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);glDisable(GL_CULL_FACE);glDisable(GL_DITHER);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    REQUIRE(ch_vk_target_create(gpu,device,queue,family,W,H,&target)==VK_SUCCESS);
    Nv2aCombiner rc={.control=1,.stage_program=3};rc.color_icw[0]=0x08040000;rc.alpha_icw[0]=0x18140000;
    rc.color_ocw[0]=rc.alpha_ocw[0]=0xc0;
    const float cases[][9]={
        {1,-.4f,-.4f,0,.8f,0,0,0,.8f},{-1,-.4f,-.4f,0,.8f,0,0,0,.8f},
        {-.4f,1,-.4f,.8f,0,0,0,0,.8f},{-.4f,-1,-.4f,.8f,0,0,0,0,.8f},
        {-.4f,-.4f,1,.8f,0,0,0,.8f,0},{-.4f,-.4f,-1,.8f,0,0,0,.8f,0},
        {1,.8f,-.4f,0,.4f,0,0,0,.8f},{1,-.4f,.8f,0,0,.4f,0,.8f,0},
        {1,.85f,.85f,0,.3f,0,0,0,.3f},{-1,-.85f,-.85f,0,-.3f,0,0,0,-.3f}};
    const float black[]={0,0,0,0};const uint16_t indices[]={0,1,2,0,2,3};
    for(unsigned test=0;test<10+32+2+3;test++) {
        unsigned mode=test<10?3:test<42?test-10:test==42?0:test==43?6:test-43;
        rc.stage_program=mode;glUniform1ui(glGetUniformLocation(program,"mode"),mode);
        const float unused[]={INFINITY,NAN,-INFINITY,0,0,0,0,0,0};
        const float *p=test>=42?unused:cases[test<10?test:0];
        glUniform3fv(glGetUniformLocation(program,"base"),1,p);glUniform3fv(glGetUniformLocation(program,"dx"),1,p+3);glUniform3fv(glGetUniformLocation(program,"dy"),1,p+6);
        glDrawArrays(GL_TRIANGLES,0,3);glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,reference);
        ChVkColorVertex quad[]={{{-1,-1,0,1},{1,1,1,1}},{{1,-1,0,1},{1,1,1,1}},{{1,1,0,1},{1,1,1,1}},{{-1,1,0,1},{1,1,1,1}}};
        for(unsigned i=0;i<4;i++) for(unsigned k=0;k<3;k++)
            quad[i].xy[11+k]=p[k]+(i==1 || i==2?p[3+k]:0)+(i>=2?p[6+k]:0);
        for(unsigned i=0;i<4;i++) quad[i].xy[14]=1;
        Nv2aRenderState state={.color_mask=0x01010101};const ChVkTexture *stages[4]={cube};
        REQUIRE(ch_vk_target_clear_read(target,black,native,sizeof native)==VK_SUCCESS);
        REQUIRE(ch_vk_target_draw_combiner_read(target,NULL,quad,4,indices,6,&state,NULL,NULL,&rc,0,stages,native,sizeof native)==VK_SUCCESS);
        unsigned maximum=0;
        for(unsigned i=0;i<sizeof native;i++) {unsigned delta=(unsigned)abs(reference[i]-native[i]);if(delta>maximum) maximum=delta;}
        printf("cube directional parity case=%u mode=%u maximum=%u\n",test,mode,maximum);
        REQUIRE(glGetError()==GL_NO_ERROR && maximum<=3);
    }
done:
    ch_vk_target_destroy(target);ch_vk_texture_destroy(cube);glUseProgram(0);
    if(vao) glDeleteVertexArrays(1,&vao);
    if(program) glDeleteProgram(program);
    if(vs) glDeleteShader(vs);
    if(fs) glDeleteShader(fs);
    if(texture) glDeleteTextures(1,&texture);
    if(!failed) puts("PASS: GLES/Vulkan cube sampling, six orientations and seamless edges/corners; shared stage contribution for all 32 modes");
    return failed;
#undef REQUIRE
}
