/* GPU encoded texture uploads against CPU uploads of known linear pixels. */
#include "vk_texture_cache.h"
#include "../d3d/d3d8_swizzle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int ch_test_command_batches(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family) {
    ChVkTexture *image=NULL,*reference=NULL;int failed=1;
    uint32_t *argb=NULL;unsigned char *raw=NULL,*actual=NULL,*expected=NULL;
    const unsigned dims[][2]={{1,1},{1,8},{8,1},{4,16},{16,4},{8,8},{3,5},{17,9},{512,512}};
    const unsigned formats[]={6,7,0x12,0x1e};unsigned cases=0;
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"texture decode line%d: %s\n",__LINE__,#x);goto done;}}while(0)
    for(unsigned shape=0;shape<9;shape++) for(unsigned format=0;format<4;format++) {
        unsigned w=dims[shape][0],h=dims[shape][1],n=w*h,swizzled=format<2;
        if(swizzled && ((w&(w-1)) || (h&(h-1)))) continue;
        unsigned pitch=(w+3)*4;size_t span=swizzled?n*4:(h-1)*pitch+w*4;
        argb=malloc(n*4);raw=malloc(span);actual=malloc(n*4);expected=malloc(n*4);REQUIRE(argb && raw && actual && expected);
        unsigned levels=1;for(unsigned dim=w>h?w:h;dim>1;dim>>=1) levels++;
        Nv2aTexture source={.width=w,.height=h,.pitch=pitch,.color=formats[format]};
        VkDescriptorImageInfo identity={0};
        for(unsigned epoch=0;epoch<3;epoch++) {
            for(unsigned i=0;i<n;i++) argb[i]=(i*1664525u+epoch*0x17394527u)^0x12345678u;
            memset(raw,0xa5,span);
            if(swizzled) xbox_swizzle_rect(raw,argb,w,h,4);
            else for(unsigned y=0;y<h;y++) memcpy(raw+y*pitch,argb+y*w,w*4);
            if(format&1) for(unsigned i=0;i<n;i++) argb[i]|=0xff000000;
            if(!image) {
                REQUIRE(ch_vk_texture_upload_encoded32(gpu,device,queue,family,VK_NULL_HANDLE,&source,raw,span,levels,1,&image)==VK_SUCCESS);
                identity=ch_vk_texture_descriptor(image);
            } else {
                REQUIRE(ch_vk_texture_replace_encoded32(image,VK_NULL_HANDLE,&source,raw,span,1)==VK_SUCCESS);
                VkDescriptorImageInfo next=ch_vk_texture_descriptor(image);
                REQUIRE(identity.imageView==next.imageView && identity.sampler==next.sampler);
            }
            REQUIRE(ch_vk_texture_upload_mips(gpu,device,queue,family,w,h,argb,levels,&reference)==VK_SUCCESS);
            for(unsigned level=0;level<levels;level++) {
                unsigned lw=w>>level,lh=h>>level;if(!lw)lw=1;if(!lh)lh=1;
                REQUIRE(ch_vk_texture_read_level_rgba(image,level,actual,n*4)==VK_SUCCESS);
                REQUIRE(ch_vk_texture_read_level_rgba(reference,level,expected,n*4)==VK_SUCCESS);
                REQUIRE(!memcmp(actual,expected,lw*lh*4));
            }
            REQUIRE(ch_vk_texture_replace_encoded32(image,VK_NULL_HANDLE,&source,raw,span-1,1)==VK_ERROR_INITIALIZATION_FAILED);
            // Alternate CPU and GPU replacements on the same allocation.
            REQUIRE(ch_vk_texture_replace_argb(image,argb,n,1)==VK_SUCCESS);
            REQUIRE(ch_vk_texture_replace_encoded32(image,VK_NULL_HANDLE,&source,raw,span,1)==VK_SUCCESS);
            REQUIRE(ch_vk_texture_read_rgba(image,actual,n*4)==VK_SUCCESS);
            REQUIRE(ch_vk_texture_read_rgba(reference,expected,n*4)==VK_SUCCESS);
            REQUIRE(!memcmp(actual,expected,n*4));
            ch_vk_texture_destroy(reference);reference=NULL;cases++;
        }
        source.cube=1;REQUIRE(!ch_vk_texture_can_decode32(&source));source.cube=0;
        source.color=0xb;REQUIRE(!ch_vk_texture_can_decode32(&source));
        ch_vk_texture_destroy(image);image=NULL;
        free(argb);argb=NULL;free(raw);raw=NULL;free(actual);actual=NULL;free(expected);expected=NULL;
    }
    printf("PASS: %u GPU texture decode cases, ARGB/XRGB, rectangular Morton/pitched/odd sizes, exact mips, queued replacement and bounds\n",cases);failed=0;
done:
    ch_vk_texture_destroy(reference);ch_vk_texture_destroy(image);free(argb);free(raw);free(actual);free(expected);return failed;
}
