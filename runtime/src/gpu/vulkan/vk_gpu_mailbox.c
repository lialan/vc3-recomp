#include "vk_gpu_mailbox.h"
#include "vk_mailbox_policy.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
struct ChVkGpuMailbox {
    pthread_mutex_t lock;
    /* Zero copy: share+1, the share its frame's eyes are counted in until
     * the headset has done reading them (0: none, or done); read_fence (of
     * read_device): the headset's read of them, signalled once done
     * (eyes_reading). */
    struct { ChVkTexture *image; uint32_t w,h; int held;
        uint64_t sequence; unsigned share; VkDevice read_device; VkFence read_fence; } slots[CH_VK_GPU_MAILBOX_SLOTS];
    ChVkGpuFrame pending[2]; unsigned waiting,depth;   /* oldest first */
    uint64_t sequence;
    VkBuffer buffers[CH_VK_GPU_MAILBOX_SLOTS]; VkDeviceSize capacity;   /* bridge mode: a buffer per slot */
    VkImage images[CH_VK_GPU_MAILBOX_SLOTS]; uint32_t image_w,image_h;  /* and an image for the eyes (set_images) */
    /* Bridge mode: each slot's copy, submitted without a wait (the copy out
     * of a frame waited for the GPU to finish it: ~2.4 ms of the game's
     * frame on the Steam Frame). Game device and queue. */
    VkDevice device; VkCommandPool pool;
    uint64_t (*due)(void),(*now)(void);
    struct {VkCommandBuffer command;VkFence fence;int pending,timed;} copy[CH_VK_GPU_MAILBOX_SLOTS];
    uint64_t last_take_ns;   /* take_ready's last frame taken (CLOCK_MONOTONIC) */
    /* CHIHIRO_VULKAN_MAILBOX_TIMING=1 (diagnostic): each copy's GPU time,
     * timestamps before and after it, averaged in the log every 60 frames */
    VkQueryPool timing; float period; double timed_ms; unsigned timed_n;
    /* Zero copy (exportable eye targets, ch_vk_target_record_release): the
     * eyes go to the headset as they are, not copied (a 4128x2208 copy:
     * ~0.9 ms of the Quest's GPU a frame). Each such target by its id, its
     * memory exported for the headset's import, and refs: the frames
     * holding its eyes that the headset has not done with. Its claim waits
     * for none (claim()). stalled: a claim gave up waiting -- the headset
     * takes no frames -- so the next do not wait until it takes one;
     * copy_only: the headset cannot import them (copies from then on). */
    struct {uint64_t id,used;unsigned refs;ChVkTargetExport memory;} shares[CH_VK_GPU_MAILBOX_SHARES];
    uint64_t share_clock;
    int stalled,copy_only;unsigned timeouts;
};
static uint64_t mono_ns(void);
/* Slot i's frame done with its eyes (b->lock held). */
static void unshare(ChVkGpuMailbox *b,unsigned i) {
    if(!b->slots[i].share) return;
    b->shares[b->slots[i].share-1].refs--;
    b->slots[i].share=0;b->slots[i].read_device=VK_NULL_HANDLE;b->slots[i].read_fence=VK_NULL_HANDLE;
}
static void slot_free(ChVkGpuMailbox *b,unsigned i) {unshare(b,i);b->slots[i].held=0;}
/* The share for eyes (b->lock held): its own, else a free one -- or the
 * least recently used that no frame holds -- given its memory; -1: none
 * (the eyes are copied). */
static int share_of(ChVkGpuMailbox *b,const ChVkTarget *eyes) {
    if(b->copy_only || !ch_vk_target_exportable(eyes)) return -1;
    const uint64_t id=ch_vk_target_id(eyes);
    int s=-1;
    for(int k=0;k<CH_VK_GPU_MAILBOX_SHARES;k++) {
        if(b->shares[k].id==id) {b->shares[k].used=++b->share_clock;return k;}
        if(!b->shares[k].refs && (s<0 || b->shares[k].used<b->shares[s].used)) s=k;
    }
    ChVkTargetExport memory;
    if(s<0 || ch_vk_target_export(eyes,&memory)!=VK_SUCCESS) return -1;
    if(b->shares[s].memory.fd>=0) close(b->shares[s].memory.fd);
    b->shares[s].id=id;b->shares[s].used=++b->share_clock;b->shares[s].memory=memory;
    static int logged;
    if(!logged++) fprintf(stderr,"[mailbox] eyes %ux%u handed to the headset as they are (zero copy)\n",memory.width,memory.height);
    return s;
}
/* Waiting frame k dropped unshown (b->lock held). */
static void drop_pending(ChVkGpuMailbox *b,unsigned k) {
    slot_free(b,b->pending[k].slot);
    for(unsigned j=k+1;j<b->waiting;j++) b->pending[j-1]=b->pending[j];
    memset(&b->pending[--b->waiting],0,sizeof b->pending[0]);
}
/* No frame to hold share s's eyes any more (b->lock held): waiting frames
 * dropped unshown; taken ones not being read -- or with force, any -- give
 * them up (the headset skips them: eyes_begin/eyes_reading 0). */
