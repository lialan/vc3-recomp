/* Standalone offscreen clear/readback test. No game or OpenXR session. */
#include "vk_target.h"
#include "vk_nv2a.h"
#include "vk_texture.h"
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define CHECK(call) do { VkResult r=(call); if(r!=VK_SUCCESS) { fprintf(stderr,"%s: %d\n",#call,r); goto done; } } while(0)
/* Fixture decoder: exercises the adapter contract, not guest-memory decoding. */
static int fixture_decode(const Nv2aTexture *tx,uint32_t *argb) {
    if(tx->color!=0x1234) return 0;
    for(unsigned y=0;y<tx->height;y++) for(unsigned x=0;x<tx->width;x++)
        argb[y*tx->width+x]=0xff000000u|((x*80u)<<16)|((y*80u)<<8)|32u;
    return 1;
}
int ch_test_guest_textures(VkPhysicalDevice,VkDevice,VkQueue,uint32_t);
static float smooth_reference(float low,float high,float x) {
    float t=fminf(1,fmaxf(0,(x-low)/(high-low))); return t*t*(3-2*t);
}
int main(void) {
    setenv("RECOMP_PB_DECODE_THREAD","0",1);
    int result=1;
    VkInstance instance=VK_NULL_HANDLE;
    VkDevice device=VK_NULL_HANDLE;
    VkCommandPool pool=VK_NULL_HANDLE;
    VkFence fence=VK_NULL_HANDLE;
    VkPhysicalDevice *physical=NULL;
    VkQueueFamilyProperties *families=NULL;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Chihiro Vulkan probe",.apiVersion=VK_API_VERSION_1_0};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    CHECK(vkCreateInstance(&ici,NULL,&instance));
    uint32_t count=0;
    CHECK(vkEnumeratePhysicalDevices(instance,&count,NULL));
    if(!count) goto done;
    physical=calloc(count,sizeof(*physical)); if(!physical) goto done;
    CHECK(vkEnumeratePhysicalDevices(instance,&count,physical));
    VkPhysicalDevice gpu=VK_NULL_HANDLE;
    uint32_t family=0;
    for(uint32_t i=0;i<count && !gpu;i++) {
        uint32_t n=0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical[i],&n,NULL);
        families=calloc(n,sizeof(*families)); if(!families) goto done;
        vkGetPhysicalDeviceQueueFamilyProperties(physical[i],&n,families);
        for(uint32_t j=0;j<n;j++) if(families[j].queueCount && (families[j].queueFlags&VK_QUEUE_GRAPHICS_BIT)) { gpu=physical[i];family=j;break; }
        free(families); families=NULL;
    }
    if(!gpu) goto done;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu,&props);
    printf("device=%s vendor=%04x deviceID=%04x api=%u.%u.%u\n",props.deviceName,props.vendorID,props.deviceID,VK_VERSION_MAJOR(props.apiVersion),VK_VERSION_MINOR(props.apiVersion),VK_VERSION_PATCH(props.apiVersion));
    float priority=1;
    VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkPhysicalDeviceFeatures enabled;
    vkGetPhysicalDeviceFeatures(gpu,&enabled);
    if(!enabled.occlusionQueryPrecise) goto done;
    enabled=(VkPhysicalDeviceFeatures){.occlusionQueryPrecise=VK_TRUE};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci,.pEnabledFeatures=&enabled};
    CHECK(vkCreateDevice(gpu,&dci,NULL,&device));
    VkQueue queue;
    vkGetDeviceQueue(device,family,0,&queue);
    VkCommandPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    CHECK(vkCreateCommandPool(device,&pci,NULL,&pool));
    VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(device,&ai,&command));
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(command,&bi));
    CHECK(vkEndCommandBuffer(command));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(device,&fi,NULL,&fence));
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    CHECK(vkQueueSubmit(queue,1,&si,fence));
    CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,5000000000ULL));
    ChVkTarget *target=NULL;
    CHECK(ch_vk_target_create(gpu,device,queue,family,37,19,&target));
    unsigned char pixels[37*19*4];
    const unsigned char expected[][4]={{0,0,0,255},{255,0,255,0},{17,85,170,238},{0,255,0,255}};
    int mismatch=0;
    for(unsigned pass=0;pass<sizeof(expected)/sizeof(expected[0]);pass++) {
        float rgba[4];
        for(unsigned c=0;c<4;c++) rgba[c]=expected[pass][c]/255.0f;
        VkResult r=ch_vk_target_clear_read(target,rgba,pixels,sizeof(pixels));
        if(r!=VK_SUCCESS) { fprintf(stderr,"readback failed: %d\n",r); mismatch=1; break; }
        for(unsigned i=0;i<sizeof(pixels);i++) if(pixels[i]!=expected[pass][i%4]) {
            fprintf(stderr,"pass %u byte %u: got %u expected %u\n",pass,i,pixels[i],expected[pass][i%4]);
            mismatch=1; break;
        }
        if(mismatch) break;
    }
    if(!mismatch) {
        /* Nontrivial index order: vertices 0/1/2 form a degenerate triangle;
         * only fetching the supplied indices covers the full target. */
        const ChVkColorVertex vertices[]={
            {{-1,-1},{1,0,0,1}}, {{-1,-1},{1,0,0,1}},
            {{-1,-1},{1,0,0,1}}, {{1,-1},{1,0,0,1}},
            {{1,1},{1,0,0,1}}, {{-1,1},{1,0,0,1}}};
        const uint16_t indices[]={2,3,4,2,4,5};
        const float clear[]={0,0,1,0};
        VkResult r=ch_vk_target_draw_read(target,clear,vertices,6,indices,6,pixels,sizeof(pixels));
        if(r!=VK_SUCCESS) { fprintf(stderr,"indexed draw failed: %d\n",r); mismatch=1; }
        for(unsigned i=0;!mismatch && i<sizeof(pixels);i++) {
            unsigned expected_byte=(i%4==0 || i%4==3)?255:0;
            if(pixels[i]!=expected_byte) {
                fprintf(stderr,"indexed quad byte %u: %u expected %u\n",i,pixels[i],expected_byte);
                mismatch=1;
            }
        }
        /* Small triangle: outside stays blue; inside has barycentric RGB.
         * Pixel center (18.5,9.5) is NDC (0,0): weights .25,.25,.5. */
        const ChVkColorVertex tri[]={
            {{-.8f,-.8f},{1,0,0,1}},{{.8f,-.8f},{0,1,0,1}},{{0,.8f},{0,0,1,1}}};
        const uint16_t ti[]={0,1,2};
        if(!mismatch) {
            r=ch_vk_target_draw_read(target,clear,tri,3,ti,3,pixels,sizeof(pixels));
            if(r!=VK_SUCCESS) mismatch=1;
            const unsigned center=(9*37+18)*4;
            const int want[]={64,64,128,255};
            for(unsigned c=0;!mismatch && c<4;c++) {
                int delta=(int)pixels[center+c]-want[c];
                if(delta < -1 || delta > 1) { fprintf(stderr,"interpolation channel %u: %u\n",c,pixels[center+c]); mismatch=1; }
            }
            if(pixels[0] || pixels[1] || pixels[2]!=255 || pixels[3]) mismatch=1;
        }
        /* A clear after rendering exercises transition back to transfer. */
        if(!mismatch) {
            r=ch_vk_target_clear_read(target,clear,pixels,sizeof(pixels));
            if(r!=VK_SUCCESS) mismatch=1;
            for(unsigned i=0;!mismatch && i<sizeof(pixels);i++)
                if(pixels[i]!=(i%4==2?255:0)) mismatch=1;
        }
    }
    if(!mismatch) {
        ChVkColorVertex quad[]={
            {{-1,-1},{1,.5f,0,.5f}},{{1,-1},{1,.5f,0,.5f}},
            {{1,1},{1,.5f,0,.5f}},{{-1,1},{1,.5f,0,.5f}}};
        const uint16_t qi[]={0,1,2,0,2,3};
        const float bg[]={0,0,1,1};
        struct BlendCase { uint32_t src,dst,eq,mask,constant; int want[4]; } cases[]={
            {0x302,0x303,0x8006,0x01010101,0,{128,64,128,191}},
            {1,1,0x8006,0x01010101,0,{255,128,255,255}},
            {1,1,0x800a,0x01010101,0,{255,128,0,0}},
            {1,1,0x800b,0x01010101,0,{0,0,255,128}},
            {0,0,0x8007,0x01010101,0,{0,0,0,128}},
            {0,0,0x8008,0x01010101,0,{255,128,255,255}},
            {1,0,0x8006,0x00010000,0,{255,0,255,255}},
            {0x8001,0x8002,0x8006,0x01010101,0x804080c0,{64,64,63,191}},
            {1,0,0x8006,0,0,{0,0,255,255}}
        };
        for(unsigned j=0;!mismatch && j<sizeof(cases)/sizeof(cases[0]);j++) {
            struct BlendCase *c=&cases[j];
            Nv2aRenderState state={.blend_enable=1,.blend_src=c->src,.blend_dst=c->dst,
                .blend_eq=c->eq,.color_mask=c->mask,.blend_color=c->constant};
            VkResult r=ch_vk_target_draw_state_read(target,bg,quad,4,qi,6,&state,pixels,sizeof(pixels));
            if(r!=VK_SUCCESS) { fprintf(stderr,"blend %u result %d\n",j,r); mismatch=1; break; }
            for(unsigned i=0;i<sizeof(pixels);i++) {
                int d=pixels[i]-c->want[i%4];
                if(d < -1 || d > 1) {
                    fprintf(stderr,"blend %u byte %u: %u expected %d\n",j,i,pixels[i],c->want[i%4]);
                    mismatch=1; break;
                }
            }
        }
        Nv2aRenderState bad={.blend_enable=1,.blend_eq=0xdead,.color_mask=0x01010101};
        if(ch_vk_target_draw_state_read(target,bg,quad,4,qi,6,&bad,pixels,sizeof(pixels))!=VK_ERROR_FEATURE_NOT_PRESENT)
            mismatch=1;
        /* Rejection must leave the target usable. */
        if(ch_vk_target_draw_read(target,bg,quad,4,qi,6,pixels,sizeof(pixels))!=VK_SUCCESS) mismatch=1;
    }
    if(!mismatch) {
        const ChVkColorVertex layers[]={
            {{-1,-1,.25f},{1,0,0,1}},{{3,-1,.25f},{1,0,0,1}},{{-1,3,.25f},{1,0,0,1}},
            {{-1,-1,.75f},{0,1,0,1}},{{3,-1,.75f},{0,1,0,1}},{{-1,3,.75f},{0,1,0,1}}};
        const uint16_t ix[]={0,1,2,3,4,5};
        const float bg[]={0,0,1,1};
        struct DepthCase { uint32_t func,write,stencil,mask; int want[4]; } cases[]={
            {0x201,1,0,255,{255,0,0,255}}, /* LESS: near writes, far fails */
            {0x201,0,0,255,{0,255,0,255}}, /* No write: both pass */
            {0x204,1,0,255,{0,0,255,255}}, /* GREATER: both fail clear 1 */
            {0x200,1,0,255,{0,0,255,255}}, /* NEVER */
            {0x207,1,0,255,{0,255,0,255}}, /* ALWAYS */
            {0x207,0,1,255,{255,0,0,255}}, /* EQ 0, increment: second fails */
            {0x207,0,1,0,{0,255,0,255}} /* stencil write masked out */
        };
        for(unsigned j=0;!mismatch && j<sizeof(cases)/sizeof(cases[0]);j++) {
            struct DepthCase *c=&cases[j];
            Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=1,
                .depth_write=c->write,.depth_func=c->func,.stencil_enable=c->stencil,
                .stencil_func=0x202,.stencil_func_mask=255,.stencil_write_mask=c->mask,
                .stencil_fail=0x1e00,.stencil_zfail=0x1e00,.stencil_zpass=0x1e02};
            VkResult r=ch_vk_target_draw_state_read(target,bg,layers,6,ix,6,&state,pixels,sizeof(pixels));
            if(r!=VK_SUCCESS) { fprintf(stderr,"depth/stencil %u result %d\n",j,r); mismatch=1; break; }
            for(unsigned i=0;i<sizeof(pixels);i++) if(pixels[i]!=c->want[i%4]) {
                fprintf(stderr,"depth/stencil %u byte %u: %u expected %d\n",j,i,pixels[i],c->want[i%4]);
                mismatch=1; break;
            }
        }
    }
    if(!mismatch) {
        ChVkColorVertex red[]={{{-1,-1,.25f},{1,0,0,1}},{{3,-1,.25f},{1,0,0,1}},{{-1,3,.25f},{1,0,0,1}}};
        ChVkColorVertex green[]={{{-1,-1,.75f},{0,1,0,1}},{{3,-1,.75f},{0,1,0,1}},{{-1,3,.75f},{0,1,0,1}}};
        const uint16_t ix[]={0,1,2};
        const float bg[]={0,0,1,1};
        Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=1,.depth_write=1,.depth_func=0x201};
        VkResult r=ch_vk_target_draw_state_read(target,bg,red,3,ix,3,&state,pixels,sizeof(pixels));
        if(r==VK_SUCCESS) r=ch_vk_target_draw_state_read(target,NULL,green,3,ix,3,&state,pixels,sizeof(pixels));
        if(r!=VK_SUCCESS) mismatch=1;
        for(unsigned i=0;!mismatch && i<sizeof(pixels);i++) if(pixels[i]!=(i%4==0||i%4==3?255:0)) mismatch=1;
        /* Depth-only clear retains red, then allows the farther green batch. */
        r=ch_vk_target_clear_aspects_read(target,2,NULL,1,0,pixels,sizeof(pixels));
        if(r!=VK_SUCCESS) mismatch=1;
        for(unsigned i=0;!mismatch && i<sizeof(pixels);i++) if(pixels[i]!=(i%4==0||i%4==3?255:0)) mismatch=1;
        r=ch_vk_target_draw_state_read(target,NULL,green,3,ix,3,&state,pixels,sizeof(pixels));
        if(r!=VK_SUCCESS) mismatch=1;
        for(unsigned i=0;!mismatch && i<sizeof(pixels);i++) if(pixels[i]!=(i%4==1||i%4==3?255:0)) mismatch=1;
        /* Stencil created by one batch controls a later batch. */
        state=(Nv2aRenderState){.stencil_enable=1,.stencil_func=0x202,.stencil_func_mask=255,
            .stencil_write_mask=255,.stencil_fail=0x1e00,.stencil_zfail=0x1e00,.stencil_zpass=0x1e02};
        r=ch_vk_target_draw_state_read(target,bg,red,3,ix,3,&state,pixels,sizeof(pixels));
        state.color_mask=0x01010101;
        if(r==VK_SUCCESS) r=ch_vk_target_draw_state_read(target,NULL,green,3,ix,3,&state,pixels,sizeof(pixels));
        if(r!=VK_SUCCESS) mismatch=1;
        for(unsigned i=0;!mismatch && i<sizeof(pixels);i++) if(pixels[i]!=(i%4==2||i%4==3?255:0)) mismatch=1;
        r=ch_vk_target_clear_aspects_read(target,4,NULL,0,0,pixels,sizeof(pixels));
        if(r==VK_SUCCESS) r=ch_vk_target_draw_state_read(target,NULL,green,3,ix,3,&state,pixels,sizeof(pixels));
        if(r!=VK_SUCCESS) mismatch=1;
        for(unsigned i=0;!mismatch && i<sizeof(pixels);i++) if(pixels[i]!=(i%4==1||i%4==3?255:0)) mismatch=1;
        if(mismatch) fprintf(stderr,"persistent color/depth/stencil sequence failed\n");
    }
    if(!mismatch) {
        Nv2aSurface surface={.width=37,.height=19,.bytes_per_pixel=4,.pitch=148,.aa_sx=1,.aa_sy=1};
        Nv2aRenderState state={.color_mask=0x01010101,.depth_min=100,.depth_max=200,
            .depth_test_enable=1,.depth_write=1,.depth_func=0x201};
        Nv2aVertex vertices[]={
            {.x=3.7f,.y=1.9f,.z=125,.rhw=1,.diffuse=0xffff0000},
            {.x=33.3f,.y=1.9f,.z=125,.rhw=2,.diffuse=0xff00ff00},
            {.x=18.5f,.y=17.1f,.z=125,.rhw=4,.diffuse=0xff0000ff}};
        Nv2aBatch batch={.vertices=vertices,.count=3,.state=&state};
        VkResult r=ch_vk_nv2a_clear_read(target,&surface,0xf3,0xff000000,0xffffff00,pixels,sizeof(pixels));
        if(r==VK_SUCCESS) r=ch_vk_nv2a_draw_read(target,&surface,&batch,pixels,sizeof(pixels));
        if(r!=VK_SUCCESS) { fprintf(stderr,"NV2A adapter result %d\n",r); mismatch=1; }
        const int want[]={23,46,185,255};
        unsigned center=(9*37+18)*4;
        for(unsigned c=0;!mismatch && c<4;c++) {
            int delta=pixels[center+c]-want[c];
            if(delta < -1 || delta > 1) { fprintf(stderr,"NV2A perspective channel %u: %u\n",c,pixels[center+c]); mismatch=1; }
        }
        if(pixels[0] || pixels[1] || pixels[2] || pixels[3]!=255) mismatch=1;
        /* A farther opaque triangle must not overwrite the prior depth. */
        for(unsigned i=0;i<3;i++) { vertices[i].z=175; vertices[i].diffuse=0xffffffff; }
        r=ch_vk_nv2a_draw_read(target,&surface,&batch,pixels,sizeof(pixels));
        if(r!=VK_SUCCESS) mismatch=1;
        for(unsigned c=0;!mismatch && c<4;c++) {
            int delta=pixels[center+c]-want[c];
            if(delta < -1 || delta > 1) mismatch=1;
        }
        Nv2aTexture unsupported={0}; batch.texture=&unsupported;
        if(ch_vk_nv2a_draw_read(target,&surface,&batch,pixels,sizeof(pixels))!=VK_ERROR_FEATURE_NOT_PRESENT) mismatch=1;
        if(ch_vk_nv2a_clear_read(target,&surface,0x100,0,0,pixels,sizeof(pixels))!=VK_ERROR_FEATURE_NOT_PRESENT) mismatch=1;
        if(mismatch) fprintf(stderr,"decoded NV2A fixture failed\n");
    }
    if(!mismatch) {
        const ChVkColorVertex tri[]={{{-1,-1,.25f},{1,0,0,.5f}},{{3,-1,.25f},{1,0,0,.5f}},{{-1,3,.25f},{1,0,0,.5f}}};
        const uint16_t ix[]={0,1,2}, reverse[]={0,2,1};
        const float bg[]={0,0,1,1};
        const int passes[]={0,0,1,1,0,0,1,1}; /* alpha rounds to 128 */
        for(unsigned f=0;!mismatch && f<8;f++) {
            Nv2aRenderState state={.color_mask=0x01010101,.alpha_test_enable=1,.alpha_func=0x200+f,.alpha_ref=128};
            VkResult r=ch_vk_target_draw_state_read(target,bg,tri,3,ix,3,&state,pixels,sizeof(pixels));
            if(r!=VK_SUCCESS) mismatch=1;
            for(unsigned i=0;!mismatch && i<sizeof(pixels);i++) {
                int expected=passes[f]?(i%4==0?255:i%4==3?128:0):(i%4>=2?255:0);
                if(pixels[i]!=expected) { fprintf(stderr,"alpha func %x byte %u got %u expected %d\n",0x200+f,i,pixels[i],expected); mismatch=1; }
            }
        }
        for(unsigned c=0;!mismatch && c<6;c++) {
            Nv2aRenderState state={.color_mask=0x01010101,.cull_enable=1,
                .front_face=c==4?0x901:0x900,.cull_face=c==5?0x408:(c==2 || c==3)?0x404:0x405};
            int visible=(c==0 || c==3);
            const uint16_t *indices=(c==1 || c==3)?reverse:ix;
            VkResult r=ch_vk_target_draw_state_read(target,bg,tri,3,indices,3,&state,pixels,sizeof(pixels));
            if(r!=VK_SUCCESS) mismatch=1;
            for(unsigned i=0;!mismatch && i<sizeof(pixels);i++) {
                int expected=visible?(i%4==0?255:i%4==3?128:0):(i%4>=2?255:0);
                if(pixels[i]!=expected) { fprintf(stderr,"cull case %u byte %u got %u expected %d\n",c,i,pixels[i],expected); mismatch=1; }
            }
        }
    }
    if(!mismatch) {
        uint32_t argb[13*7];
        unsigned char readback[sizeof(argb)];
        for(unsigned i=0;i<13*7;i++) argb[i]=((i*37u&255)<<24)|((i*71u&255)<<16)|((i*19u&255)<<8)|(i*113u&255);
        ChVkTexture *texture=NULL;
        VkResult r=ch_vk_texture_upload(gpu,device,queue,family,13,7,argb,&texture);
        if(r!=VK_SUCCESS) { fprintf(stderr,"texture upload: %d\n",r); mismatch=1; }
        for(unsigned pass=0;!mismatch && pass<3;pass++) {
            r=ch_vk_texture_read_rgba(texture,readback,sizeof(readback));
            if(r!=VK_SUCCESS) { mismatch=1; break; }
            for(unsigned i=0;i<13*7;i++) {
                unsigned char expected[]={argb[i]>>16,argb[i]>>8,argb[i],argb[i]>>24};
                for(unsigned c=0;c<4;c++) if(readback[i*4+c]!=expected[c]) {
                    fprintf(stderr,"texture pixel %u channel %u got %u expected %u\n",i,c,readback[i*4+c],expected[c]); mismatch=1;
                }
            }
            VkDescriptorImageInfo d=ch_vk_texture_descriptor(texture);
            if(!d.sampler || !d.imageView || d.imageLayout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) mismatch=1;
        }
        ChVkColorVertex quad[]={
            {{-1,-1,0,1,0,0},{1,1,1,1}},{{1,-1,0,1,1,0},{1,1,1,1}},
            {{1,1,0,1,1,1},{1,1,1,1}},{{-1,1,0,1,0,1},{1,1,1,1}}};
        const uint16_t indices[]={0,1,2,0,2,3};
        const float bg[]={0,0,0,0};
        Nv2aRenderState state={.color_mask=0x01010101};
        for(unsigned pass=0;!mismatch && pass<2;pass++) {
            if(pass) for(unsigned i=0;i<4;i++) { quad[i].rgba[0]=.5f; quad[i].rgba[2]=.25f; }
            r=ch_vk_target_draw_texture_read(target,bg,quad,4,indices,6,&state,texture,pixels,sizeof(pixels));
            if(r!=VK_SUCCESS) { fprintf(stderr,"texture draw %d\n",r); mismatch=1; break; }
            for(unsigned y=0;!mismatch && y<19;y++) for(unsigned x=0;x<37;x++) {
                unsigned tx=(unsigned)((x+.5f)*13/37),ty=(unsigned)((y+.5f)*7/19);
                uint32_t a=argb[ty*13+tx];
                unsigned char expected[]={a>>16,a>>8,a,a>>24};
                for(unsigned c=0;c<4;c++) {
                    float scale=pass?(c==0?.5f:c==2?.25f:1):1;
                    int want=(int)(expected[c]*scale+.5f);
                    int delta=pixels[(y*37+x)*4+c]-want;
                    if(delta < -1 || delta > 1) {
                        fprintf(stderr,"sample pass %u xy %u,%u channel %u got %u expected %d\n",pass,x,y,c,pixels[(y*37+x)*4+c],want);
                        mismatch=1; break;
                    }
                }
            }
        }
        ch_vk_texture_destroy(texture);
    }
    if(!mismatch) {
        const Nv2aSurface s={.width=37,.height=19,.pitch=148,.bytes_per_pixel=4};
        const Nv2aRenderState state={.color_mask=0x01010101};
        Nv2aTexture tx={.width=3,.height=3,.levels=1,.addr_u=3,.addr_v=3,.filter=0x01010000,.color=0x1234};
        const Nv2aVertex v[]={
            {.x=0,.y=0,.rhw=1,.diffuse=0xffffffff,.u=0,.v=0},
            {.x=37,.y=0,.rhw=1,.diffuse=0xffffffff,.u=3,.v=0},
            {.x=37,.y=19,.rhw=1,.diffuse=0xffffffff,.u=3,.v=3},
            {.x=0,.y=0,.rhw=1,.diffuse=0xffffffff,.u=0,.v=0},
            {.x=37,.y=19,.rhw=1,.diffuse=0xffffffff,.u=3,.v=3},
            {.x=0,.y=19,.rhw=1,.diffuse=0xffffffff,.u=0,.v=3}};
        Nv2aBatch batch={.vertices=v,.count=6,.state=&state,.texture=&tx,.stage={&tx}};
        VkResult r=ch_vk_nv2a_clear_read(target,&s,0xf3,0,0xffffff00,pixels,sizeof(pixels));
        if(r==VK_SUCCESS) r=ch_vk_nv2a_draw_decoded_read(target,&s,&batch,fixture_decode,pixels,sizeof(pixels));
        if(r!=VK_SUCCESS) { fprintf(stderr,"textured NV2A result %d\n",r); mismatch=1; }
        for(unsigned y=0;!mismatch && y<19;y++) for(unsigned x=0;x<37;x++) {
            unsigned u=(unsigned)((x+.5f)*3/37),v=(unsigned)((y+.5f)*3/19);
            unsigned char want[]={u*80,v*80,32,255};
            for(unsigned c=0;c<4;c++) if(pixels[(y*37+x)*4+c]!=want[c]) {
                fprintf(stderr,"textured NV2A mismatch xy %u,%u c%u\n",x,y,c); mismatch=1;
            }
        }
        tx.color=0xdead;
        if(ch_vk_nv2a_draw_decoded_read(target,&s,&batch,fixture_decode,pixels,sizeof(pixels))!=VK_ERROR_FORMAT_NOT_SUPPORTED) mismatch=1;
        tx.color=0x1234; tx.levels=2;
        if(ch_vk_nv2a_draw_decoded_read(target,&s,&batch,fixture_decode,pixels,sizeof(pixels))!=VK_SUCCESS) mismatch=1;
        for(unsigned y=0;!mismatch && y<19;y++) for(unsigned x=0;x<37;x++) {
            unsigned u=(unsigned)((x+.5f)*3/37),v=(unsigned)((y+.5f)*3/19);
            unsigned char want[]={u*80,v*80,32,255};
            if(memcmp(pixels+(y*37+x)*4,want,4)) mismatch=1;
        }
        tx.filter=0x01030000; /* Actual mip filter still needs explicit support. */
        if(ch_vk_nv2a_draw_decoded_read(target,&s,&batch,fixture_decode,pixels,sizeof(pixels))!=VK_ERROR_FEATURE_NOT_PRESENT) mismatch=1;
    }
    if(!mismatch) mismatch=ch_test_guest_textures(gpu,device,queue,family);
    if(!mismatch) {
        const Nv2aSurface s={.width=37,.height=19,.pitch=148,.bytes_per_pixel=4};
        /* All 16 channel subsets; depth/stencil clearing independently varies. */
        for(unsigned mask=0;mask<16 && !mismatch;mask++) {
            VkResult r=ch_vk_nv2a_clear_read(target,&s,0xf3,0x11223344,0xffffff00,pixels,sizeof(pixels));
            if(r==VK_SUCCESS) r=ch_vk_nv2a_clear_read(target,&s,(mask<<4)|(mask&3),0xaabbccdd,0x80000071,pixels,sizeof(pixels));
            if(r!=VK_SUCCESS) { fprintf(stderr,"channel clear mask %x result %d\n",mask,r); mismatch=1; break; }
            const unsigned char before[]={0x22,0x33,0x44,0x11},after[]={0xbb,0xcc,0xdd,0xaa};
            for(unsigned i=0;i<sizeof(pixels);i++) {
                unsigned c=i%4;
                unsigned want=(mask & (1u<<c))?after[c]:before[c];
                if(pixels[i]!=want) { fprintf(stderr,"channel clear mask%x byte%u got%u expected%u\n",mask,i,pixels[i],want); mismatch=1; break; }
            }
        }
    }
    if(!mismatch) {
        const Nv2aSurface s={.width=37,.height=19,.pitch=148,.bytes_per_pixel=4};
        Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=1,.depth_write=1,
            .depth_func=0x201,.depth_min=0,.depth_max=1,.stencil_enable=1,
            .stencil_func=0x207,.stencil_ref=7,.stencil_func_mask=255,.stencil_write_mask=255,
            .stencil_fail=0x1e00,.stencil_zfail=0x1e00,.stencil_zpass=0x1e01};
        Nv2aVertex v[]={
            {.x=0,.y=0,.z=.25f,.rhw=1,.diffuse=0xffff0000},
            {.x=74,.y=0,.z=.25f,.rhw=1,.diffuse=0xffff0000},
            {.x=0,.y=38,.z=.25f,.rhw=1,.diffuse=0xffff0000}};
        Nv2aBatch batch={.vertices=v,.count=3,.state=&state};
        VkResult r=ch_vk_nv2a_clear_read(target,&s,0xf3,0xff000000,0xffffff00,pixels,sizeof(pixels));
        if(r==VK_SUCCESS) r=ch_vk_nv2a_draw_read(target,&s,&batch,pixels,sizeof(pixels));
        /* Import must preserve the depth and stencil produced above. */
        if(r==VK_SUCCESS) r=ch_vk_target_import_color(target,pixels,sizeof(pixels));
        if(r==VK_SUCCESS) r=ch_vk_nv2a_clear_read(target,&s,0x20,0xff00ff00,0,pixels,sizeof(pixels));
        state.stencil_func=0x202; state.stencil_zpass=0x1e00;
        for(unsigned i=0;i<3;i++) { v[i].z=.75f; v[i].diffuse=0xff0000ff; }
        if(r==VK_SUCCESS) r=ch_vk_nv2a_draw_read(target,&s,&batch,pixels,sizeof(pixels));
        if(r!=VK_SUCCESS) mismatch=1;
        for(unsigned i=0;!mismatch && i<sizeof(pixels);i++)
            if(pixels[i]!=(i%4==2?0:255)) mismatch=1; /* yellow: red retained, green cleared */
        for(unsigned i=0;i<3;i++) v[i].z=.1f;
        r=ch_vk_nv2a_draw_read(target,&s,&batch,pixels,sizeof(pixels));
        if(r!=VK_SUCCESS) mismatch=1;
        for(unsigned i=0;!mismatch && i<sizeof(pixels);i++)
            if(pixels[i]!=(i%4>=2?255:0)) mismatch=1;
        if(mismatch) fprintf(stderr,"partial clear depth/stencil preservation failed\n");
    }
    if(!mismatch) {
        const Nv2aSurface s={.width=37,.height=19,.pitch=148,.bytes_per_pixel=4};
        Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=1,.depth_write=1,
            .depth_func=0x201,.depth_max=1};
        Nv2aVertex v[]={
            {.x=0,.y=0,.z=.25f,.rhw=1,.diffuse=0xffff0000},
            {.x=74,.y=0,.z=.25f,.rhw=1,.diffuse=0xffff0000},
            {.x=0,.y=38,.z=.25f,.rhw=1,.diffuse=0xffff0000}};
        uint32_t count=11;
        Nv2aBatch batch={.vertices=v,.count=3,.state=&state,.zpass_out=&count};
        VkResult r=ch_vk_target_enable_queries(target);
        if(r==VK_SUCCESS) r=ch_vk_nv2a_clear_read(target,&s,0xf3,0,0xffffff00,pixels,sizeof(pixels));
        if(r==VK_SUCCESS) r=ch_vk_nv2a_draw_read(target,&s,&batch,pixels,sizeof(pixels));
        if(r!=VK_SUCCESS || count!=11+37*19) mismatch=1;
        for(unsigned i=0;i<3;i++) v[i].z=.75f;
        r=ch_vk_nv2a_draw_read(target,&s,&batch,pixels,sizeof(pixels));
        if(r!=VK_SUCCESS || count!=11+37*19) mismatch=1;
        state.depth_test_enable=0; state.alpha_test_enable=1; state.alpha_func=0x200;
        r=ch_vk_nv2a_draw_read(target,&s,&batch,pixels,sizeof(pixels));
        if(r!=VK_SUCCESS || count!=11+37*19) mismatch=1;
        state.alpha_func=0x207;
        r=ch_vk_nv2a_draw_read(target,&s,&batch,pixels,sizeof(pixels));
        if(r!=VK_SUCCESS || count!=11+2*37*19) mismatch=1;
        if(mismatch) fprintf(stderr,"precise visibility count failed: %u\n",count);
    }
    if(!mismatch) {
        uint32_t seed=17;
        ChVkColorVertex vertices[]={{{-1,-1,0,1},{.2f,.4f,.6f,.8f}},{{3,-1,0,1},{.2f,.4f,.6f,.8f}},{{-1,3,0,1},{.2f,.4f,.6f,.8f}}};
        const uint16_t indices[]={0,1,2};
        const Nv2aRenderState state={.color_mask=0x01010101}; const float bg[]={0,0,0,0};
        for(unsigned c=0;c<3;c++) { vertices[c].xy[6]=.7f; vertices[c].xy[7]=.3f; vertices[c].xy[8]=.1f; vertices[c].xy[9]=.9f; vertices[c].xy[10]=.5f; }
        for(unsigned test=0;test<32 && !mismatch;test++) {
            Nv2aCombiner rc={0}; uint32_t words[54];
            for(unsigned i=0;i<54;i++) { seed=seed*1664525u+1013904223u; words[i]=seed; }
            memcpy(&rc,words,sizeof(rc)); rc.control=(test%9)|0x11100; rc.stage_program=0;
            const float diffuse[]={.2f,.4f,.6f,.8f},spec[]={.7f,.3f,.1f,.9f},fog[]={1,0,0,.5f};
            const float texels[4][4]={{0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1}};
            float expected[4]; nv2a_rc_eval(&rc,diffuse,spec,fog,texels,expected);
            VkResult r=ch_vk_target_draw_combiner_read(target,bg,vertices,3,indices,3,&state,NULL,NULL,&rc,0xffff0000,NULL,pixels,sizeof(pixels));
            if(r!=VK_SUCCESS) { fprintf(stderr,"fragment combiner %u result %d\n",test,r); mismatch=1; break; }
            for(unsigned i=0;i<sizeof(pixels);i++) {
                int want=(int)(expected[i%4]*255+.5f),delta=pixels[i]-want;
                if(delta < -1 || delta > 1) { fprintf(stderr,"fragment combiner %u byte%u got%u expected%d\n",test,i,pixels[i],want); mismatch=1; break; }
            }
        }
    }
    if(!mismatch) {
        float depth[37*19],read_depth[37*19]; unsigned char stencil[37*19],read_stencil[37*19];
        unsigned char before[sizeof(pixels)];
        const float bg[]={0,0,1,1};
        VkResult r=ch_vk_target_clear_read(target,bg,before,sizeof(before));
        for(unsigned i=0;i<37*19;i++) { depth[i]=i/1024.f; stencil[i]=i&7; }
        if(r==VK_SUCCESS) r=ch_vk_target_write_depth_stencil(target,depth,stencil,37*19);
        if(r==VK_SUCCESS) r=ch_vk_target_read_depth_stencil(target,read_depth,read_stencil,37*19);
        if(r!=VK_SUCCESS || memcmp(depth,read_depth,sizeof(depth)) || memcmp(stencil,read_stencil,sizeof(stencil))) mismatch=1;
        r=ch_vk_target_clear_aspects_read(target,0,NULL,0,0,pixels,sizeof(pixels));
        if(r!=VK_SUCCESS || memcmp(before,pixels,sizeof(pixels))) mismatch=1;
        const ChVkColorVertex tri[]={{{-1,-1,.5f,1},{1,0,0,1}},{{3,-1,.5f,1},{1,0,0,1}},{{-1,3,.5f,1},{1,0,0,1}}};
        const uint16_t ix[]={0,1,2};
        const Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=1,.depth_func=0x201,
            .stencil_enable=1,.stencil_func=0x202,.stencil_ref=7,.stencil_func_mask=255,
            .stencil_fail=0x1e00,.stencil_zfail=0x1e00,.stencil_zpass=0x1e00};
        r=ch_vk_target_draw_state_read(target,NULL,tri,3,ix,3,&state,pixels,sizeof(pixels));
        if(r!=VK_SUCCESS) mismatch=1;
        for(unsigned i=0;!mismatch && i<37*19;i++) {
            int passes=depth[i]>.5f && stencil[i]==7;
            const unsigned char red[]={255,0,0,255},blue[]={0,0,255,255};
            if(memcmp(pixels+i*4,passes?red:blue,4)) mismatch=1;
        }
        depth[0]=-1;
        if(ch_vk_target_write_depth_stencil(target,depth,stencil,37*19)!=VK_ERROR_INITIALIZATION_FAILED) mismatch=1;
        depth[0]=0;
        r=ch_vk_target_read_depth_stencil(target,read_depth,read_stencil,37*19);
        if(r!=VK_SUCCESS || memcmp(depth,read_depth,sizeof(depth)) || memcmp(stencil,read_stencil,sizeof(stencil))) mismatch=1;
        if(mismatch) fprintf(stderr,"depth/stencil transfer fixture failed\n");
    }
    if(!mismatch) {
        const Nv2aSurface s={.width=37,.height=19,.pitch=148,.bytes_per_pixel=4};
        const unsigned rectangles[][4]={{3,5,17,11},{35,17,50,40},{20,10,5,4}};
        const unsigned flag_cases[]={0xf3,0x21,0x80,1,2,0xf0};
        float depth[37*19]; unsigned char stencil[37*19];
        for(unsigned rect=0;rect<3 && !mismatch;rect++) for(unsigned f=0;f<6 && !mismatch;f++) {
            const unsigned *q=rectangles[rect]; unsigned flags=flag_cases[f];
            Nv2aRenderState rs={.clear_rect_set=3,.clear_rect_h=q[0]|(q[2]<<16),.clear_rect_v=q[1]|(q[3]<<16),
                .blend_enable=1,.blend_eq=0xdead,.cull_enable=1,.front_face=0xdead,.alpha_test_enable=1,.alpha_func=0x200};
            VkResult r=ch_vk_nv2a_clear_read(target,&s,0xf3,0x11223344,0xffffff00,pixels,sizeof(pixels));
            if(r==VK_SUCCESS) r=ch_vk_nv2a_clear_state_read(target,&s,&rs,flags,0xaabbccdd,0x80000071,pixels,sizeof(pixels));
            if(r==VK_SUCCESS) r=ch_vk_target_read_depth_stencil(target,depth,stencil,37*19);
            if(r!=VK_SUCCESS) { fprintf(stderr,"rectangle %u flags%x result%d\n",rect,flags,r); mismatch=1; break; }
            for(unsigned y=0;y<19;y++) for(unsigned x=0;x<37;x++) {
                int inside=x>=q[0] && x<=q[2] && y>=q[1] && y<=q[3];
                unsigned i=y*37+x;
                const unsigned char old[]={0x22,0x33,0x44,0x11},cleared[]={0xbb,0xcc,0xdd,0xaa};
                for(unsigned c=0;c<4;c++) if(pixels[i*4+c]!=(inside && (flags&(0x10u<<c))?cleared[c]:old[c])) mismatch=1;
                float want_depth=inside && (flags&1)?(float)(8388608.0/16777215.0):1;
                if(depth[i]!=want_depth || stencil[i]!=(inside && (flags&2)?0x71:0)) mismatch=1;
            }
            if(mismatch) fprintf(stderr,"rectangle clear mismatch rect%u flags%x\n",rect,flags);
        }
    }
    if(!mismatch) {
        Nv2aSurface s={.width=640,.height=480,.pitch=2560,.bytes_per_pixel=4,.aa_sx=1,.aa_sy=1};
        Nv2aVertex v[]={
            {.x=200,.y=330,.z=.5f,.rhw=.5f,.diffuse=0xffff0000},
            {.x=440,.y=330,.z=.5f,.rhw=.5f,.diffuse=0xffff0000},
            {.x=320,.y=150,.z=.5f,.rhw=.5f,.diffuse=0xffff0000}};
        const Nv2aRenderState state={.color_mask=0x01010101,.depth_test_enable=1,.depth_func=0x207,.depth_write=1};
        Nv2aBatch b={.vertices=v,.count=3,.state=&state};
        ChVkEye eye={.game_projection={320,-240,320,240,1}};
        const float q[]={0,0,0,1},fov[]={-1,1,1,-1},bg[]={0,0,1,1};
        unsigned char saved[sizeof(pixels)];
        for(unsigned side=0;side<2 && !mismatch;side++) {
            float eye_x=side?.3f:-.3f; float position[]={eye_x,0,0};
            vr_eye_matrix(q,position,fov,1,.1f,100,eye.gl_eye_vp);
            VkResult r=ch_vk_target_clear_read(target,bg,pixels,sizeof(pixels));
            if(r==VK_SUCCESS) r=ch_vk_nv2a_draw_eye_read(target,&s,&b,NULL,&eye,pixels,sizeof(pixels));
            if(r!=VK_SUCCESS) { fprintf(stderr,"immersive eye%u result%d\n",side,r); mismatch=1; break; }
            float eye_depth[37*19]; unsigned char eye_stencil[37*19];
            if(ch_vk_target_read_depth_stencil(target,eye_depth,eye_stencil,37*19)!=VK_SUCCESS) mismatch=1;
            /* Independent pinhole projection, positive-height Vulkan viewport. */
            const float wx[]={-.75f,.75f,0},wy[]={-.75f,-.75f,.75f};
            float px[3],py[3];
            for(unsigned i=0;i<3;i++) { px[i]=((wx[i]-eye_x)/2*.5f+.5f)*37; py[i]=(.5f-wy[i]/4)*19; }
            for(unsigned y=0;y<19;y++) for(unsigned x=0;x<37;x++) {
                int pos=0,neg=0,edge=0;
                for(unsigned i=0;i<3;i++) {
                    unsigned j=(i+1)%3;
                    float e=(x+.5f-px[i])*(py[j]-py[i])-(y+.5f-py[i])*(px[j]-px[i]);
                    pos|=e>0; neg|=e<0; edge|=(e>-.02f && e<.02f);
                }
                if(edge) continue;
                int inside=!(pos && neg);
                const unsigned char red[]={255,0,0,255},blue[]={0,0,255,255};
                if(memcmp(pixels+(y*37+x)*4,inside?red:blue,4)) { fprintf(stderr,"eye%u pixel%u,%u\n",side,x,y); mismatch=1; }
                float expected_depth=inside?(100.f/99.9f-.1f*100.f/(99.9f*2)):1;
                if(fabsf(eye_depth[y*37+x]-expected_depth)>0.000001f) mismatch=1;
            }
            memcpy(saved,pixels,sizeof(saved));
            s.width=1280; s.height=960; s.aa_sx=s.aa_sy=2;
            for(unsigned i=0;i<3;i++) { v[i].x*=2; v[i].y*=2; }
            r=ch_vk_target_clear_read(target,bg,pixels,sizeof(pixels));
            if(r==VK_SUCCESS) r=ch_vk_nv2a_draw_eye_read(target,&s,&b,NULL,&eye,pixels,sizeof(pixels));
            if(r!=VK_SUCCESS || memcmp(saved,pixels,sizeof(saved))) mismatch=1;
            s.width=640; s.height=480; s.aa_sx=s.aa_sy=1;
            for(unsigned i=0;i<3;i++) { v[i].x/=2; v[i].y/=2; }
        }
        v[0].rhw=1;
        if(ch_vk_nv2a_draw_eye_read(target,&s,&b,NULL,&eye,pixels,sizeof(pixels))!=VK_ERROR_FEATURE_NOT_PRESENT) mismatch=1;
        if(mismatch) fprintf(stderr,"immersive projection fixture failed\n");
    }
    if(!mismatch) {
        const ChVkColorVertex quad[]={
            {{-1,-1,0,1,-1,-1},{1,1,1,1}},{{1,-1,0,1,1,-1},{1,1,1,1}},
            {{1,1,0,1,1,1},{1,1,1,1}},{{-1,1,0,1,-1,1},{1,1,1,1}}};
        const uint16_t indices[]={0,1,2,0,2,3};
        for(unsigned test=0;test<5 && !mismatch;test++) {
            ChVkEffect effect={.kind=test==3?2:1,.color={.8f,.2f,.1f,.7f},
                .shape={0,0,0,0},.tint={.55f,.75f,.85f,.1f},.reticle={.1f,-.2f,0,0}};
            if(test==1) { effect.shape[0]=.6f; effect.shape[1]=.15f; }
            if(test==2) effect.shape[0]=-1;
            if(test==3) { effect.shape[0]=.15f; effect.shape[1]=.5f; effect.shape[2]=.05f; }
            const float bg[]={test==4?26/255.f:0,test==4?51/255.f:0,test==4?102/255.f:0,test==4?128/255.f:0};
            Nv2aRenderState state={.color_mask=0x01010101,.blend_enable=test==4,
                .blend_src=1,.blend_dst=0x303,.blend_eq=0x8006};
            VkResult r=ch_vk_target_clear_read(target,bg,pixels,sizeof(pixels));
            if(r==VK_SUCCESS) r=ch_vk_target_draw_effect_read(target,quad,4,indices,6,&state,&effect,pixels,sizeof(pixels));
            if(r!=VK_SUCCESS) { fprintf(stderr,"effect test%u result%d\n",test,r); mismatch=1; break; }
            for(unsigned y=0;y<19;y++) for(unsigned x=0;x<37;x++) {
                float u=(x+.5f)/37*2-1,v=(y+.5f)/19*2-1,a,expected[4];
                if(test==3) {
                    float d=hypotf(u-.1f,v+.2f);
                    float dot=1-smooth_reference(.15f*.55f,.15f,d);
                    float ring=1-smooth_reference(.05f*.4f,.05f,fabsf(d-.5f));
                    a=fmaxf(dot,ring)*.7f;
                    float ta=.1f*(1-a);
                    for(unsigned c=0;c<3;c++) expected[c]=effect.color[c]*a+effect.tint[c]*ta;
                    expected[3]=a+ta;
                } else {
                    float d=hypotf(u,v);
                    a=test==2?1:test==1?fminf(1,fmaxf(0,1-fabsf(d-.6f)/.15f)):fminf(1,fmaxf(0,1-d));
                    a=.7f*(test==2?1:a*a);
                    for(unsigned c=0;c<3;c++) expected[c]=effect.color[c]*a;
                    expected[3]=a;
                }
                for(unsigned c=0;c<4;c++) {
                    if(test==4) expected[c]+=bg[c]*(1-a);
                    int want=(int)(fminf(1,expected[c])*255+.5f),delta=pixels[(y*37+x)*4+c]-want;
                    if(delta < -2 || delta > 2) { fprintf(stderr,"effect%u pixel%u,%u channel%u got%u expected%d\n",test,x,y,c,pixels[(y*37+x)*4+c],want); mismatch=1; }
                }
            }
        }
    }
    ch_vk_target_destroy(target);
    if(mismatch) goto done;
    puts("PASS: RGBA8 clears, indexed quad coverage, interpolated triangle, post-draw clear, blend equations/factors/color masks, depth/stencil, persistent batches and selective clears, decoded NV2A adapter, alpha comparisons, face culling, ARGB texture upload/readback and sampled modulation, decoded textured batches, partial-channel clears, precise visibility queries, fragment combiners, guest CPU color imports, depth/stencil transfers, rectangular clears, per-eye projection, overlay effect shading (synthetic fixtures)");
    result=0;
done:
    if(device) { vkDeviceWaitIdle(device); if(fence) vkDestroyFence(device,fence,NULL); if(pool) vkDestroyCommandPool(device,pool,NULL); vkDestroyDevice(device,NULL); }
    if(instance) vkDestroyInstance(instance,NULL);
    free(families);free(physical);
    return result;
}
