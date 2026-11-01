/* Called with the real GLES pbuffer current; Vulkan shares only the GPU. */
#include <GLES3/gl3.h>
#include "vk_target.h"
#include "vk_nv2a.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
static GLuint shader(GLenum type,const char *source) {
    GLuint s=glCreateShader(type);glShaderSource(s,1,&source,NULL);glCompileShader(s);
    GLint ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok) {glDeleteShader(s);return 0;}return s;
}
static VkResult resolve_image(void *context,const Nv2aTexture *tx,ChVkSampleImage *out) {
    *out=(ChVkSampleImage){.gpu=context,.width=tx->width,.height=tx->height};return VK_SUCCESS;
}
int ch_test_mipmap_parity(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    enum {TW=64,TH=32,W=13,H=9};
    uint32_t argb[TW*TH];unsigned char rgba[TW*TH*4],reference[TW*TH*4],native[TW*TH*4];
    GLuint texture=0,fbo=0,program=0,vs=0,fs=0,vao=0;
    ChVkTexture *image=NULL;ChVkTarget *target=NULL;int failed=0;
#define REQUIRE(x) do {if(!(x)) {fprintf(stderr,"mipmap parity failure line %d: %s GL=%x\n",__LINE__,#x,glGetError());failed=1;goto done;}}while(0)
    for(unsigned y=0;y<TH;y++) for(unsigned x=0;x<TW;x++) {
        size_t i=y*TW+x;unsigned r=(x*31+y*17)&255,g=((x^y)&1)?255:0,b=(y*11+x*7)&255,a=64+((x*3+y*5)&127);
        argb[i]=(a<<24)|(r<<16)|(g<<8)|b;
        rgba[i*4]=r;rgba[i*4+1]=g;rgba[i*4+2]=b;rgba[i*4+3]=a;
    }
    REQUIRE(ch_vk_texture_upload_mips(gpu,device,queue,family,TW,TH,argb,7,&image)==VK_SUCCESS);
    REQUIRE(ch_vk_texture_levels(image)==7);
    glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,TW,TH,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba);glGenerateMipmap(GL_TEXTURE_2D);
    glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    for(unsigned level=0;level<7;level++) {
        unsigned width=TW>>level,height=TH>>level;if(!height) height=1;
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,level);
        REQUIRE(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
        glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,reference);
        REQUIRE(ch_vk_texture_read_level_rgba(image,level,native,sizeof native)==VK_SUCCESS);
        unsigned maximum=0;
        for(unsigned i=0;i<width*height*4;i++) {unsigned delta=(unsigned)abs(reference[i]-native[i]);if(delta>maximum) maximum=delta;}
        REQUIRE(glGetError()==GL_NO_ERROR && maximum<=3);
        printf("generated mip parity level=%u size=%ux%u maximum=%u\n",level,width,height,maximum);
    }
    REQUIRE(ch_vk_texture_read_level_rgba(image,7,native,sizeof native)==VK_ERROR_INITIALIZATION_FAILED);
    glBindFramebuffer(GL_FRAMEBUFFER,0);
    const char *vertex="#version 300 es\nprecision highp float;uniform float scale;uniform vec2 phase;out highp vec2 uv;void main(){vec2 p=vec2(gl_VertexID==1?3.0:-1.0,gl_VertexID==2?3.0:-1.0);gl_Position=vec4(p,0,1);uv=(p*.5+.5)*scale+phase;}";
    const char *fragment="#version 300 es\nprecision highp float;uniform highp sampler2D tex;in highp vec2 uv;out vec4 color;void main(){color=texture(tex,uv);}";
    vs=shader(GL_VERTEX_SHADER,vertex);fs=shader(GL_FRAGMENT_SHADER,fragment);REQUIRE(vs && fs);
    program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);
    GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);REQUIRE(linked);
    glGenVertexArrays(1,&vao);glBindVertexArray(vao);glUseProgram(program);
    glUniform1i(glGetUniformLocation(program,"tex"),0);glUniform2f(glGetUniformLocation(program,"phase"),.037f,.071f);glActiveTexture(GL_TEXTURE0);
    glBindSampler(0,0);glBindTexture(GL_TEXTURE_2D,texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    glViewport(0,0,W,H);glDisable(GL_DEPTH_TEST);glDisable(GL_STENCIL_TEST);glDisable(GL_BLEND);glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glDisable(GL_DITHER);
    REQUIRE(ch_vk_target_create(gpu,device,queue,family,W,H,&target)==VK_SUCCESS);
    const float black[]={0,0,0,0};const uint16_t indices[]={0,1,2,0,2,3};
    const GLenum filters[]={GL_NEAREST_MIPMAP_NEAREST,GL_LINEAR_MIPMAP_NEAREST,GL_NEAREST_MIPMAP_LINEAR,GL_LINEAR_MIPMAP_LINEAR};
    for(unsigned min=3;min<=6;min++) for(unsigned test=0;test<3;test++) {
        float scale=test==0?1:test==1?1.5f:4;
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,filters[min-3]);glUniform1f(glGetUniformLocation(program,"scale"),scale);
        glDrawArrays(GL_TRIANGLES,0,3);glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,reference);
        Nv2aTexture tx={.width=TW,.height=TH,.levels=7,.filter=0x02000000|(min<<16),.addr_u=1,.addr_v=1};
        VkSamplerCreateInfo sampler;REQUIRE(ch_vk_sampler_state(&tx,&sampler)==VK_SUCCESS && sampler.maxLod==6);
        REQUIRE(ch_vk_texture_set_sampler(image,&sampler)==VK_SUCCESS);
        /* Avoid exact texel-boundary ties: APIs may round interpolation
         * either way for nearest sampling at those discontinuities. */
        ChVkColorVertex quad[]={{{-1,-1,0,1,.037f,.071f},{1,1,1,1}},{{1,-1,0,1,scale+.037f,.071f},{1,1,1,1}},
            {{1,1,0,1,scale+.037f,scale+.071f},{1,1,1,1}},{{-1,1,0,1,.037f,scale+.071f},{1,1,1,1}}};
        Nv2aRenderState state={.color_mask=0x01010101};
        REQUIRE(ch_vk_target_clear_read(target,black,native,sizeof native)==VK_SUCCESS);
        REQUIRE(ch_vk_target_draw_texture_read(target,NULL,quad,4,indices,6,&state,image,native,sizeof native)==VK_SUCCESS);
        unsigned maximum=0;
        for(unsigned i=0;i<W*H*4;i++) {unsigned delta=(unsigned)abs(reference[i]-native[i]);if(delta>maximum) maximum=delta;}
        printf("mip sample parity min=%u scale=%.1f maximum=%u\n",min,scale,maximum);
        REQUIRE(glGetError()==GL_NO_ERROR && maximum<=3);
    }
    /* Reference GL passes through nonfinite shader coordinates. Compare the
     * actual driver outcome before choosing an adapter policy for those data. */
    for(unsigned filter=1;filter<=2;filter++) for(unsigned test=0;test<4;test++) {
        float uv[2]={.25f,.5f};uv[test&1]=test<2?NAN:INFINITY;
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,filter==1?GL_NEAREST:GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,filter==1?GL_NEAREST:GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,0);
        glUniform1f(glGetUniformLocation(program,"scale"),0);glUniform2fv(glGetUniformLocation(program,"phase"),1,uv);
        glDrawArrays(GL_TRIANGLES,0,3);glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,reference);
        Nv2aTexture tx={.width=TW,.height=TH,.filter=(filter<<24)|(filter<<16),.addr_u=1,.addr_v=1};
        VkSamplerCreateInfo sampler;REQUIRE(ch_vk_sampler_state(&tx,&sampler)==VK_SUCCESS);
        REQUIRE(ch_vk_texture_set_sampler(image,&sampler)==VK_SUCCESS);
        ChVkColorVertex quad[]={{{-1,-1,0,1},{1,1,1,1}},{{1,-1,0,1},{1,1,1,1}},
            {{1,1,0,1},{1,1,1,1}},{{-1,1,0,1},{1,1,1,1}}};
        for(unsigned i=0;i<4;i++) {quad[i].xy[4]=uv[0];quad[i].xy[5]=uv[1];}
        Nv2aRenderState state={.color_mask=0x01010101};
        REQUIRE(ch_vk_target_draw_texture_read(target,black,quad,4,indices,6,&state,image,native,sizeof native)==VK_SUCCESS);
        unsigned maximum=0;
        for(unsigned i=0;i<W*H*4;i++) {unsigned delta=(unsigned)abs(reference[i]-native[i]);if(delta>maximum) maximum=delta;}
        printf("nonfinite 2D sample parity filter=%u case=%u maximum=%u\n",filter,test,maximum);
        REQUIRE(glGetError()==GL_NO_ERROR && maximum<=3);
        /* The real decoded-batch adapter must preserve that reference behavior,
         * rather than aborting before the GPU sees nonfinite coordinates. */
        Nv2aVertex vertices[6]={{0,0,0,1,0xffffffff,0,0},{W,0,0,1,0xffffffff,0,0},{W,H,0,1,0xffffffff,0,0},
            {0,0,0,1,0xffffffff,0,0},{W,H,0,1,0xffffffff,0,0},{0,H,0,1,0xffffffff,0,0}};
        Nv2aVertexEx ex[6]={0};
        for(unsigned i=0;i<6;i++) {vertices[i].u=uv[0]*TW;vertices[i].v=uv[1]*TH;
            ex[i].tex[0][0]=uv[0];ex[i].tex[0][1]=uv[1];ex[i].tex[0][3]=1;}
        tx.color=0x06; /* Normalized Morton/DXT coordinate convention. */
        Nv2aCombiner rc={.control=1,.stage_program=1};
        rc.color_icw[0]=0x08040000;rc.alpha_icw[0]=0x18140000;rc.color_ocw[0]=rc.alpha_ocw[0]=0xc0;
        Nv2aSurface surface={.width=W,.height=H,.pitch=W*4,.bytes_per_pixel=4};
        Nv2aBatch batch={.vertices=vertices,.ext=ex,.count=6,.state=&state,.texture=&tx,.stage={&tx},.rc=&rc};
        REQUIRE(ch_vk_nv2a_draw_resolved_read(target,&surface,&batch,NULL,resolve_image,image,native,sizeof native)==VK_SUCCESS);
        for(unsigned i=0;i<W*H*4;i++) REQUIRE(abs(reference[i]-native[i])<=3);
    }
done:
    ch_vk_target_destroy(target);ch_vk_texture_destroy(image);
    glBindFramebuffer(GL_FRAMEBUFFER,0);glUseProgram(0);
    if(vao) glDeleteVertexArrays(1,&vao);
    if(program) glDeleteProgram(program);
    if(vs) glDeleteShader(vs);
    if(fs) glDeleteShader(fs);
    if(fbo) glDeleteFramebuffers(1,&fbo);
    if(texture) glDeleteTextures(1,&texture);
    if(!failed) puts("PASS: GLES/Vulkan generated mip levels and all four mip filters, 12 minified draws including fractional LOD");
    return failed;
#undef REQUIRE
}