static void give_up(ChVkGpuMailbox *b,unsigned s,int force) {
    for(unsigned k=0;k<b->waiting;) {
        if(b->slots[b->pending[k].slot].share!=s+1) {k++;continue;}
        drop_pending(b,k);
    }
    for(unsigned i=0;i<CH_VK_GPU_MAILBOX_SLOTS;i++)
        if(b->slots[i].share==s+1 && (force || !b->slots[i].read_fence)) unshare(b,i);
}
/* An eye target handed over wants its image back (ch_vk_target_record_release,
 * the game's thread): returns once no frame holds its eyes -- each released
 * or dropped, or the headset done reading them: eyes_done, or the fence of
 * its read seen here (no wait for the headset's next loop to say so). The
 * oldest of a full queue goes as make_room would drop it at the publish
 * that follows (a newer frame finished): waiting for the headset to take
 * it instead kept the queue full -- the game's thread then waited ~12 ms a
 * frame for each take, two frames queued, after a single late take. The
 * headset not taking frames (its session paused, the headset off) or
 * stalled before it reads them: after 100 ms the frames it is not reading
 * give the eyes up, and until it takes one again the next claims do not
 * wait; a read submitted -- a copy, ~1 ms of GPU -- is waited for, up to a
 * second. */
#define CLAIM_WAIT_NS 100000000ull
#define CLAIM_READ_NS 1000000000ull
static void claim(void *context,const ChVkTarget *t) {
    ChVkGpuMailbox *b=context;
    const uint64_t id=ch_vk_target_id(t),start=mono_ns();
    pthread_mutex_lock(&b->lock);
    unsigned s=0;while(s<CH_VK_GPU_MAILBOX_SHARES && b->shares[s].id!=id) s++;
    int timed_out=0;
    while(s<CH_VK_GPU_MAILBOX_SHARES && b->shares[s].refs) {
        for(unsigned i=0;i<CH_VK_GPU_MAILBOX_SLOTS;i++)
            if(b->slots[i].share==s+1 && b->slots[i].read_fence &&
               vkGetFenceStatus(b->slots[i].read_device,b->slots[i].read_fence)==VK_SUCCESS) unshare(b,i);
        if(!b->shares[s].refs) break;
        if(b->waiting>=b->depth && b->slots[b->pending[0].slot].share==s+1) {
            int ready[2]={1,1};
            for(unsigned k=0;k<b->waiting && k<2;k++)
                ready[k]=!b->pending[k].ready || vkGetFenceStatus(b->pending[k].ready_device,b->pending[k].ready)==VK_SUCCESS;
            if(ch_vk_mailbox_full_action(b->waiting,b->depth,ready)==CH_VK_MAILBOX_DROP_OLDEST) {drop_pending(b,0);continue;}
        }
        const uint64_t waited=mono_ns()-start;
        if(b->stalled || waited>=CLAIM_WAIT_NS) {
            if(!b->stalled && !timed_out && (++b->timeouts<=3 || b->timeouts%100==0))
                fprintf(stderr,"[mailbox] the headset held an eye target %.0f ms: taken back (%u times)\n",waited/1e6,b->timeouts);
            timed_out=b->stalled=1;
            give_up(b,s,waited>=CLAIM_READ_NS);
            if(!b->shares[s].refs) break;
        }
        pthread_mutex_unlock(&b->lock);
        nanosleep(&(struct timespec){0,250000},NULL);
        pthread_mutex_lock(&b->lock);
    }
    pthread_mutex_unlock(&b->lock);
}
static int timing_on(void) {
    static int on=-1;
    if(on<0) {const char *e=getenv("CHIHIRO_VULKAN_MAILBOX_TIMING");on=e && atoi(e)!=0;}
    return on;
}
/* slot i's copy is done (its fence signalled): its time, into the average */
static void timing_take(ChVkGpuMailbox *b,unsigned i) {
    if(!b->timing || !b->copy[i].timed) return;
    uint64_t t[2];
    b->copy[i].timed=0;
    if(vkGetQueryPoolResults(b->device,b->timing,2*i,2,sizeof t,t,sizeof t[0],VK_QUERY_RESULT_64_BIT)!=VK_SUCCESS) return;
    b->timed_ms+=(double)(t[1]-t[0])*b->period/1e6;
    if(++b->timed_n==60) {
        fprintf(stderr,"[mailbox] frame copies: %.3f ms of GPU each (60 frames)\n",b->timed_ms/b->timed_n);
        b->timed_ms=0;b->timed_n=0;
    }
}
void ch_vk_gpu_mailbox_set_pacing(ChVkGpuMailbox *b,uint64_t (*due)(void),uint64_t (*now)(void)) {
    if(!b) return;
    pthread_mutex_lock(&b->lock);b->due=due;b->now=now;pthread_mutex_unlock(&b->lock);
}
VkResult ch_vk_gpu_frame_wait(const ChVkGpuFrame *f) {
    if(!f || !f->ready) return VK_SUCCESS;
    return vkWaitForFences(f->ready_device,1,&f->ready,VK_TRUE,5000000000ULL);
}
/* Records copies of n targets (the first required) into slot i's command
 * buffer, each at its offset -- the first into first_image instead when
 * given, or handed over as it is with release (zero copy) -- and submits
 * it; the caller holds the slot. */
