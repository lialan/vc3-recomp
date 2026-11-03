#include "vk_gpu_mailbox.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
struct ChVkGpuMailbox {
    pthread_mutex_t lock;
    struct { ChVkTexture *image; uint32_t w,h; int held; } slots[CH_VK_GPU_MAILBOX_SLOTS];
    ChVkGpuFrame pending[2]; unsigned waiting,depth;   /* oldest first */
    uint64_t sequence;
    VkBuffer buffers[CH_VK_GPU_MAILBOX_SLOTS]; VkDeviceSize capacity;   /* bridge mode: a buffer per slot */
};
void ch_vk_gpu_mailbox_set_buffers(ChVkGpuMailbox *b,const VkBuffer buffers[CH_VK_GPU_MAILBOX_SLOTS],VkDeviceSize capacity) {
    if(!b) return;
    for(unsigned i=0;i<CH_VK_GPU_MAILBOX_SLOTS;i++) b->buffers[i]=buffers?buffers[i]:VK_NULL_HANDLE;
    b->capacity=buffers?capacity:0;
}
void ch_vk_gpu_mailbox_set_queue(ChVkGpuMailbox *b,unsigned depth) {
    if(!b) return;
    pthread_mutex_lock(&b->lock);b->depth=depth>=2?2:1;pthread_mutex_unlock(&b->lock);
}
VkResult ch_vk_gpu_mailbox_create(ChVkGpuMailbox **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;
    ChVkGpuMailbox *b=calloc(1,sizeof *b);
    if(!b) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if(pthread_mutex_init(&b->lock,NULL)) {free(b);return VK_ERROR_INITIALIZATION_FAILED;}
    b->depth=1;*out=b;return VK_SUCCESS;
}
VkResult ch_vk_gpu_mailbox_publish(void *context,const Nv2aSurface *surface,ChVkTarget *target) {
    ChVkGpuMailbox *b=context;
    if(!b || !surface || !target) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t w,h;ch_vk_target_size(target,&w,&h);
    if(!w || !h || surface->width!=w || surface->height!=h) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&b->lock);
    unsigned i=0;for(;i<CH_VK_GPU_MAILBOX_SLOTS && b->slots[i].held;i++);
    if(i==CH_VK_GPU_MAILBOX_SLOTS || b->sequence==UINT64_MAX) {pthread_mutex_unlock(&b->lock);return i==CH_VK_GPU_MAILBOX_SLOTS?VK_NOT_READY:VK_ERROR_TOO_MANY_OBJECTS;}
    b->slots[i].held=1;
    pthread_mutex_unlock(&b->lock);
    VkResult r;
    if(b->buffers[i]) r=ch_vk_target_copy_to_buffer(target,b->buffers[i],b->capacity);
    else {
        if(b->slots[i].w!=w || b->slots[i].h!=h) {
            ch_vk_texture_destroy(b->slots[i].image);b->slots[i].image=NULL;
        }
        r=b->slots[i].image?ch_vk_target_refresh_texture(target,b->slots[i].image):
            ch_vk_target_snapshot_texture(target,1,&b->slots[i].image);
    }
    pthread_mutex_lock(&b->lock);
    if(r==VK_SUCCESS) {
        b->slots[i].w=w;b->slots[i].h=h;
        if(b->waiting==b->depth) {   /* full: drop the oldest waiting frame */
            b->slots[b->pending[0].slot].held=0;
            for(unsigned k=1;k<b->waiting;k++) b->pending[k-1]=b->pending[k];
            b->waiting--;
        }
        b->pending[b->waiting++]=(ChVkGpuFrame){.surface=*surface,.sequence=++b->sequence,
            .image=b->slots[i].image,.owner=b,.slot=i};
    } else b->slots[i].held=0;
    pthread_mutex_unlock(&b->lock);
    return r;
}
VkResult ch_vk_gpu_mailbox_take(ChVkGpuMailbox *b,ChVkGpuFrame *out) {
    if(!b || !out || out->owner || out->image) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&b->lock);
    VkResult r=VK_NOT_READY;
    if(b->waiting) {
        *out=b->pending[0];
        for(unsigned k=1;k<b->waiting;k++) b->pending[k-1]=b->pending[k];
        memset(&b->pending[--b->waiting],0,sizeof b->pending[0]);r=VK_SUCCESS;
    }
    pthread_mutex_unlock(&b->lock);return r;
}
void ch_vk_gpu_frame_release(ChVkGpuFrame *f) {
    if(!f || !f->owner) return;
    ChVkGpuMailbox *b=f->owner;
    pthread_mutex_lock(&b->lock);b->slots[f->slot].held=0;pthread_mutex_unlock(&b->lock);
    memset(f,0,sizeof *f);
}
VkResult ch_vk_gpu_mailbox_destroy(ChVkGpuMailbox *b) {
    if(!b) return VK_SUCCESS;
    for(unsigned i=0;i<CH_VK_GPU_MAILBOX_SLOTS;i++) {
        int waiting=0;
        for(unsigned k=0;k<b->waiting;k++) waiting|=b->pending[k].slot==i;
        if(b->slots[i].held && !waiting) return VK_NOT_READY;
    }
    for(unsigned i=0;i<CH_VK_GPU_MAILBOX_SLOTS;i++) ch_vk_texture_destroy(b->slots[i].image);
    pthread_mutex_destroy(&b->lock);free(b);return VK_SUCCESS;
}
