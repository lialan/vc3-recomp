/* Real reference GLES renderer vs Vulkan on the same GPU. Pbuffer only. */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include "vk_overlay.h"
#include "../runtime/src/chihiro/chihiro_profile.h"
#include "../runtime/src/host/vr_math.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Isolated profile: fallback sight heights, as used by the reference geometry. */
const ChihiroProfile *chihiro_profile(void) { static const ChihiroProfile p={0}; return &p; }
int ch_vk_overlay_parity(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    enum {W=320,H=256}; const size_t bytes=W*H*4;
    EGLDisplay display=eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLContext context=EGL_NO_CONTEXT; EGLSurface surface=EGL_NO_SURFACE;
    ChVkTarget *target=NULL; unsigned char *glpixels=malloc(bytes),*vkpixels=malloc(bytes);
    int failed=1; EGLConfig config; EGLint count;
    if(!glpixels || !vkpixels || display==EGL_NO_DISPLAY || !eglInitialize(display,NULL,NULL)) goto done;
    const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_DEPTH_SIZE,24,EGL_NONE};
    const EGLint ctx[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
    const EGLint surf[]={EGL_WIDTH,W,EGL_HEIGHT,H,EGL_NONE};
    if(!eglBindAPI(EGL_OPENGL_ES_API) || !eglChooseConfig(display,attrs,&config,1,&count) || count!=1) goto done;
    context=eglCreateContext(display,config,EGL_NO_CONTEXT,ctx);
    surface=eglCreatePbufferSurface(display,config,surf);
    if(context==EGL_NO_CONTEXT || surface==EGL_NO_SURFACE || !eglMakeCurrent(display,surface,surface,context)) goto done;
    printf("overlay reference GL=%s renderer=%s\n",glGetString(GL_VERSION),glGetString(GL_RENDERER));
    if(!qo_init() || ch_vk_target_create(gpu,device,queue,family,W,H,&target)!=VK_SUCCESS) goto done;
    glViewport(0,0,W,H); glDisable(GL_DITHER);
    float projection[16]; vr_proj_fov(-.7f,.7f,.7f,-.7f,.01f,10,projection);
    failed=0;
    for(unsigned sight=0;sight<QO_SIGHT_COUNT;sight++) for(unsigned mode=0;mode<4;mode++)
    for(unsigned shifted=0;shifted<2;shifted++) for(unsigned recoil=0;recoil<2;recoil++) {
        float eye[3]={shifted?.025f:0,shifted?.01f:0,0},vp[16]; memcpy(vp,projection,sizeof vp);
        /* Translate view by the eye, retaining asymmetric eye-target geometry. */
        for(unsigned k=0;k<4;k++) vp[12+k]-=vp[k]*eye[0]+vp[4+k]*eye[1]+vp[8+k]*eye[2];
        QoFrame f={.gun_valid=1,.show_gun=mode!=2,.gun_q={0,0,0,1},.gun_p={.04f,-.01f,-.35f},
            .end={.04f,.02f,-2},.sight=(int)sight,.kick=recoil?.8f:0,
            .beam=mode==1 || mode==2,.dot=mode==1 || mode==2,.flash=mode==1 || mode==2};
        if(shifted) { f.gun_q[0]=.02f; f.gun_q[1]=.08f; f.gun_q[2]=-.03f; f.gun_q[3]=sqrtf(1-.02f*.02f-.08f*.08f-.03f*.03f); }
        const float bg[4]={mode==3?.05f:0,mode==3?.1f:0,mode==3?.15f:0,mode==3?.5f:0};
        glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE); glDepthMask(GL_TRUE);
        glClearColor(bg[0],bg[1],bg[2],bg[3]); glClearDepthf(1); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        qo_draw(&f,vp,eye); glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,glpixels);
        if(glGetError()!=GL_NO_ERROR || ch_vk_target_clear_read(target,bg,vkpixels,bytes)!=VK_SUCCESS ||
            ch_vk_overlay_read(target,&f,vp,eye,qo_sight_height((int)sight),vkpixels,bytes)!=VK_SUCCESS) {failed=1;goto done;}
        unsigned bad=0,maxdelta=0,covered=0;
        for(unsigned y=0;y<H;y++) for(unsigned x=0;x<W;x++) {
            const unsigned char *a=glpixels+((H-1-y)*W+x)*4,*b=vkpixels+(y*W+x)*4;
            int mismatch=0; if(a[3] || b[3]) covered++;
            for(unsigned c=0;c<4;c++) { unsigned d=(unsigned)abs((int)a[c]-(int)b[c]); if(d>maxdelta) maxdelta=d; if(d>3) mismatch=1; }
            bad+=(unsigned)mismatch;
        }
        printf("overlay parity sight=%u mode=%u eye=%u recoil=%u covered=%u bad=%u max=%u\n",sight,mode,shifted,recoil,covered,bad,maxdelta);
        if(bad) {
            failed=1;
            FILE *out=fopen("/data/local/tmp/vc3-overlay-gl.rgba","wb"); if(out) {fwrite(glpixels,1,bytes,out);fclose(out);}
            out=fopen("/data/local/tmp/vc3-overlay-vk.rgba","wb"); if(out) {fwrite(vkpixels,1,bytes,out);fclose(out);}
        }
    }
    if(!failed) puts("PASS: actual GLES/Vulkan overlay readbacks, 48 cases, all pixels RGBA within 3 UNORM steps");
    if(!failed) {
        extern int ch_test_mipmap_parity(VkPhysicalDevice,VkDevice,VkQueue,uint32_t);
        failed=ch_test_mipmap_parity(gpu,device,queue,family);
        extern int ch_test_cube_parity(VkPhysicalDevice,VkDevice,VkQueue,uint32_t);
        if(!failed) failed=ch_test_cube_parity(gpu,device,queue,family);
        extern int ch_test_fixed_parity(VkPhysicalDevice,VkDevice,VkQueue,uint32_t);
        if(!failed) failed=ch_test_fixed_parity(gpu,device,queue,family);
    }
done:
    if(failed) fprintf(stderr,"FAIL: GLES/Vulkan overlay parity (EGL error 0x%x)\n",eglGetError());
    ch_vk_target_destroy(target);
    if(display!=EGL_NO_DISPLAY) { eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        if(surface!=EGL_NO_SURFACE) eglDestroySurface(display,surface);
        if(context!=EGL_NO_CONTEXT) eglDestroyContext(display,context); eglTerminate(display); }
    free(glpixels);free(vkpixels);return failed;
}