static VkResult async_copy(ChVkGpuMailbox *b,unsigned i,unsigned n,ChVkTarget *const *targets,const VkDeviceSize *offsets,
    VkImage first_image,int release) {
    ChVkTarget *first=targets[0];
    VkDevice device=ch_vk_target_device(first);VkResult r;
    if(!b->pool) {
        VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=ch_vk_target_family(first)};
        r=vkCreateCommandPool(device,&pi,NULL,&b->pool);if(r!=VK_SUCCESS) return r;
        b->device=device;
        for(unsigned k=0;k<CH_VK_GPU_MAILBOX_SLOTS;k++) {
            VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=b->pool,
                .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
            VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            r=vkAllocateCommandBuffers(device,&ai,&b->copy[k].command);if(r!=VK_SUCCESS) return r;
            r=vkCreateFence(device,&fi,NULL,&b->copy[k].fence);if(r!=VK_SUCCESS) return r;
        }
        if(timing_on()) {
            VkPhysicalDeviceProperties p;vkGetPhysicalDeviceProperties(ch_vk_target_physical(first),&p);
            VkQueryPoolCreateInfo qi={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,
                .queryCount=2*CH_VK_GPU_MAILBOX_SLOTS};
            if(vkCreateQueryPool(device,&qi,NULL,&b->timing)!=VK_SUCCESS) b->timing=VK_NULL_HANDLE;
            b->period=p.limits.timestampPeriod;
        }
    }
    if(device!=b->device) return VK_ERROR_INITIALIZATION_FAILED;
    if(b->copy[i].pending) {   /* a frame dropped unread: its copy finishes first */
        r=vkWaitForFences(device,1,&b->copy[i].fence,VK_TRUE,5000000000ULL);if(r!=VK_SUCCESS) return r;
    }
    if(b->copy[i].timed) timing_take(b,i);
    r=vkResetFences(device,1,&b->copy[i].fence);if(r!=VK_SUCCESS) return r;
    VkCommandBuffer c=b->copy[i].command;
    r=vkResetCommandBuffer(c,0);if(r!=VK_SUCCESS) return r;
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    r=vkBeginCommandBuffer(c,&begin);if(r!=VK_SUCCESS) return r;
    r=VK_SUCCESS;
    if(b->timing) {
        vkCmdResetQueryPool(c,b->timing,2*i,2);
        vkCmdWriteTimestamp(c,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,b->timing,2*i);
    }
    for(unsigned k=0;k<n && r==VK_SUCCESS;k++)
        if(targets[k]) r=k==0 && release?ch_vk_target_record_release(targets[k],c,claim,b):
            k==0 && first_image?ch_vk_target_record_copy_to_image(targets[k],c,first_image):
            ch_vk_target_record_copy_to_buffer(targets[k],c,b->buffers[i],offsets[k],b->capacity);
    if(b->timing) vkCmdWriteTimestamp(c,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,b->timing,2*i+1);
    VkResult e=vkEndCommandBuffer(c);if(r==VK_SUCCESS) r=e;
    if(r!=VK_SUCCESS) return r;
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&c};
    r=vkQueueSubmit(ch_vk_target_queue(first),1,&submit,b->copy[i].fence);
    if(r==VK_SUCCESS) {b->copy[i].pending=1;b->copy[i].timed=b->timing!=VK_NULL_HANDLE;}
    return r;
}
void ch_vk_gpu_mailbox_set_buffers(ChVkGpuMailbox *b,const VkBuffer buffers[CH_VK_GPU_MAILBOX_SLOTS],VkDeviceSize capacity) {
    if(!b) return;
    for(unsigned i=0;i<CH_VK_GPU_MAILBOX_SLOTS;i++) b->buffers[i]=buffers?buffers[i]:VK_NULL_HANDLE;
    b->capacity=buffers?capacity:0;
}
void ch_vk_gpu_mailbox_set_images(ChVkGpuMailbox *b,const VkImage images[CH_VK_GPU_MAILBOX_SLOTS],uint32_t width,uint32_t height) {
    if(!b) return;
    for(unsigned i=0;i<CH_VK_GPU_MAILBOX_SLOTS;i++) b->images[i]=images && width && height?images[i]:VK_NULL_HANDLE;
    b->image_w=images?width:0;b->image_h=images?height:0;
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
    for(unsigned k=0;k<CH_VK_GPU_MAILBOX_SHARES;k++) b->shares[k].memory.fd=-1;
    b->depth=1;*out=b;return VK_SUCCESS;
}
/* Room for one more waiting frame (b->lock held; it may be dropped and
 * taken again while waiting): a full queue waits while the GPU has not
 * finished its next frame, then drops its oldest (vk_mailbox_policy.h). The
 * wait also keeps the game from running more than the queue ahead of the
 * GPU. A second's wait (a stuck GPU): the oldest goes, as before. */
