#!/usr/bin/env python3
"""Execute actual DXT decoder GLSL against the shared CPU decoder (macOS GL)."""
import ast
from pathlib import Path
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/src/gpu/gl_texdec.c').read_text()
text = source.split('static const char DECODE_GLSL[] =', 1)[1].split('\n#define GLSL_HEAD', 1)[0]
# Select desktop storage convention from the C conditional around texel().
text = text.replace('    "    return v.bgra;\\n"\n', '')
decode = ''.join(ast.literal_eval(x) for x in re.findall(r'"(?:[^"\\]|\\.)*"', text))
fragment = '#version 410 core\nout vec4 o;\n' + decode + '\nvoid main(){o=texel(uint(gl_FragCoord.x),uint(gl_FragCoord.y));}\n'
harness = r'''
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "d3d8_swizzle.h"
static GLuint shader(GLenum stage, const char *src) {
    GLuint s = glCreateShader(stage);
    GLint ok;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096]; glGetShaderInfoLog(s,sizeof log,NULL,log); puts(log); exit(1); }
    return s;
}
static uint32_t seed=12345;
static uint32_t random32(void) { seed=1664525*seed+1013904223; return seed; }
int main(int argc, char **argv) {
    (void)argc;
    CGLPixelFormatAttribute attrs[]={kCGLPFAOpenGLProfile,(CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,0};
    CGLPixelFormatObj pf; CGLContextObj ctx; GLint np;
    assert(CGLChoosePixelFormat(attrs,&pf,&np)==kCGLNoError);
    assert(CGLCreateContext(pf,NULL,&ctx)==kCGLNoError);
    CGLDestroyPixelFormat(pf); CGLSetCurrentContext(ctx);
    FILE *f=fopen(argv[1],"rb"); assert(f); fseek(f,0,SEEK_END); long len=ftell(f); rewind(f);
    char *src=calloc((size_t)len+1,1); assert(fread(src,1,(size_t)len,f)==(size_t)len); fclose(f);
    const char *vs="#version 410\nvoid main(){vec2 p=vec2(float((gl_VertexID&1)*4-1),float((gl_VertexID&2)*2-1));gl_Position=vec4(p,0,1);}";
    GLuint prog=glCreateProgram(); glAttachShader(prog,shader(GL_VERTEX_SHADER,vs)); glAttachShader(prog,shader(GL_FRAGMENT_SHADER,src));
    glLinkProgram(prog); GLint ok; glGetProgramiv(prog,GL_LINK_STATUS,&ok); assert(ok); glUseProgram(prog);
#define LOC(n) glGetUniformLocation(prog,n)
    enum { W=128,H=128, BLOCKS=W*H/16 };
    GLuint vao,fbo,target,buffer,tbo;
    glGenVertexArrays(1,&vao); glBindVertexArray(vao);
    glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glGenTextures(1,&target); glBindTexture(GL_TEXTURE_2D,target);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,W,H,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target,0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
    glGenBuffers(1,&buffer); glBindBuffer(GL_TEXTURE_BUFFER,buffer); glBufferData(GL_TEXTURE_BUFFER,4096,NULL,GL_STREAM_DRAW);
    glGenTextures(1,&tbo); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_BUFFER,tbo); glTexBuffer(GL_TEXTURE_BUFFER,GL_R32UI,buffer);
    glUniform1i(LOC("u_raw"),0); glUniform1ui(LOC("u_w"),W);
    glViewport(0,0,W,H); glDisable(GL_DITHER);
    uint32_t words[BLOCKS*4], result[W*H];
    for (int fmt=0x0C;fmt<=0x0F;fmt++) {
        if(fmt==0x0D) continue;
        unsigned bytes=fmt==0x0C?8:16;
        for(unsigned i=0;i<BLOCKS*bytes/4;i++) words[i]=random32();
        /* Deterministic palettes cover endpoint order, all selectors, alpha
         * extremes and DXT1's transparent entry alongside random blocks. */
        for(unsigned b=0;b<16;b++) {
            uint8_t *p=(uint8_t *)words+b*bytes;
            for(unsigned j=0;j<bytes;j++) p[j]=0;
            if(bytes==16) {p[0]=b&1?0:255;p[1]=b&1?255:0;for(int j=2;j<8;j++)p[j]=(uint8_t)(j*37);p+=8;}
            uint16_t e0=b&1?0:0xffff,e1=b&1?0xffff:0;
            memcpy(p,&e0,2);memcpy(p+2,&e1,2);uint32_t idx=0xe4e4e4e4;memcpy(p+4,&idx,4);
        }
        glBufferData(GL_TEXTURE_BUFFER,BLOCKS*bytes,words,GL_STREAM_DRAW);
        glUniform1i(LOC("u_fmt"),fmt); glDrawArrays(GL_TRIANGLES,0,3);
        glReadPixels(0,0,W,H,GL_BGRA,GL_UNSIGNED_INT_8_8_8_8_REV,result);
        assert(glGetError()==GL_NO_ERROR);
        unsigned bad=0;
        for(unsigned y=0;y<H;y++) for(unsigned x=0;x<W;x++) {
            uint32_t expected;
            assert(d3d8_dxt_decode_texel((const uint8_t *)words,(uint32_t)fmt,x,y,W,&expected));
            if(result[y*W+x]!=expected) { if(bad++<3) printf("fmt%02X (%u,%u) expected%08X got%08X\n",fmt,x,y,expected,result[y*W+x]); }
        }
        assert(bad==0);
        printf("DXT fmt%02X actual GLSL vs CPU: %u texels PASS\n",fmt,W*H);
    }
    CGLSetCurrentContext(NULL); CGLDestroyContext(ctx); free(src); return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='dxt-decode-') as temp:
    temp = Path(temp)
    (temp/'decode.glsl').write_text(fragment)
    (temp/'test.c').write_text(harness)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-uninitialized',
                    '-I',str(root/'external/xboxrecomp/src/d3d'),str(temp/'test.c'),
                    '-framework','OpenGL','-o',str(temp/'test')],check=True)
    subprocess.run([str(temp/'test'),str(temp/'decode.glsl')],check=True)
