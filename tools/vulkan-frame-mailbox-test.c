#include "vk_frame_mailbox.h"
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
struct Producer { ChVkFrameMailbox *box; atomic_int done,failed; };
static void *produce(void *context) {
    struct Producer *p=context;
    Nv2aSurface s={.width=16,.height=16,.pitch=64,.bytes_per_pixel=4};
    unsigned char rgba[1024],left[64],right[64],hud[16];
    for(unsigned i=1;i<=1000;i++) {
        memset(rgba,(int)(i&255),sizeof rgba); s.color_va=i;
        VkResult result;
        if(i&1) {
            memset(left,(int)((i+1)&255),sizeof left); memset(right,(int)((i+2)&255),sizeof right); memset(hud,(int)((i+3)&255),sizeof hud);
            ChVkCpuFrame frame={.surface=s,.bytes=sizeof rgba,.rgba=rgba,.immersive=1,
                .view={.on=1,.id=i},.projection={.kx=(float)i},
                .eyes={{4,4,sizeof left,left},{4,4,sizeof right,right}},.hud={2,2,sizeof hud,hud}};
            result=ch_vk_frame_mailbox_publish_frame(p->box,&frame);
        } else result=ch_vk_frame_mailbox_publish(p->box,&s,rgba,sizeof rgba);
        if(result!=VK_SUCCESS) { atomic_store(&p->failed,1); break; }
        memset(left,0xcc,sizeof left); memset(right,0xcc,sizeof right); memset(hud,0xcc,sizeof hud);
        memset(rgba,0xcc,sizeof rgba); /* Producer's borrowed storage is reused immediately. */
    }
    atomic_store(&p->done,1); return NULL;
}
int ch_test_frame_mailbox(void) {
    struct Producer p={0}; pthread_t thread;
    if(ch_vk_frame_mailbox_create(&p.box)!=VK_SUCCESS) return 1;
    if(pthread_create(&thread,NULL,produce,&p)) { ch_vk_frame_mailbox_destroy(p.box); return 1; }
    int failed=0; uint64_t sequence=0; unsigned consumed=0;
    for(;;) {
        int producer_done=atomic_load(&p.done);
        ChVkCpuFrame frame={0}; VkResult r=ch_vk_frame_mailbox_take(p.box,&frame);
        if(r==VK_NOT_READY) { if(producer_done) break; sched_yield(); continue; }
        if(r!=VK_SUCCESS) { failed=1;break; }
        if(frame.sequence<=sequence || frame.surface.color_va!=frame.sequence || frame.bytes!=1024) failed=1;
        for(size_t k=0;k<frame.bytes;k++) if(frame.rgba[k]!=(frame.sequence&255)) {failed=1;break;}
        if(frame.immersive!=(int)(frame.sequence&1)) failed=1;
        if(frame.immersive) {
            if(frame.view.id!=frame.sequence || frame.projection.kx!=(float)frame.sequence) failed=1;
            const ChVkCpuImage *planes[]={&frame.eyes[0],&frame.eyes[1],&frame.hud};
            for(unsigned j=0;j<3;j++) for(size_t k=0;k<planes[j]->bytes;k++)
                if(planes[j]->rgba[k]!=((frame.sequence+j+1)&255)) failed=1;
        }
        sequence=frame.sequence; consumed++;
        ChVkCpuFrame occupied=frame;
        if(ch_vk_frame_mailbox_take(p.box,&occupied)!=VK_ERROR_INITIALIZATION_FAILED) failed=1;
        ch_vk_cpu_frame_release(&frame);
    }
    pthread_join(thread,NULL);
    if(!consumed || sequence!=1000 || atomic_load(&p.failed)) failed=1;
    /* A retained consumer frame survives mailbox teardown. */
    ChVkCpuFrame retained={0}; unsigned char rgba[4]={1,2,3,4};
    Nv2aSurface s={.width=1,.height=1,.pitch=4,.bytes_per_pixel=4};
    if(ch_vk_frame_mailbox_publish(p.box,&s,rgba,4)!=VK_SUCCESS || ch_vk_frame_mailbox_take(p.box,&retained)!=VK_SUCCESS) failed=1;
    ch_vk_frame_mailbox_destroy(p.box);
    if(!retained.rgba || memcmp(retained.rgba,rgba,4)) failed=1;
    ch_vk_cpu_frame_release(&retained);
    if(!failed) printf("PASS: concurrent atomic flat/stereo/HUD/pose frame mailbox, %u/1000 latest frames consumed, owned snapshots survive producer reuse/teardown\n",consumed);
    return failed;
}