static void make_room(ChVkGpuMailbox *b) {
    for(;;) {
        int ready[2]={1,1};
        for(unsigned k=0;k<b->waiting && k<2;k++)
            ready[k]=!b->pending[k].ready || vkGetFenceStatus(b->pending[k].ready_device,b->pending[k].ready)==VK_SUCCESS;
        int a=ch_vk_mailbox_full_action(b->waiting,b->depth,ready);
        if(a==CH_VK_MAILBOX_ROOM) return;
        if(a!=CH_VK_MAILBOX_DROP_OLDEST) {
            VkDevice d=b->pending[a].ready_device;VkFence f=b->pending[a].ready;
            pthread_mutex_unlock(&b->lock);
            VkResult w=vkWaitForFences(d,1,&f,VK_TRUE,1000000000ULL);
            pthread_mutex_lock(&b->lock);
            if(w==VK_SUCCESS) continue;
            if(b->waiting<b->depth) return;
        }
        slot_free(b,b->pending[0].slot);
        for(unsigned k=1;k<b->waiting;k++) b->pending[k-1]=b->pending[k];
        b->waiting--;
        return;
    }
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
    if(b->buffers[i]) r=async_copy(b,i,1,&target,(const VkDeviceSize[]){0},VK_NULL_HANDLE,0);
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
        make_room(b);
        b->pending[b->waiting++]=(ChVkGpuFrame){.surface=*surface,.sequence=++b->sequence,
            .image=b->slots[i].image,.owner=b,.slot=i,
            .ready_device=b->buffers[i]?b->device:VK_NULL_HANDLE,.ready=b->buffers[i]?b->copy[i].fence:VK_NULL_HANDLE,
            .due_ns=b->due?b->due():0};
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
        memset(&b->pending[--b->waiting],0,sizeof b->pending[0]);r=VK_SUCCESS;b->stalled=0;
    }
    pthread_mutex_unlock(&b->lock);return r;
}
static uint64_t s_gpu_late;   /* frames taken late for the GPU, all mailboxes */
uint64_t ch_vk_gpu_mailbox_gpu_late_count(void) {return __atomic_load_n(&s_gpu_late,__ATOMIC_RELAXED);}
static uint64_t mono_ns(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;}
/* A frame due by now that is not done marks itself late: a game frame's time
 * (60 Hz, less 1 ms) since the last frame taken, or its due time (pacing). */
