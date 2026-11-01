#include "vk_submission.h"
#include <stdio.h>
#include <string.h>
struct ViewState { uint64_t next; };
static VkResult latch_test_view(void *context,Vc3VrView *view) {
    struct ViewState *state=context; memset(view,0,sizeof(*view)); view->on=1; view->id=state->next++;
    const float q[]={0,0,0,1},fov[]={-1,1,1,-1};
    for(unsigned eye=0;eye<2;eye++) { float p[]={eye?.3f:-.3f,0,0}; vr_eye_matrix(q,p,fov,1,.1f,100,view->vp[eye]); view->pose_q[eye][3]=1; view->pose_p[eye][0]=p[0]; }
    return VK_SUCCESS;
}
int ch_test_submission(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family,
    void *guest,size_t guest_bytes,ChVkDecodeTexture decode) {
    ChVkFrameMailbox *frames=NULL; ChVkSubmission *submission=NULL;
    ChVkCpuFrame first={0},stereo={0},edited={0}; struct ViewState views={.next=10}; int failed=0;
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"submission check failed line %d\n",__LINE__); failed=1; goto done; } } while(0)
    CHECK(ch_vk_frame_mailbox_create(&frames)==VK_SUCCESS);
    ChVkSubmissionConfig config={.backend={.physical=gpu,.device=device,.queue=queue,.family=family,
        .guest_base=guest,.guest_bytes=guest_bytes,.decode=decode},.eye_width=37,.eye_height=19,.hud_width=8,.hud_height=6,
        .frames=frames,.view_context=&views,.latch_view=latch_test_view};
    CHECK(ch_vk_submission_create(&config,&submission)==VK_SUCCESS);
    CHECK(ch_vk_submission_attach(submission)==VK_SUCCESS);
    const Nv2aBackend *cb=ch_vk_backend_callbacks();
    Nv2aSurface surface={.color_va=2048,.width=4,.height=3,.pitch=16,.bytes_per_pixel=4,.aa_sx=1,.aa_sy=1};
    Nv2aRenderState state={.color_mask=0x01010101};
    Nv2aFixedState ff={0}; ff.proj[0]=2;ff.proj[5]=-2;ff.proj[10]=1;ff.proj[14]=1;ff.c59[0]=2;ff.c59[1]=1.5f;
    Nv2aRawGeometry raw={.kind=NV2A_RAW_FIXED,.ff=&ff}; Nv2aBatch metadata={.raw=&raw};
    Nv2aVertex world[]={
        {.x=1.25f,.y=2.25f,.rhw=.5f,.diffuse=0xffff0000},
        {.x=2.75f,.y=2.25f,.rhw=.5f,.diffuse=0xffff0000},
        {.x=2,.y=.75f,.rhw=.5f,.diffuse=0xffff0000}};
    Nv2aBatch batch={.vertices=world,.count=3,.state=&state};
    nv2a_backend_presented_override(1,surface.color_va);
    cb->clear(&surface,&state,0xf3,0xff0000ff,0xffffff00);
    CHECK(cb->draw_raw && cb->draw_raw(&surface,&metadata)==0); cb->draw(&surface,&batch); cb->flip();
    CHECK(ch_vk_frame_mailbox_take(frames,&first)==VK_SUCCESS && first.sequence==1 && !first.immersive && first.bytes==48 && !first.eyes[0].rgba);
    unsigned char retained_flat[48]; memcpy(retained_flat,first.rgba,48);
    cb->clear(&surface,&state,0xf3,0xff0000ff,0xffffff00); cb->draw(&surface,&batch);
    Nv2aVertex hud[]={
        {.x=1,.y=1,.rhw=1,.diffuse=0x80ff0000},{.x=3,.y=1,.rhw=1,.diffuse=0x80ff0000},{.x=3,.y=2,.rhw=1,.diffuse=0x80ff0000},
        {.x=1,.y=1,.rhw=1,.diffuse=0x80ff0000},{.x=3,.y=2,.rhw=1,.diffuse=0x80ff0000},{.x=1,.y=2,.rhw=1,.diffuse=0x80ff0000}};
    Nv2aRenderState blend={.color_mask=0x01010101,.blend_enable=1,.blend_src=0x302,.blend_dst=0x303};
    Nv2aBatch panel={.vertices=hud,.count=6,.state=&blend}; cb->draw(&surface,&panel); cb->flip();
    CHECK(ch_vk_frame_mailbox_take(frames,&stereo)==VK_SUCCESS && stereo.sequence==2 && stereo.immersive && stereo.view.id==11 && stereo.projection.kx==2);
    CHECK(stereo.eyes[0].width==37 && stereo.eyes[0].height==19 && stereo.eyes[0].bytes==37*19*4 &&
        stereo.eyes[1].bytes==stereo.eyes[0].bytes && memcmp(stereo.eyes[0].rgba,stereo.eyes[1].rgba,stereo.eyes[0].bytes));
    CHECK(stereo.hud.width==8 && stereo.hud.height==6 && stereo.hud.rgba[(2*8+2)*4+3]==128);
    CHECK(!memcmp(first.rgba,retained_flat,48));
    unsigned char retained_eye[37*19*4]; memcpy(retained_eye,stereo.eyes[0].rgba,sizeof retained_eye);
    uint32_t cpu_pixel=0xff123456; memcpy((unsigned char *)guest+surface.color_va,&cpu_pixel,4); cb->flip();
    CHECK(ch_vk_frame_mailbox_take(frames,&edited)==VK_SUCCESS && edited.sequence==3 && !edited.immersive &&
        edited.rgba[0]==0x12 && edited.rgba[1]==0x34 && edited.rgba[2]==0x56);
    CHECK(!memcmp(stereo.eyes[0].rgba,retained_eye,sizeof retained_eye) && stereo.view.id==11);
    ch_vk_submission_destroy(submission); submission=NULL;
    ch_vk_frame_mailbox_destroy(frames); frames=NULL;
    CHECK(!memcmp(first.rgba,retained_flat,48) && !memcmp(stereo.eyes[0].rgba,retained_eye,sizeof retained_eye));
    puts("PASS: registered submission pipeline -> atomic flat/stereo/HUD/render-pose packet, next-frame latch, CPU edits and retained ownership through teardown");
done:
    nv2a_backend_presented_override(0,0);
    ch_vk_submission_destroy(submission); ch_vk_frame_mailbox_destroy(frames);
    ch_vk_cpu_frame_release(&first); ch_vk_cpu_frame_release(&stereo); ch_vk_cpu_frame_release(&edited);
    return failed;
}
