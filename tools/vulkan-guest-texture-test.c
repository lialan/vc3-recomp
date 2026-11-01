/* Links the actual executor decoder, with only its guest-memory-base accessor
 * supplied by this isolated fixture. No game process or memory is accessed. */
#include "vk_nv2a.h"
#include "vk_backend.h"
#include "vk_menu.h"
#include "vk_overlay.h"
#include "vk_frame_mailbox.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
struct QueueLockProbe { unsigned locks,unlocks; int held; };
static void probe_lock(void *context) {
    struct QueueLockProbe *probe=context;
    if(probe->held) abort();
    probe->held=1;probe->locks++;
}
static void probe_unlock(void *context) {
    struct QueueLockProbe *probe=context;
    if(!probe->held) abort();
    probe->held=0;probe->unlocks++;
}
static int ring_mode;
static volatile uint32_t dma_get;
/* Link-only dependencies for disabled deferred/present paths. Abort rather
 * than inventing successful guest allocator or worker behavior. */
uint32_t g_xbox_stack_base,g_xbox_stack_size,g_xbox_image_lo,g_xbox_image_hi;
uint32_t xbox_ContiguousAllocatedBytes(void) { return ring_mode?4096:0; } /* fixture has no contiguous arena */
void xbox_ThreadAcctName(const char *name) { (void)name; abort(); }
uint64_t xbox_HostNowNs(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;
}
static union { uint64_t align; unsigned char bytes[4096]; } guest;
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)guest.bytes-(ring_mode?(ptrdiff_t)0x80000000u:0); }
/* These platform hooks are unreachable in this no-flip method fixture. */
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t x) { if(ring_mode && x==0x800044u) return &dma_get; abort(); }
uint32_t xbox_Nv2aFenceWordVa(void) { abort(); }
void xbox_Nv2aFrameCounterFlip(void) { abort(); }
void xbox_FramebufferWindowFrameStats(uint32_t x) { (void)x; abort(); }
void xbox_FramebufferWindowSet(uint32_t a,uint32_t b) { (void)a; (void)b; abort(); }
void xbox_FramebufferWindowSetAA(uint32_t x) { (void)x; abort(); }
void xbox_FramebufferWindowPresent(uint32_t a,uint32_t b) { (void)a; (void)b; abort(); }
void xbox_FramebufferWindowStart(void) { abort(); }
uint32_t xbox_VblankCount(void) { if(ring_mode) return 0; abort(); }
uint64_t xbox_VblankLastNs(void) { if(ring_mode) return 0; abort(); }
uint32_t xbox_Nv2aFrameCountersOwned(void) { abort(); }
void xbox_Nv2aAckBusyBits(void) { if(!ring_mode) abort(); }
void nv2a_pb_resync(uint32_t);
void nv2a_pb_scan(uint32_t);
void nv2a_pb_exec_method(uint32_t,uint32_t,uint32_t);
static void method_float(uint32_t method,float f) { uint32_t bits; memcpy(&bits,&f,4); nv2a_pb_exec_method(0,method,bits); }
struct StereoHost {
    ChVkTarget *eyes[2]; ChVkEye view[2]; unsigned clears,draws,flips,raws;
    unsigned char rgba[2][37*19*4];
};
static VkResult host_raw(void *context,const Nv2aSurface *s,const Nv2aBatch *batch) {
    (void)s; struct StereoHost *host=context;
    VrGameProj projection;
    VkResult r=ch_vk_nv2a_projection_from_raw(batch,&projection);
    if(r!=VK_SUCCESS) return r;
    for(unsigned eye=0;eye<2;eye++) host->view[eye].game_projection=projection;
    host->raws++; return VK_SUCCESS;
}
static VkResult host_clear(void *context,const Nv2aSurface *surface,const Nv2aRenderState *state,
    uint32_t flags,uint32_t argb,uint32_t zs) {
    struct StereoHost *host=context; (void)surface;(void)state;
    if(flags!=0xf3) return VK_ERROR_FEATURE_NOT_PRESENT;
    float color[]={((argb>>16)&255)/255.f,((argb>>8)&255)/255.f,(argb&255)/255.f,(argb>>24)/255.f};
    for(unsigned eye=0;eye<2;eye++) {
        VkResult r=ch_vk_target_clear_aspects_read(host->eyes[eye],7,color,
            (float)((double)(zs>>8)/16777215.0),zs&255,host->rgba[eye],sizeof(host->rgba[eye]));
        if(r!=VK_SUCCESS) return r;
    }
    host->clears++; return VK_SUCCESS;
}
static VkResult host_draw(void *context,const Nv2aSurface *s,const Nv2aBatch *batch) {
    struct StereoHost *host=context;
    /* Guest visibility counted once on flat execution, never per eye. */
    Nv2aBatch eye_batch=*batch; eye_batch.zpass_out=NULL;
    for(unsigned eye=0;eye<2;eye++) {
        VkResult r=ch_vk_nv2a_draw_eye_read(host->eyes[eye],s,&eye_batch,nv2a_backend_decode_texture,
            &host->view[eye],host->rgba[eye],sizeof(host->rgba[eye]));
        if(r!=VK_SUCCESS) return r;
    }
    host->draws++; return VK_SUCCESS;
}
static ChVkFrameMailbox *test_mailbox;
static VkResult host_present(void *context,const Nv2aSurface *surface,const void *rgba,size_t bytes) {
    (void)context;
    return ch_vk_frame_mailbox_publish(test_mailbox,surface,rgba,bytes);
}
static VkResult host_flip(void *context,uint32_t presented) {
    struct StereoHost *host=context; (void)presented; host->flips++; return VK_SUCCESS;
}
struct ColorEvents { unsigned initial,edits; uint32_t va; unsigned char pixel[4]; };
static VkResult color_changed(void *context,const Nv2aSurface *surface,const void *rgba,size_t bytes,int initial) {
    struct ColorEvents *events=context;
    if(bytes!=(size_t)surface->width*surface->height*4) return VK_ERROR_INITIALIZATION_FAILED;
    if(initial) events->initial++; else events->edits++;
    events->va=surface->color_va; memcpy(events->pixel,rgba,4); return VK_SUCCESS;
}
static int check_simple_combiner(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    ChVkTarget *target=NULL;
    unsigned char reference[8*8*4],actual[sizeof reference];
    if(ch_vk_target_create(gpu,device,queue,family,8,8,&target)!=VK_SUCCESS) return 1;
    Nv2aSurface surface={.width=8,.height=8,.pitch=32,.bytes_per_pixel=4};
    Nv2aTexture tx={.offset=256,.width=4,.height=4,.pitch=16,.levels=1,.filter=0x02020000,.color=0x12,.addr_u=3,.addr_v=3};
    for(unsigned i=0;i<16;i++) {uint32_t color=((i*11+50)<<24)|((i*13)<<16)|((i*7)<<8)|180;memcpy(guest.bytes+256+i*4,&color,4);}
    Nv2aRenderState state={.color_mask=0x01010101,.alpha_test_enable=1,.alpha_func=0x204,.alpha_ref=50};
    Nv2aVertex v[6]={{0,0,0,1,0xa0f08020,3,2},{8,0,0,1,0xa0f08020,3,2},
        {8,8,0,1,0xa0f08020,3,2},{0,0,0,1,0xa0f08020,3,2},
        {8,8,0,1,0xa0f08020,3,2},{0,8,0,1,0xa0f08020,3,2}};
    Nv2aVertexEx ex[6]={0};
    Nv2aCombiner rc={.control=1,.stage_program=1};
    Nv2aBatch batch={.vertices=v,.count=6,.state=&state,.rc=&rc,.ext=ex,.stage={&tx}};
    int failed=0;
    const char *env=getenv("CHIHIRO_VULKAN_FORCE_COMBINER");char *saved=env?strdup(env):NULL;
    for(unsigned kind=0;kind<72 && !failed;kind++) {
        unsigned phase=kind%12;
        const uint32_t formats[3]={0x12,0x06,0x0f};
        tx.color=formats[(kind%36)/12];
        for(unsigned i=0;i<6;i++) {
            float q=phase>=8?1+(i%3):2;
            ex[i].tex[0][0]=v[i].x*.5f*q/((kind%36)/12?4:1);ex[i].tex[0][1]=v[i].y*.5f*q/((kind%36)/12?4:1);ex[i].tex[0][3]=q;
        }
        rc.stage_program=phase&1?2:1;
        rc.color_icw[0]=phase&2?0x04080000:0x08040000;
        rc.alpha_icw[0]=phase&2?0x14180000:0x18140000;
        rc.color_ocw[0]=rc.alpha_ocw[0]=phase&4?0xc00:0xc0;
        rc.final0=phase&1?0x0c:0;rc.final1=phase&1?0x1c00:0;
        rc.control=kind>=36?4:1;
        if(kind>=36) {
            rc.final0=0x0c;rc.final1=0x1c80;
            if(phase&2) rc.alpha_icw[0]=0x18200000;
            for(unsigned stage=1;stage<4;stage++) {
                rc.color_icw[stage]=0x0c200000;rc.alpha_icw[stage]=0x1c200000;
                rc.color_ocw[stage]=rc.alpha_ocw[stage]=0xc00;
            }
        }
        for(unsigned full=0;full<2;full++) {
            if(full) unsetenv("CHIHIRO_VULKAN_FORCE_COMBINER");else setenv("CHIHIRO_VULKAN_FORCE_COMBINER","1",1);
            const float blank[4]={.1,.2,.3,.4};
            unsigned char *pixels=full?actual:reference;
            if(ch_vk_target_clear_read(target,blank,pixels,sizeof actual)!=VK_SUCCESS ||
               ch_vk_nv2a_draw_decoded_read(target,&surface,&batch,nv2a_backend_decode_texture,pixels,sizeof actual)!=VK_SUCCESS) failed=1;
        }
        for(size_t i=0;!failed && i<sizeof actual;i++) if(abs((int)reference[i]-actual[i])>1) {
            fprintf(stderr,"simple combiner case%u byte%zu generic%u reduced%u\n",kind,i,reference[i],actual[i]);failed=1;
        }
    }
    if(saved) {setenv("CHIHIRO_VULKAN_FORCE_COMBINER",saved,1);free(saved);}else unsetenv("CHIHIRO_VULKAN_FORCE_COMBINER");
    ch_vk_target_destroy(target);
    if(!failed) puts("PASS: proven texture-diffuse combiner reduction matches generic shader: linear/Morton/DXT textures, alpha tests, bilinear/projective UV, swapped inputs/final identity; varying q retains fragment divide");
    return failed;
}
static int bench_guest_draws(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    ChVkTarget *target=NULL;
    const size_t bytes=640*480*4;
    unsigned char *pixels=malloc(bytes);
    if(!pixels || ch_vk_target_create(gpu,device,queue,family,640,480,&target)!=VK_SUCCESS) {free(pixels);return 1;}
    const float black[4]={0,0,0,1};
    int failed=ch_vk_target_clear_read(target,black,pixels,bytes)!=VK_SUCCESS;
    for(unsigned i=0;i<16;i++) {uint32_t color=0xff204080;memcpy(guest.bytes+256+i*4,&color,4);}
    Nv2aTexture tx={.offset=256,.width=4,.height=4,.pitch=16,.color=0x12,.levels=7,
        .filter=0x02022000,.addr_u=3,.addr_v=3};
    Nv2aSurface surface={.width=640,.height=480,.pitch=2560,.bytes_per_pixel=4};
    Nv2aRenderState state={.color_mask=0x01010101};
    Nv2aVertex v[6]={{0,0,0,1,0xffffffff,0,0},{640,0,0,1,0xffffffff,4,0},
        {640,480,0,1,0xffffffff,4,4},{0,0,0,1,0xffffffff,0,0},
        {640,480,0,1,0xffffffff,4,4},{0,480,0,1,0xffffffff,0,4}};
    Nv2aVertexEx ex[6]={0};
    for(unsigned i=0;i<6;i++) {ex[i].tex[0][0]=v[i].u;ex[i].tex[0][1]=v[i].v;ex[i].tex[0][3]=1;}
    Nv2aCombiner rc={.control=4,.stage_program=1,.final0=0x0c,.final1=0x1c80};
    rc.color_icw[0]=0x08040000;rc.alpha_icw[0]=0x18140000;
    rc.color_ocw[0]=rc.alpha_ocw[0]=0xc00;
    for(unsigned i=1;i<4;i++) {
        rc.color_icw[i]=0x0c200000;rc.alpha_icw[i]=0x1c200000;
        rc.color_ocw[i]=rc.alpha_ocw[i]=0xc00;
    }
    Nv2aBatch batch={.vertices=v,.count=6,.state=&state,.rc=&rc,.ext=ex,.stage={&tx}};
    const char *env=getenv("CHIHIRO_VULKAN_FORCE_COMBINER");char *saved=env?strdup(env):NULL;
    for(unsigned variant=0;variant<4 && !failed;variant++) {
        unsigned resident=variant&1,reduced=variant>>1;
        if(reduced) unsetenv("CHIHIRO_VULKAN_FORCE_COMBINER");else setenv("CHIHIRO_VULKAN_FORCE_COMBINER","1",1);
        ch_vk_target_defer_color_readback(target,resident);
        uint64_t start=xbox_HostNowNs();
        for(unsigned n=0;n<100;n++) if(ch_vk_nv2a_draw_decoded_read(target,&surface,&batch,
            nv2a_backend_decode_texture,pixels,bytes)!=VK_SUCCESS) {failed=1;break;}
        double elapsed=(xbox_HostNowNs()-start)/1e6;
        if(ch_vk_target_read_color(target,pixels,bytes)!=VK_SUCCESS) failed=1;
        const unsigned char expected[4]={0x20,0x40,0x80,0xff};
        for(size_t p=0;!failed && p<bytes;p+=4) if(memcmp(pixels+p,expected,4)) failed=1;
        printf("BENCH: 640x480 combiner, 100 separate textured draws, reduced=%u resident=%u total=%.2fms mean=%.3fms checksum=%s\n",
            reduced,resident,elapsed,elapsed/100,failed?"FAIL":"PASS");
    }
    if(saved) {setenv("CHIHIRO_VULKAN_FORCE_COMBINER",saved,1);free(saved);}else unsetenv("CHIHIRO_VULKAN_FORCE_COMBINER");
    ch_vk_target_destroy(target);free(pixels);return failed;
}
int ch_test_guest_textures(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    ChVkTarget *target=NULL;
    if(ch_vk_target_create(gpu,device,queue,family,4,4,&target)!=VK_SUCCESS) return 1;
    const Nv2aSurface surface={.width=4,.height=4,.pitch=16,.bytes_per_pixel=4};
    const Nv2aRenderState state={.color_mask=0x01010101};
    const Nv2aVertex vertices[]={
        {.x=0,.y=0,.rhw=1,.diffuse=0xffffffff,.u=0,.v=0},
        {.x=4,.y=0,.rhw=1,.diffuse=0xffffffff,.u=4,.v=0},
        {.x=4,.y=4,.rhw=1,.diffuse=0xffffffff,.u=4,.v=4},
        {.x=0,.y=0,.rhw=1,.diffuse=0xffffffff,.u=0,.v=0},
        {.x=4,.y=4,.rhw=1,.diffuse=0xffffffff,.u=4,.v=4},
        {.x=0,.y=4,.rhw=1,.diffuse=0xffffffff,.u=0,.v=4}};
    Nv2aTexture texture={.offset=256,.width=4,.height=4,.pitch=24,.levels=1,
        .addr_u=3,.addr_v=3,.filter=0x01010000};
    Nv2aBatch batch={.vertices=vertices,.count=6,.state=&state,.texture=&texture};
    uint32_t want[16],decoded[16]; unsigned char pixels[64];
    int failed=0;
    for(unsigned mode=0;mode<5 && !failed;mode++) {
        memset(guest.bytes,0xcd,sizeof(guest.bytes));
        if(mode<2) {
            texture.color=mode?0x06:0x12;
            for(unsigned y=0;y<4;y++) for(unsigned x=0;x<4;x++) {
                unsigned i=y*4+x;
                want[i]=((32+i*13)<<24)|((x*67)<<16)|((y*73)<<8)|(i*11);
                unsigned offset=mode?((x&1)|((y&1)<<1)|((x&2)<<1)|((y&2)<<2))*4:y*24+x*4;
                memcpy(guest.bytes+256+offset,&want[i],4);
            }
        } else {
            texture.color=mode==2?0x0c:mode==3?0x0e:0x0f;
            /* DXT1: RGB565 red > green; selectors 0,1,2,3 in every row. */
            const unsigned char block[]={0x00,0xf8,0xe0,0x07,0xe4,0xe4,0xe4,0xe4};
            memcpy(guest.bytes+256+(mode>2?8:0),block,sizeof(block));
            if(mode==3) {
                for(unsigned i=0;i<8;i++) guest.bytes[256+i]=(unsigned char)((2*i)|((2*i+1)<<4));
            } else if(mode==4) {
                guest.bytes[256]=255; guest.bytes[257]=0;
                uint64_t selectors=0;
                for(unsigned i=0;i<16;i++) selectors|=(uint64_t)(i%8)<<(i*3);
                for(unsigned i=0;i<6;i++) guest.bytes[258+i]=(unsigned char)(selectors>>(i*8));
            }
            const uint32_t colors[]={0xffff0000,0xff00ff00,0xffaa5500,0xff55aa00};
            for(unsigned i=0;i<16;i++) {
                const unsigned alpha5[]={255,0,218,182,145,109,72,36};
                unsigned alpha=mode==3?i*17:mode==4?alpha5[i%8]:255;
                want[i]=(colors[i%4]&0xffffff)|(alpha<<24);
            }
        }
        if(!nv2a_backend_decode_texture(&texture,decoded)) { failed=1; break; }
        for(unsigned i=0;i<16;i++) if(decoded[i]!=want[i]) {
            fprintf(stderr,"real decoder mode %u pixel %u got %08x expected %08x\n",mode,i,decoded[i],want[i]); failed=1;
        }
        VkResult r=ch_vk_nv2a_clear_read(target,&surface,0xf3,0,0xffffff00,pixels,sizeof(pixels));
        if(r==VK_SUCCESS) r=ch_vk_nv2a_draw_decoded_read(target,&surface,&batch,nv2a_backend_decode_texture,pixels,sizeof(pixels));
        if(r!=VK_SUCCESS) { fprintf(stderr,"real decoded draw mode %u result %d\n",mode,r); failed=1; }
        for(unsigned i=0;i<16;i++) {
            unsigned char rgba[]={want[i]>>16,want[i]>>8,want[i],want[i]>>24};
            for(unsigned c=0;c<4;c++) if(pixels[i*4+c]!=rgba[c]) {
                fprintf(stderr,"real decoded draw mode %u pixel %u channel %u\n",mode,i,c); failed=1;
            }
        }
        /* Vulkan output -> pitched guest bytes -> actual decoder -> Vulkan.
         * Keep source input separate to verify both bpp conversions independently. */
        unsigned char original[64]; memcpy(original,pixels,sizeof(original));
        for(unsigned bpp=2;bpp<=4 && !failed;bpp+=2) {
            Nv2aSurface output=surface;
            output.color_va=1024; output.bytes_per_pixel=bpp; output.pitch=bpp*6;
            memset(guest.bytes+1024,0xab,128);
            r=ch_vk_nv2a_store_color(&output,original,sizeof(original),guest.bytes,sizeof(guest.bytes));
            if(r!=VK_SUCCESS) { failed=1; break; }
            for(unsigned y=0;y<4;y++) for(unsigned b=4*bpp;b<6*bpp;b++)
                if(guest.bytes[1024+y*output.pitch+b]!=0xab) failed=1;
            Nv2aTexture roundtrip=texture;
            roundtrip.offset=1024; roundtrip.pitch=output.pitch;
            roundtrip.color=bpp==4?0x12:0x11;
            Nv2aBatch redraw=batch; redraw.texture=&roundtrip;
            r=ch_vk_nv2a_draw_decoded_read(target,&surface,&redraw,nv2a_backend_decode_texture,pixels,sizeof(pixels));
            if(r!=VK_SUCCESS) { failed=1; break; }
            for(unsigned i=0;i<16;i++) for(unsigned c=0;c<4;c++) {
                unsigned expected=original[i*4+c];
                if(bpp==2) {
                    if(c==3) expected=255;
                    else if(c==1) { unsigned six=expected>>2; expected=six*255/63; }
                    else { unsigned five=expected>>3; expected=five*255/31; }
                }
                if(pixels[i*4+c]!=expected) {
                    fprintf(stderr,"render-to-texture mode%u bpp%u pixel%u c%u got%u expected%u\n",mode,bpp,i,c,pixels[i*4+c],expected);
                    failed=1;
                }
            }
            /* Invalid destination must fail before any writes. */
            unsigned char snapshot[128]; memcpy(snapshot,guest.bytes+1024,128);
            Nv2aSurface invalid=output; invalid.pitch=1;
            if(ch_vk_nv2a_store_color(&invalid,original,sizeof(original),guest.bytes,sizeof(guest.bytes))==VK_SUCCESS ||
               memcmp(snapshot,guest.bytes+1024,128)) failed=1;
            invalid=output; invalid.color_va=4090;
            if(ch_vk_nv2a_store_color(&invalid,original,sizeof(original),guest.bytes,sizeof(guest.bytes))==VK_SUCCESS) failed=1;
        }
    }
    if(!failed) {
        texture.color=0x12;
        for(unsigned y=0;y<4;y++) for(unsigned x=0;x<4;x++) {
            uint32_t color=0xff000020u|(x*60u<<16)|(y*60u<<8);
            memcpy(guest.bytes+256+y*24+x*4,&color,4);
        }
        Nv2aVertex sampled[6];
        memcpy(sampled,vertices,sizeof(sampled)); batch.vertices=sampled;
        for(unsigned mode=1;mode<=4 && !failed;mode++) {
            texture.addr_u=texture.addr_v=mode==4?3:mode;
            texture.filter=mode==4?0x02020000:0x01010000;
            for(unsigned i=0;i<6;i++) {
                sampled[i].u=mode==4?2:vertices[i].u*3-4;
                sampled[i].v=mode==4?2:vertices[i].v*3-4;
            }
            VkResult r=ch_vk_nv2a_draw_decoded_read(target,&surface,&batch,nv2a_backend_decode_texture,pixels,sizeof(pixels));
            if(r!=VK_SUCCESS) { failed=1; break; }
            const unsigned lookup[][4]={{0,0,0,0},{1,0,3,2},{2,0,3,1},{0,0,3,3}};
            for(unsigned y=0;y<4;y++) for(unsigned x=0;x<4;x++) {
                unsigned char expected[]={mode==4?90:lookup[mode][x]*60,mode==4?90:lookup[mode][y]*60,32,255};
                for(unsigned c=0;c<4;c++) if(pixels[(y*4+x)*4+c]!=expected[c]) {
                    fprintf(stderr,"sampler mode %u xy %u,%u c%u got %u expected %u\n",mode,x,y,c,pixels[(y*4+x)*4+c],expected[c]); failed=1;
                }
            }
        }
        texture.levels=7;texture.filter=0x02022000;texture.addr_u=texture.addr_v=3;
        batch.texture=&texture;
        if(ch_vk_nv2a_draw_decoded_read(target,&surface,&batch,nv2a_backend_decode_texture,pixels,sizeof pixels)!=VK_SUCCESS) failed=1;
        texture.levels=1;
        texture.addr_u=4;
        if(ch_vk_nv2a_draw_decoded_read(target,&surface,&batch,nv2a_backend_decode_texture,pixels,sizeof(pixels))!=VK_ERROR_FEATURE_NOT_PRESENT) failed=1;
    }
    if(!failed) {
        ChVkBackend *backend=NULL;
        struct QueueLockProbe probe={0};
        ChVkBackendConfig config={.physical=gpu,.device=device,.queue=queue,.family=family,
            .guest_base=guest.bytes,.guest_bytes=sizeof(guest.bytes),.decode=nv2a_backend_decode_texture,.precise_queries=1,
            .lock_queue=probe_lock,.queue_context=&probe};
        if(ch_vk_backend_create(&config,&backend)!=VK_ERROR_INITIALIZATION_FAILED || backend) failed=1;
        config.unlock_queue=probe_unlock;
        if(ch_vk_backend_create(&config,&backend)!=VK_SUCCESS || ch_vk_backend_attach(backend)!=VK_SUCCESS) failed=1;
        else {
            const Nv2aBackend *cb=ch_vk_backend_callbacks();
            Nv2aSurface first=surface, second=surface;
            first.color_va=2048; second.color_va=2304;
            cb->clear(&first,&state,0xf3,0xffff0000,0xffffff00);
            cb->clear(&second,&state,0xf3,0xff00ff00,0xffffff00);
            cb->flip();
            for(unsigned i=0;i<16;i++) {
                uint32_t a,b;
                memcpy(&a,guest.bytes+2048+i*4,4); memcpy(&b,guest.bytes+2304+i*4,4);
                if(a!=0xffff0000 || b!=0xff00ff00) failed=1;
            }
            texture=(Nv2aTexture){.offset=2048,.width=4,.height=4,.pitch=16,.levels=1,
                .color=0x12,.filter=0x01010000,.addr_u=3,.addr_v=3};
            batch.vertices=vertices; batch.texture=&texture;
            uint32_t visible=7; batch.zpass_out=&visible;
            cb->draw(&second,&batch); cb->flip();
            batch.zpass_out=NULL;
            if(visible!=23) failed=1;
            /* CPU overwrites target between callbacks: red-only draw must
             * retain CPU blue, rather than the earlier GPU red/green. */
            for(unsigned i=0;i<16;i++) { uint32_t blue=0xff0000ff; memcpy(guest.bytes+2304+i*4,&blue,4); }
            Nv2aRenderState masked=state; masked.color_mask=0x10000;
            Nv2aBatch masked_batch={.vertices=vertices,.count=6,.state=&masked};
            cb->draw(&second,&masked_batch);
            for(unsigned i=0;i<16;i++) {
                uint32_t pixel; memcpy(&pixel,guest.bytes+2304+i*4,4);
                if(pixel!=0xffff00ff) failed=1;
            }
            /* Partial clear must also observe CPU-written untouched channels. */
            for(unsigned i=0;i<16;i++) { uint32_t blue=0xff0000ff; memcpy(guest.bytes+2304+i*4,&blue,4); }
            cb->clear(&second,&state,0x20,0xff00ff00,0);
            for(unsigned i=0;i<16;i++) {
                uint32_t pixel; memcpy(&pixel,guest.bytes+2304+i*4,4);
                if(pixel!=0xff00ffff) failed=1;
            }
            /* Restore the existing assertion's source result. */
            cb->draw(&second,&batch);
            for(unsigned i=0;i<16;i++) {
                uint32_t pixel; memcpy(&pixel,guest.bytes+2304+i*4,4);
                if(pixel!=0xffff0000) failed=1;
            }
            Nv2aRenderState zstate=state;
            zstate.zeta_va=3072; zstate.zeta_pitch=24; zstate.surface_format=0x120;
            zstate.depth_test_enable=1; zstate.depth_write=1; zstate.depth_func=0x201;
            zstate.depth_min=0; zstate.depth_max=1;
            zstate.stencil_enable=1; zstate.stencil_func=0x207; zstate.stencil_ref=7;
            zstate.stencil_func_mask=255; zstate.stencil_write_mask=255;
            zstate.stencil_fail=zstate.stencil_zfail=0x1e00; zstate.stencil_zpass=0x1e01;
            memset(guest.bytes+3072,0xab,96);
            cb->clear(&first,&zstate,0xf3,0xffff0000,0xffffff00);
            cb->clear(&second,&zstate,0xf0,0xff00ff00,0);
            Nv2aVertex layered[6]; memcpy(layered,vertices,sizeof(layered));
            for(unsigned i=0;i<6;i++) { layered[i].z=.25f; layered[i].diffuse=0xffff0000; }
            Nv2aBatch layer={.vertices=layered,.count=6,.state=&zstate};
            cb->draw(&first,&layer);
            zstate.stencil_func=0x202; zstate.stencil_zpass=0x1e00;
            for(unsigned i=0;i<6;i++) { layered[i].z=.75f; layered[i].diffuse=0xffffffff; }
            cb->draw(&second,&layer);
            for(unsigned i=0;i<16;i++) { uint32_t pixel; memcpy(&pixel,guest.bytes+2304+i*4,4); if(pixel!=0xff00ff00) failed=1; }
            for(unsigned y=0;y<4;y++) for(unsigned x=0;x<4;x++) {
                uint32_t cpu=0xffffff07; memcpy(guest.bytes+3072+y*24+x*4,&cpu,4);
            }
            cb->draw(&second,&layer);
            for(unsigned i=0;i<16;i++) { uint32_t pixel; memcpy(&pixel,guest.bytes+2304+i*4,4); if(pixel!=0xffffffff) failed=1; }
            for(unsigned y=0;y<4;y++) for(unsigned b=16;b<24;b++) if(guest.bytes[3072+y*24+b]!=0xab) failed=1;
            /* Drive the real executor method decoder, not callbacks. */
            nv2a_pb_exec_method(0,0x0200,4u<<16); nv2a_pb_exec_method(0,0x0204,4u<<16);
            nv2a_pb_exec_method(0,0x0208,0x128); nv2a_pb_exec_method(0,0x020c,(24u<<16)|16);
            nv2a_pb_exec_method(0,0x0210,2048); nv2a_pb_exec_method(0,0x0214,3072);
            nv2a_pb_exec_method(0,0x0300,0); nv2a_pb_exec_method(0,0x0304,0); nv2a_pb_exec_method(0,0x0308,0);
            nv2a_pb_exec_method(0,0x030c,1); nv2a_pb_exec_method(0,0x0354,0x201); nv2a_pb_exec_method(0,0x035c,1);
            method_float(0x0394,0); method_float(0x0398,1);
            nv2a_pb_exec_method(0,0x1d8c,0xffffff00); nv2a_pb_exec_method(0,0x1d90,0xff000000);
            nv2a_pb_exec_method(0,0x1d94,0xf3);
            nv2a_pb_exec_method(0,0x17c8,0); nv2a_pb_exec_method(0,0x17cc,1);
            nv2a_pb_exec_method(0,0x194c,0xff0000ff); nv2a_pb_exec_method(0,0x1950,0xff0000ff);
            nv2a_pb_exec_method(0,0x17fc,5);
            const float positions[3][4]={{0,0,.25f,1},{8,0,.25f,1},{0,8,.25f,1}};
            for(unsigned i=0;i<3;i++) for(unsigned c=0;c<4;c++) method_float(0x1518+c*4,positions[i][c]);
            nv2a_pb_exec_method(0,0x17fc,0);
            nv2a_pb_exec_method(0,0x17d0,3840);
            uint32_t report,done; memcpy(&report,guest.bytes+3848,4); memcpy(&done,guest.bytes+3852,4);
            if(report!=16 || done) { fprintf(stderr,"executor report count%u done%u\n",report,done); failed=1; }
            for(unsigned i=0;i<16;i++) { uint32_t pixel; memcpy(&pixel,guest.bytes+2048+i*4,4); if(pixel!=0xffff0000) { fprintf(stderr,"executor pixel%u %08x\n",i,pixel); failed=1; break; } }
            nv2a_pb_set_semaphore_target(3968); nv2a_pb_exec_method(0,0x1d70,0x12345678);
            uint32_t semaphore; memcpy(&semaphore,guest.bytes+3968,4); if(semaphore!=0x12345678) failed=1;
            nv2a_pb_set_semaphore_target(0);
            if(cb->draw_raw || cb->flip_async) failed=1;
            if(!failed) puts("PASS: real executor methods -> Vulkan pixels, precise GET_REPORT=16, ordered semaphore release");
        }
        unsigned calls=probe.locks;
        ch_vk_backend_destroy(backend);
        if(calls<8 || probe.locks!=calls+1 || probe.held || probe.locks!=probe.unlocks) failed=1;
        if(!failed) puts("PASS: guest callbacks and destruction serialize the shared host queue; unpaired hooks rejected");
    }
    if(!failed) {
        ChVkBackend *backend=NULL;
        ChVkBackendConfig config={.physical=gpu,.device=device,.queue=queue,.family=family,
            .guest_base=guest.bytes,.guest_bytes=sizeof guest.bytes,.decode=nv2a_backend_decode_texture,
            .precise_queries=1,.resident_color=1,.cache_textures=1};
        Nv2aRenderState rs={.surface_format=0x128,.color_mask=0x01010101};
        Nv2aSurface a=surface,b=surface; a.color_va=2048;b.color_va=2304;
        memset(guest.bytes+2048,0,64);memset(guest.bytes+2304,0,64);
        if(ch_vk_backend_create(&config,&backend)!=VK_SUCCESS || ch_vk_backend_attach(backend)!=VK_SUCCESS) failed=1;
        else {
            const Nv2aBackend *cb=ch_vk_backend_callbacks();
            cb->clear(&a,&rs,0xf0,0xffff0000,0);
            uint32_t first;memcpy(&first,guest.bytes+2048,4);
            if(first || !cb->flush) failed=1;
            uint32_t green=0xff00ff00;memcpy(guest.bytes+2048,&green,4);
            cb->flush();
            for(unsigned i=0;i<16;i++) {
                uint32_t actual;memcpy(&actual,guest.bytes+2048+i*4,4);
                if(actual!=(i?0xffff0000:green)) failed=1;
            }
            cb->clear(&a,&rs,0xf0,0xff0000ff,0);
            Nv2aTexture tx={.offset=2048,.width=4,.height=4,.pitch=16,.color=0x12,.levels=1,
                .filter=0x01010000,.addr_u=3,.addr_v=3};
            Nv2aVertex quad[6]={{0,0,0,1,0xffffffff,0,0},{4,0,0,1,0xffffffff,4,0},
                {4,4,0,1,0xffffffff,4,4},{0,0,0,1,0xffffffff,0,0},
                {4,4,0,1,0xffffffff,4,4},{0,4,0,1,0xffffffff,0,4}};
            uint32_t visible=0;
            Nv2aBatch draw={.vertices=quad,.count=6,.texture=&tx,.state=&rs,.zpass_out=&visible};
            cb->draw(&b,&draw);
            if(visible!=16) failed=1;
            /* A backend semaphore must expose the resident target before the
             * completion word. This exercises the actual executor write path. */
            nv2a_pb_set_semaphore_target(3968);nv2a_pb_exec_method(0,0x1d70,0xabcdef01);
            nv2a_pb_set_semaphore_target(0);
            uint32_t semaphore;memcpy(&semaphore,guest.bytes+3968,4);
            if(semaphore!=0xabcdef01) failed=1;
            for(unsigned i=0;i<16;i++) {uint32_t actual;memcpy(&actual,guest.bytes+2304+i*4,4);if(actual!=0xff0000ff) failed=1;}
            /* Self-sampling must snapshot pending GPU output before decoding. */
            cb->clear(&b,&rs,0xf0,0xffffffff,0);
            tx.offset=2304;
            for(unsigned i=0;i<6;i++) quad[i].diffuse=0xffff0000;
            draw.zpass_out=NULL;cb->draw(&b,&draw);cb->flush();
            for(unsigned i=0;i<16;i++) {uint32_t actual;memcpy(&actual,guest.bytes+2304+i*4,4);if(actual!=0xffff0000) failed=1;}
            cb->clear(&b,&rs,0xf0,0xff00ff00,0);
            nv2a_pb_exec_method(0,0x17d0,3840);
            for(unsigned i=0;i<16;i++) {uint32_t actual;memcpy(&actual,guest.bytes+2304+i*4,4);if(actual!=0xff00ff00) failed=1;}
        }
        ch_vk_backend_destroy(backend);
        if(!failed) puts("PASS: resident color defers readback; CPU pixel edits preserve GPU neighbors; sampled/self targets, precise queries and executor semaphore/report flushes");
        else fprintf(stderr,"FAIL: resident color completion/coherence\n");
    }
    if(!failed) {
        const uint32_t colors[]={0x80204080,0x4080c040};
        for(unsigned st=0;st<2;st++) for(unsigned i=0;i<16;i++) memcpy(guest.bytes+256+st*256+i*4,&colors[st],4);
        Nv2aTexture tx[2]={{.offset=256,.width=4,.height=4,.pitch=16,.color=0x12,.levels=1,.addr_u=3,.addr_v=3,.filter=0x01010000},
            {.offset=512,.width=4,.height=4,.color=0x06,.levels=1,.addr_u=3,.addr_v=3,.filter=0x01010000}};
        Nv2aCombiner rc={.control=1,.stage_program=1|(1<<5)};
        rc.color_icw[0]=0x08090000; rc.alpha_icw[0]=0x18190000;
        rc.color_ocw[0]=rc.alpha_ocw[0]=0xc0;
        Nv2aVertexEx ex[6]={0};
        for(unsigned i=0;i<6;i++) { ex[i].tex[0][0]=ex[i].tex[0][1]=2; ex[i].tex[0][3]=1;
            ex[i].tex[1][0]=ex[i].tex[1][1]=.5f; ex[i].tex[1][3]=1; }
        Nv2aBatch combined={.vertices=vertices,.count=6,.state=&state,.rc=&rc,.ext=ex,.stage={&tx[0],&tx[1]}};
        VkResult r=ch_vk_nv2a_draw_decoded_read(target,&surface,&combined,nv2a_backend_decode_texture,pixels,sizeof(pixels));
        if(r!=VK_SUCCESS) { fprintf(stderr,"sampled combiner result%d\n",r); failed=1; }
        const unsigned char expected[]={16,48,32,32};
        for(unsigned i=0;i<sizeof(pixels);i++) {
            int delta=pixels[i]-expected[i%4];
            if(delta < -1 || delta > 1) { fprintf(stderr,"sampled combiner byte%u got%u expected%u\n",i,pixels[i],expected[i%4]); failed=1; }
        }
    }
    if(!failed) {
        ring_mode=1;
        setenv("RECOMP_PB_EXEC","1",1); setenv("RECOMP_PB_WALK","dma",1);
        ChVkBackend *backend=NULL;
        ChVkBackendConfig config={.physical=gpu,.device=device,.queue=queue,.family=family,
            .guest_base=(void *)xbox_GetMemoryOffset(),.guest_bytes=(size_t)0x80000000u+sizeof(guest.bytes),
            .decode=nv2a_backend_decode_texture,.precise_queries=1};
        if(ch_vk_backend_create(&config,&backend)!=VK_SUCCESS || ch_vk_backend_attach(backend)!=VK_SUCCESS) failed=1;
        else {
            uint32_t main_words[]={0x302, (1u<<18)|0x17d0,3840, (1u<<18)|0x1d70,0x87654321,0x201};
            memcpy(guest.bytes+0x100,main_words,sizeof(main_words));
            uint32_t final_words[]={(1u<<18)|0x0358,0x01010101}; memcpy(guest.bytes+0x200,final_words,sizeof(final_words));
            uint32_t stream[96]; unsigned n=0;
#define EMIT(method,value) do { stream[n++]=(1u<<18)|(method); stream[n++]=(value); } while(0)
            EMIT(0x0200,4u<<16); EMIT(0x0204,4u<<16); EMIT(0x0208,0x128);
            EMIT(0x020c,(24u<<16)|16); EMIT(0x0210,2048); EMIT(0x0214,3072);
            EMIT(0x1d8c,0xffffff00); EMIT(0x1d90,0xff000000); EMIT(0x1d94,0xf3);
            EMIT(0x17c8,0); EMIT(0x17cc,1);
            /* Non-increasing packet writes diffuse twice; final value is red. */
            stream[n++]=0x40000000u|(2u<<18)|0x194c; stream[n++]=0xff00ff00; stream[n++]=0xff0000ff;
            EMIT(0x1950,0xff0000ff); EMIT(0x17fc,5);
            const float pos[3][4]={{0,0,.25f,1},{8,0,.25f,1},{0,8,.25f,1}};
            for(unsigned i=0;i<3;i++) { stream[n++]=(4u<<18)|0x1518; for(unsigned c=0;c<4;c++) memcpy(&stream[n++],&pos[i][c],4); }
            EMIT(0x17fc,0); stream[n++]=0x00020000; /* RETURN */
#undef EMIT
            memcpy(guest.bytes+0x300,stream,n*4);
            memset(guest.bytes+3840,0xff,16);
            nv2a_pb_set_semaphore_target(0x80000000u+3968);
            dma_get=0x100; nv2a_pb_resync(dma_get); nv2a_pb_scan(0x208);
            uint32_t count,done,sem; memcpy(&count,guest.bytes+3848,4); memcpy(&done,guest.bytes+3852,4); memcpy(&sem,guest.bytes+3968,4);
            if(dma_get!=0x208 || count!=16 || done || sem!=0x87654321) {
                fprintf(stderr,"ring GET%x count%u done%u semaphore%x\n",dma_get,count,done,sem); failed=1;
            }
            for(unsigned i=0;i<16;i++) { uint32_t pixel; memcpy(&pixel,guest.bytes+2048+i*4,4); if(pixel!=0xffff0000) failed=1; }
            nv2a_pb_set_semaphore_target(0);
            if(!failed) puts("PASS: real DMA ring CALL/RETURN/JUMP, increment/nonincrement packets -> Vulkan pixels/report/semaphore/GET");
        }
        ch_vk_backend_destroy(backend); ring_mode=0;
    }
    if(!failed) {
        struct ColorEvents events={0}; ChVkBackend *backend=NULL;
        ChVkBackendConfig config={.physical=gpu,.device=device,.queue=queue,.family=family,
            .guest_base=guest.bytes,.guest_bytes=sizeof guest.bytes,.decode=nv2a_backend_decode_texture,
            .render_context=&events,.guest_color_changed=color_changed};
        if(ch_vk_backend_create(&config,&backend)!=VK_SUCCESS || ch_vk_backend_attach(backend)!=VK_SUCCESS) failed=1;
        else {
            const Nv2aBackend *cb=ch_vk_backend_callbacks();
            Nv2aSurface argb={.color_va=1024,.width=4,.height=4,.pitch=20,.bytes_per_pixel=4};
            Nv2aSurface rgb565={.color_va=1536,.width=4,.height=4,.pitch=10,.bytes_per_pixel=2};
            cb->clear(&argb,&state,0xf3,0xff123456,0xffffff00);
            cb->clear(&rgb565,&state,0xf3,0xff123456,0xffffff00);
            cb->flip(); cb->flip();
            if(events.initial!=2 || events.edits) failed=1;
            guest.bytes[argb.color_va+16]^=0xff; guest.bytes[rgb565.color_va+8]^=0xff;
            cb->flip();
            if(events.edits) failed=1; /* Row padding is not pixel data. */
            uint32_t cpu_color=0xff2468ac; memcpy(guest.bytes+argb.color_va,&cpu_color,4);
            Nv2aBatch draw={.vertices=vertices,.count=6,.state=&state};
            cb->draw(&rgb565,&draw); /* Observe other source surface before target draw. */
            if(events.edits!=1 || events.va!=argb.color_va || events.pixel[0]!=0x24 || events.pixel[1]!=0x68 || events.pixel[2]!=0xac || events.pixel[3]!=255) failed=1;
            cb->flip(); if(events.edits!=1) failed=1;
            uint16_t cpu565=0xf800; memcpy(guest.bytes+rgb565.color_va,&cpu565,2);
            cb->flip();
            if(events.edits!=2 || events.va!=rgb565.color_va || events.pixel[0]!=255 || events.pixel[1] || events.pixel[2] || events.pixel[3]!=255) failed=1;
            cb->flip(); if(events.edits!=2) failed=1;
            if(!failed) puts("PASS: exact guest color coherence, ARGB/RGB565 CPU edits before sampling/flip, no false edits from quantized GPU writes or padding");
        }
        ch_vk_backend_destroy(backend);
    }
    if(!failed) {
        struct StereoHost host={0};
        if(ch_vk_frame_mailbox_create(&test_mailbox)!=VK_SUCCESS) return 1;
        const float q[]={0,0,0,1},fov[]={-1,1,1,-1};
        for(unsigned eye=0;eye<2;eye++) {
            if(ch_vk_target_create(gpu,device,queue,family,37,19,&host.eyes[eye])!=VK_SUCCESS) return 1;
            float position[]={eye?.3f:-.3f,0,0};
            vr_eye_matrix(q,position,fov,1,.1f,100,host.view[eye].gl_eye_vp);
        }
        ChVkBackendConfig config={.physical=gpu,.device=device,.queue=queue,.family=family,
            .guest_base=guest.bytes,.guest_bytes=sizeof(guest.bytes),.decode=nv2a_backend_decode_texture,
            .precise_queries=1,.observe_raw=host_raw,.render_context=&host,.render_clear=host_clear,.render_draw=host_draw,.render_flip=host_flip,.present_rgba=host_present};
        ChVkBackend *backend=NULL;
        if(ch_vk_backend_create(&config,&backend)!=VK_SUCCESS || ch_vk_backend_attach(backend)!=VK_SUCCESS) failed=1;
        else {
            const Nv2aBackend *cb=ch_vk_backend_callbacks();
            Nv2aSurface s={.color_va=2048,.width=4,.height=4,.pitch=16,.bytes_per_pixel=4,.aa_sx=1,.aa_sy=1};
            Nv2aRenderState state={.color_mask=0x01010101};
            Nv2aVertex v[]={
                {.x=1.25f,.y=2.75f,.rhw=.5f,.diffuse=0xffff0000},
                {.x=2.75f,.y=2.75f,.rhw=.5f,.diffuse=0xffff0000},
                {.x=2,.y=1.25f,.rhw=.5f,.diffuse=0xffff0000}};
            Nv2aBatch batch={.vertices=v,.count=3,.state=&state};
            Nv2aFixedState ff={0}; ff.proj[0]=2; ff.proj[5]=-2; ff.proj[10]=1; ff.proj[14]=1;
            ff.c59[0]=ff.c59[1]=2;
            Nv2aRawGeometry raw={.kind=NV2A_RAW_FIXED,.ff=&ff,.aa_sx=2,.aa_sy=2};
            Nv2aBatch raw_batch={.raw=&raw,.state=&state};
            s.aa_sx=s.aa_sy=2;
            cb->clear(&s,&state,0xf3,0xff0000ff,0xffffff00);
            if(!cb->draw_raw || cb->draw_raw(&s,&raw_batch)!=0) failed=1;
            if(host.view[0].game_projection.kx!=2 || host.view[0].game_projection.ky!=-2 ||
               host.view[0].game_projection.cx!=2 || host.view[0].game_projection.cy!=2) failed=1;
            /* A failed recovery must not overwrite a known projection. */
            VrGameProj sentinel={17,19,23,29,31},recovered=sentinel;
            ff.proj[14]=0;
            if(ch_vk_nv2a_projection_from_raw(&raw_batch,&recovered)!=VK_ERROR_INITIALIZATION_FAILED || memcmp(&sentinel,&recovered,sizeof(sentinel))) failed=1;
            ff.proj[14]=1;
            ff.use_composite=1;
            ff.mv[0][0]=ff.mv[0][5]=ff.mv[0][10]=ff.mv[0][15]=1;
            ff.mv[0][3]=3; ff.mv[0][7]=4; ff.mv[0][11]=5;
            ff.composite[0]=2; ff.composite[3]=6;
            ff.composite[5]=-2; ff.composite[7]=-8;
            ff.composite[10]=1; ff.composite[11]=5; ff.composite[14]=1; ff.composite[15]=5;
            if(ch_vk_nv2a_projection_from_raw(&raw_batch,&recovered)!=VK_SUCCESS || recovered.kx!=2 || recovered.ky!=-2 || recovered.cx!=2 || recovered.cy!=2 || recovered.inv_pz!=1) failed=1;
            memset(ff.mv[0],0,sizeof(ff.mv[0])); recovered=sentinel;
            if(ch_vk_nv2a_projection_from_raw(&raw_batch,&recovered)!=VK_ERROR_INITIALIZATION_FAILED || memcmp(&sentinel,&recovered,sizeof(sentinel))) failed=1;
            raw.kind=NV2A_RAW_PROGRAM;
            if(ch_vk_nv2a_projection_from_raw(&raw_batch,&recovered)!=VK_ERROR_FEATURE_NOT_PRESENT) failed=1;
            s.aa_sx=s.aa_sy=1;
            cb->draw(&s,&batch);
            nv2a_backend_presented_override(1,s.color_va);
            cb->flip();
            ChVkCpuFrame first={0},second={0};
            unsigned char expected[64];
            if(ch_vk_nv2a_load_color(&s,expected,sizeof expected,guest.bytes,sizeof guest.bytes)!=VK_SUCCESS ||
               ch_vk_frame_mailbox_take(test_mailbox,&first)!=VK_SUCCESS || first.sequence!=1 || first.bytes!=64 || first.surface.aa_sx!=1 || first.surface.aa_sy!=1 ||
               (!first.rgba || memcmp(first.rgba,expected,64))) failed=1;
            /* CPU-written framebuffer must be presented, not stale GPU staging. */
            const uint32_t cpu_pixel=0xff123456; memcpy(guest.bytes+s.color_va,&cpu_pixel,4);
            cb->flip();
            if(ch_vk_frame_mailbox_take(test_mailbox,&second)!=VK_SUCCESS || second.sequence!=2 ||
               second.rgba[0]!=0x12 || second.rgba[1]!=0x34 || second.rgba[2]!=0x56 || second.rgba[3]!=255 ||
               (!first.rgba || memcmp(first.rgba,expected,64))) failed=1;
            if(second.rgba && (ch_vk_target_import_color(target,second.rgba,second.bytes)!=VK_SUCCESS ||
               ch_vk_target_clear_aspects_read(target,0,NULL,0,0,pixels,sizeof pixels)!=VK_SUCCESS ||
               memcmp(pixels,second.rgba,64))) failed=1;
            if(ch_vk_frame_mailbox_publish(test_mailbox,&s,expected,63)!=VK_ERROR_INITIALIZATION_FAILED) failed=1;
            ch_vk_cpu_frame_release(&first); ch_vk_cpu_frame_release(&second);
            nv2a_backend_presented_override(0,0);
            if(!failed) puts("PASS: backend flip -> owned host frame -> Vulkan upload includes CPU writes and retains prior frame ownership");
            if(host.raws!=1 || host.clears!=1 || host.draws!=1 || host.flips!=2 || !memcmp(host.rgba[0],host.rgba[1],sizeof(host.rgba[0]))) failed=1;
            unsigned counts[2]={0},sum[2]={0};
            for(unsigned eye=0;eye<2;eye++) for(unsigned y=0;y<19;y++) for(unsigned x=0;x<37;x++)
                if(host.rgba[eye][(y*37+x)*4]==255) { counts[eye]++; sum[eye]+=x; }
            if(!counts[0] || !counts[1] || sum[0]*counts[1]<=sum[1]*counts[0]) failed=1;
        }
        ch_vk_backend_destroy(backend);
        ch_vk_frame_mailbox_destroy(test_mailbox); test_mailbox=NULL;
        for(unsigned eye=0;eye<2;eye++) ch_vk_target_destroy(host.eyes[eye]);
        if(!failed) puts("PASS: registered backend synchronous clear/draw/flip host hooks -> distinct stereo eye targets");
    }
    if(!failed) {
        ChVkMenu *menu=NULL;
        const size_t bytes=(size_t)XM_W*XM_H*4;
        unsigned char *expected=malloc(bytes),*actual=malloc(bytes);
        if(!expected || !actual || ch_vk_menu_create(gpu,device,queue,family,&menu)!=VK_SUCCESS) failed=1;
        else {
            XmItem items[]={{.label="Renderer",.value="Vulkan"},{.label="Display",.value="Immersive"},{.label="Restart",.value=""}};
            for(int pass=0;pass<2 && !failed;pass++) {
                int hover=pass?1:-1;
                xm_paint(expected,"Virtua Cop 3",items,3,hover,"Select to change");
                VkResult r=ch_vk_menu_paint(menu,"Virtua Cop 3",items,3,hover,"Select to change");
                if(r==VK_SUCCESS) r=ch_vk_target_clear_aspects_read(ch_vk_menu_target(menu),0,NULL,0,0,actual,bytes);
                if(r!=VK_SUCCESS || memcmp(expected,actual,bytes)) { fprintf(stderr,"Vulkan menu RGBA mismatch pass%d\n",pass); failed=1; }
            }
            if(!failed) {
                FILE *image=fopen("/data/local/tmp/vc3-vulkan-menu.rgba","wb");
                if(!image || fwrite(actual,1,bytes,image)!=bytes) failed=1;
                if(image) fclose(image);
                puts("PASS: reference CPU menu painter -> Vulkan target, every RGBA byte including alpha, hover/repaint");
            }
        }
        ch_vk_menu_destroy(menu); free(expected); free(actual);
    }
    if(!failed) {
        ChVkTarget *gun=NULL; size_t bytes=256*256*4;
        unsigned char *pixels=malloc(bytes),*previous=malloc(bytes);
        if(!pixels || !previous || ch_vk_target_create(gpu,device,queue,family,256,256,&gun)!=VK_SUCCESS) failed=1;
        else {
            float vp[16]; vr_proj_fov(-.7f,.7f,.7f,-.7f,.01f,10,vp);
            QoFrame f={.gun_valid=1,.show_gun=1,.gun_q={0,0,0,1},.gun_p={.04f,-.01f,-.35f}};
            const float transparent[]={0,0,0,0};
            for(unsigned sight=0;sight<QO_SIGHT_COUNT && !failed;sight++) for(unsigned kick=0;kick<2 && !failed;kick++) {
                f.sight=(int)sight; f.kick=(float)kick;
                VkResult r=ch_vk_target_clear_read(gun,transparent,pixels,bytes);
                if(r==VK_SUCCESS) r=ch_vk_overlay_gun_read(gun,&f,vp,pixels,bytes);
                if(r!=VK_SUCCESS) { failed=1; break; }
                unsigned opaque=0;
                for(unsigned i=0;i<256*256;i++) { if(pixels[i*4+3]==255) opaque++; else if(pixels[i*4+3]) failed=1; }
                if(opaque<100) failed=1;
                if(kick && !memcmp(previous,pixels,bytes)) failed=1;
                memcpy(previous,pixels,bytes);
            }
            const float eye[3]={0,0,0};
            f.end[0]=.04f; f.end[1]=.02f; f.end[2]=-2; f.beam=f.dot=f.flash=1;
            for(unsigned sight=0;sight<QO_SIGHT_COUNT && !failed;sight++) {
                f.sight=(int)sight; f.kick=0;
                if(ch_vk_target_clear_read(gun,transparent,pixels,bytes)!=VK_SUCCESS ||
                   ch_vk_overlay_read(gun,&f,vp,eye,.035f,pixels,bytes)!=VK_SUCCESS) { failed=1; break; }
                unsigned translucent=0;
                for(unsigned k=0;k<256*256;k++) {
                    if(pixels[k*4+3] && pixels[k*4+3]<255) translucent++;
                    for(unsigned c=0;c<3;c++) if(pixels[k*4+c]>pixels[k*4+3]+1) failed=1;
                }
                if(translucent<10) failed=1;
                memcpy(previous,pixels,bytes);
                const float shifted_eye[3]={.025f,.01f,0};
                if(ch_vk_overlay_read(gun,&f,vp,shifted_eye,.035f,pixels,bytes)!=VK_SUCCESS) failed=1;
            }
            if(!failed) {
                FILE *image=fopen("/data/local/tmp/vc3-vulkan-overlay.rgba","wb");
                if(!image || fwrite(pixels,1,bytes,image)!=bytes) failed=1;
                if(image) fclose(image);
                puts("PASS: complete shared overlay geometry: sights, shifted eye, beam/dot/flash, premultiplied alpha");
            }
            memcpy(previous,pixels,bytes);
            f.gun_valid=0;
            if(ch_vk_overlay_gun_read(gun,&f,vp,pixels,bytes)!=VK_SUCCESS || memcmp(previous,pixels,bytes)) failed=1;
            memset(f.gun_q,0,sizeof f.gun_q);
            if(ch_vk_overlay_read(gun,&f,vp,eye,.035f,pixels,bytes)!=VK_SUCCESS || memcmp(previous,pixels,bytes)) failed=1;
            if(!failed) {
                FILE *image=fopen("/data/local/tmp/vc3-vulkan-gun.rgba","wb");
                if(!image || fwrite(pixels,1,bytes,image)!=bytes) failed=1;
                if(image) fclose(image);
                puts("PASS: shared gun mesh, three sight housings, recoil changes and invalid-pose visibility on Vulkan");
            }
        }
        ch_vk_target_destroy(gun); free(pixels); free(previous);
    }
    if(!failed) {
        extern int ch_vk_overlay_parity(VkPhysicalDevice,VkDevice,VkQueue,uint32_t);
        failed=ch_vk_overlay_parity(gpu,device,queue,family);
        extern int ch_test_frame_mailbox(void);
        if(!failed) failed=ch_test_frame_mailbox();
        extern int ch_test_scene(VkPhysicalDevice,VkDevice,VkQueue,uint32_t,ChVkDecodeTexture);
        if(!failed) failed=ch_test_scene(gpu,device,queue,family,nv2a_backend_decode_texture);
        extern int ch_test_submission(VkPhysicalDevice,VkDevice,VkQueue,uint32_t,void *,size_t,ChVkDecodeTexture);
        if(!failed) failed=ch_test_submission(gpu,device,queue,family,guest.bytes,sizeof guest.bytes,nv2a_backend_decode_texture);
    }
    if(!failed) {
        extern int ch_test_texture_cache(VkPhysicalDevice,VkDevice,VkQueue,uint32_t,void *,size_t,ChVkDecodeTexture);
        failed=ch_test_texture_cache(gpu,device,queue,family,guest.bytes,sizeof guest.bytes,nv2a_backend_decode_texture);
    }
    if(!failed) failed=check_simple_combiner(gpu,device,queue,family);
    if(!failed && getenv("CHIHIRO_VULKAN_BENCH")) failed=bench_guest_draws(gpu,device,queue,family);
    ch_vk_target_destroy(target);
    if(!failed) puts("PASS: real executor decoder + Vulkan sampled draws: padded linear ARGB, Morton ARGB, DXT1/3/5, repeat/mirror/clamp, bilinear, guest ARGB8888/RGB565 render-to-texture");
    return failed;
}