#define LATE_AFTER_NS 15666667ull
VkResult ch_vk_gpu_mailbox_take_ready(ChVkGpuMailbox *b,ChVkGpuFrame *out) {
    if(!b || !out || out->owner || out->image) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&b->lock);
    VkResult r=VK_NOT_READY;
    if(b->waiting && (!b->pending[0].due_ns || !b->now || b->now()>=b->pending[0].due_ns)) {
        if(!b->pending[0].ready || vkGetFenceStatus(b->pending[0].ready_device,b->pending[0].ready)==VK_SUCCESS) {
            if(b->pending[0].ready) timing_take(b,b->pending[0].slot);
            *out=b->pending[0];
            for(unsigned k=1;k<b->waiting;k++) b->pending[k-1]=b->pending[k];
            memset(&b->pending[--b->waiting],0,sizeof b->pending[0]);r=VK_SUCCESS;
            if(out->gpu_late) __atomic_add_fetch(&s_gpu_late,1,__ATOMIC_RELAXED);
            b->last_take_ns=mono_ns();b->stalled=0;   /* the headset takes frames again */
        } else if(b->pending[0].due_ns || (b->last_take_ns && mono_ns()-b->last_take_ns>=LATE_AFTER_NS))
            b->pending[0].gpu_late=1;   /* due, the GPU not done: the headset shows the last frame again */
    }
    pthread_mutex_unlock(&b->lock);return r;
}
void ch_vk_gpu_frame_release(ChVkGpuFrame *f) {
    if(!f || !f->owner) return;
    ChVkGpuMailbox *b=f->owner;
    pthread_mutex_lock(&b->lock);slot_free(b,f->slot);pthread_mutex_unlock(&b->lock);
    memset(f,0,sizeof *f);
}
/* The taken frame's slot, while it still holds its eyes (b->lock held) */
static int holds_eyes(const ChVkGpuMailbox *b,const ChVkGpuFrame *f) {
    return f->slot<CH_VK_GPU_MAILBOX_SLOTS && b->slots[f->slot].held && b->slots[f->slot].sequence==f->sequence &&
        b->slots[f->slot].share;
}
int ch_vk_gpu_frame_eyes_begin(const ChVkGpuFrame *f,unsigned *share,uint64_t *id) {
    if(!f || !f->owner || !f->eyes_shared || !share || !id) return 0;
    ChVkGpuMailbox *b=f->owner;
    pthread_mutex_lock(&b->lock);
    const int ok=holds_eyes(b,f);
    if(ok) {*share=b->slots[f->slot].share-1;*id=b->shares[*share].id;}
    pthread_mutex_unlock(&b->lock);
    return ok;
}
int ch_vk_gpu_frame_eyes_memory(const ChVkGpuFrame *f,ChVkTargetExport *out) {
    if(!f || !f->owner || !out) return 0;
    ChVkGpuMailbox *b=f->owner;
    pthread_mutex_lock(&b->lock);
    int ok=holds_eyes(b,f);
    if(ok) {
        *out=b->shares[b->slots[f->slot].share-1].memory;
        out->fd=out->fd>=0?dup(out->fd):-1;ok=out->fd>=0;
    }
    pthread_mutex_unlock(&b->lock);
    return ok;
}
int ch_vk_gpu_frame_eyes_reading(const ChVkGpuFrame *f,VkDevice device,VkFence fence) {
    if(!f || !f->owner || !fence) return 0;
    ChVkGpuMailbox *b=f->owner;
    pthread_mutex_lock(&b->lock);
    const int ok=holds_eyes(b,f);
    if(ok) {b->slots[f->slot].read_device=device;b->slots[f->slot].read_fence=fence;}
    pthread_mutex_unlock(&b->lock);
    return ok;
}
void ch_vk_gpu_frame_eyes_done(const ChVkGpuFrame *f) {
    if(!f || !f->owner) return;
    ChVkGpuMailbox *b=f->owner;
    pthread_mutex_lock(&b->lock);
    if(holds_eyes(b,f)) unshare(b,f->slot);
    pthread_mutex_unlock(&b->lock);
}
void ch_vk_gpu_mailbox_copy_eyes(ChVkGpuMailbox *b) {
    if(!b) return;
    pthread_mutex_lock(&b->lock);b->copy_only=1;pthread_mutex_unlock(&b->lock);
}
VkResult ch_vk_gpu_mailbox_destroy(ChVkGpuMailbox *b) {
    if(!b) return VK_SUCCESS;
    for(unsigned i=0;i<CH_VK_GPU_MAILBOX_SLOTS;i++) {
        int waiting=0;
        for(unsigned k=0;k<b->waiting;k++) waiting|=b->pending[k].slot==i;
        if(b->slots[i].held && !waiting) return VK_NOT_READY;
    }
    for(unsigned i=0;i<CH_VK_GPU_MAILBOX_SLOTS;i++) ch_vk_texture_destroy(b->slots[i].image);
    if(b->pool) {
        for(unsigned i=0;i<CH_VK_GPU_MAILBOX_SLOTS;i++) {
            if(b->copy[i].pending) vkWaitForFences(b->device,1,&b->copy[i].fence,VK_TRUE,5000000000ULL);
            vkDestroyFence(b->device,b->copy[i].fence,NULL);
        }
        vkDestroyCommandPool(b->device,b->pool,NULL);
        if(b->timing) vkDestroyQueryPool(b->device,b->timing,NULL);
    }
    for(unsigned k=0;k<CH_VK_GPU_MAILBOX_SHARES;k++) if(b->shares[k].memory.fd>=0) close(b->shares[k].memory.fd);
    pthread_mutex_destroy(&b->lock);free(b);return VK_SUCCESS;
}
VkResult ch_vk_gpu_mailbox_publish_immersive(ChVkGpuMailbox *b,const Nv2aSurface *surface,ChVkTarget *eyes,
    ChVkTarget *hud,ChVkTarget *lens,const ChVkImmFrame *info) {
    if(!b || !surface || !eyes || !info) return VK_ERROR_INITIALIZATION_FAILED;
    if(!b->buffers[0]) return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t w,h;ch_vk_target_size(eyes,&w,&h);
    if(w!=info->eye_width*2 || h!=info->eye_height) return VK_ERROR_INITIALIZATION_FAILED;
    VkDeviceSize hud_offset=((VkDeviceSize)w*h*4+255)&~(VkDeviceSize)255,lens_offset=hud_offset;
    if(hud) {uint32_t hw,hh;ch_vk_target_size(hud,&hw,&hh);lens_offset=(hud_offset+(VkDeviceSize)hw*hh*4+255)&~(VkDeviceSize)255;}
    uint32_t lw=0,lh=0;
    if(lens && info->lens) ch_vk_target_size(lens,&lw,&lh);
    if(!lw || !lh || lens_offset+(VkDeviceSize)lw*lh*4>b->capacity) lens=NULL;   /* none, or no room */
    pthread_mutex_lock(&b->lock);
    unsigned i=0;for(;i<CH_VK_GPU_MAILBOX_SLOTS && b->slots[i].held;i++);
    if(i==CH_VK_GPU_MAILBOX_SLOTS || b->sequence==UINT64_MAX) {pthread_mutex_unlock(&b->lock);return i==CH_VK_GPU_MAILBOX_SLOTS?VK_NOT_READY:VK_ERROR_TOO_MANY_OBJECTS;}
    b->slots[i].held=1;
    const int share=share_of(b,eyes);   /* zero copy: the eye target's own image */
    pthread_mutex_unlock(&b->lock);
    ChVkTarget *const targets[3]={eyes,hud,lens};
    /* else the eyes into the slot's shared image when there is one of their size */
    const VkImage eyes_image=share<0 && b->images[i] && w==b->image_w && h==b->image_h?b->images[i]:VK_NULL_HANDLE;
    VkResult r=async_copy(b,i,3,targets,(const VkDeviceSize[]){0,hud_offset,lens_offset},eyes_image,share>=0);
    pthread_mutex_lock(&b->lock);
    if(r==VK_SUCCESS) {
        make_room(b);
        ChVkGpuFrame frame={.surface=*surface,.sequence=++b->sequence,.owner=b,.slot=i,
            .immersive=1,.imm=*info,.hud_offset=hud_offset,.ready_device=b->device,.ready=b->copy[i].fence,
            .due_ns=b->due?b->due():0};
        frame.imm.hud=hud!=NULL;frame.eyes_image=eyes_image!=VK_NULL_HANDLE;frame.eyes_shared=share>=0;
        frame.imm.lens=lens!=NULL;frame.imm.lens_w=lens?lw:0;frame.imm.lens_h=lens?lh:0;frame.imm.lens_offset=lens?lens_offset:0;
        b->slots[i].sequence=frame.sequence;
        if(share>=0) {b->slots[i].share=(unsigned)share+1;b->shares[share].refs++;}
        b->pending[b->waiting++]=frame;
    } else b->slots[i].held=0;
    pthread_mutex_unlock(&b->lock);
    return r;
}
