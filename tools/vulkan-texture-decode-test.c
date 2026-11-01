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
    const unsigned formats[]={6,7,0x12,0x1e,0x3a,0x3b,0x3c,0x3f,0x40,0x41};unsigned cases=0;
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"texture decode line%d: %s\n",__LINE__,#x);goto done;}}while(0)
    for(unsigned shape=0;shape<9;shape++) for(unsigned format=0;format<sizeof(formats)/sizeof(formats[0]);format++) {
        unsigned w=dims[shape][0],h=dims[shape][1],n=w*h,swizzled=format<2 || (format>=4 && format<=6);
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
            for(unsigned i=0;i<n;i++) {
                uint32_t v=argb[i];
                if(format==1 || format==3) argb[i]|=0xff000000;
                // Independent byte-lane oracle, before the ordinary ARGB upload.
                if(format>=4) {
                    unsigned order=(format-4)%3;
                    unsigned char b[4];memcpy(b,&v,4);
                    if(order==0) argb[i]=((uint32_t)b[3]<<24)|((uint32_t)b[0]<<16)|((uint32_t)b[1]<<8)|b[2];
                    if(order==1) argb[i]=((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3];
                    if(order==2) argb[i]=((uint32_t)b[0]<<24)|((uint32_t)b[3]<<16)|((uint32_t)b[2]<<8)|b[1];
                }
            }
            if(!image) {
                REQUIRE(ch_vk_texture_upload_encoded(gpu,device,queue,family,VK_NULL_HANDLE,&source,raw,span,levels,1,&image)==VK_SUCCESS);
                identity=ch_vk_texture_descriptor(image);
            } else {
                REQUIRE(ch_vk_texture_replace_encoded(image,VK_NULL_HANDLE,&source,raw,span,1)==VK_SUCCESS);
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
            REQUIRE(ch_vk_texture_replace_encoded(image,VK_NULL_HANDLE,&source,raw,span-1,1)==VK_ERROR_INITIALIZATION_FAILED);
            // Alternate CPU and GPU replacements on the same allocation.
            REQUIRE(ch_vk_texture_replace_argb(image,argb,n,1)==VK_SUCCESS);
            REQUIRE(ch_vk_texture_replace_encoded(image,VK_NULL_HANDLE,&source,raw,span,1)==VK_SUCCESS);
            REQUIRE(ch_vk_texture_read_rgba(image,actual,n*4)==VK_SUCCESS);
            REQUIRE(ch_vk_texture_read_rgba(reference,expected,n*4)==VK_SUCCESS);
            REQUIRE(!memcmp(actual,expected,n*4));
            ch_vk_texture_destroy(reference);reference=NULL;cases++;
        }
        source.cube=1;REQUIRE(!ch_vk_texture_can_decode(&source));source.cube=0;
        source.color=0xb;REQUIRE(!ch_vk_texture_can_decode(&source));
        ch_vk_texture_destroy(image);image=NULL;
        free(argb);argb=NULL;free(raw);raw=NULL;free(actual);actual=NULL;free(expected);expected=NULL;
    }
    unsigned compressed_cases=0;
    const unsigned compressed[]={0x0c,0x0e,0x0f};
    for(unsigned shape=0;shape<9;shape++) for(unsigned f=0;f<3;f++) {
        unsigned w=dims[shape][0],h=dims[shape][1],n=w*h,fmt=compressed[f];
        size_t span=((w+3)/4)*((h+3)/4)*d3d8_format_dxt_block_bytes(fmt);
        argb=malloc(n*4);raw=malloc(span);actual=malloc(n*4);expected=malloc(n*4);
        REQUIRE(argb && raw && actual && expected);
        unsigned levels=1;for(unsigned dim=w>h?w:h;dim>1;dim>>=1) levels++;
        Nv2aTexture source={.width=w,.height=h,.color=fmt};
        for(unsigned epoch=0;epoch<4;epoch++) {
            uint32_t seed=12345+epoch;
            for(size_t i=0;i<span;i++) {seed=seed*1664525u+1013904223u;raw[i]=seed>>24;}
            // Explicit endpoint ties and both DXT5 alpha interpolation regimes.
            unsigned block=d3d8_format_dxt_block_bytes(fmt);
            for(size_t b=0;b<span;b+=block) {
                unsigned color=b+(block==16?8:0);
                if(epoch==0) {raw[color]=raw[color+2]=0;raw[color+1]=raw[color+3]=0;}
                if(fmt==0x0f) {raw[b]=epoch&1?255:0;raw[b+1]=epoch&1?0:255;}
            }
            for(unsigned y=0;y<h;y++) for(unsigned x=0;x<w;x++)
                REQUIRE(d3d8_dxt_decode_texel(raw,fmt,x,y,w,&argb[y*w+x]));
            if(!image) REQUIRE(ch_vk_texture_upload_encoded(gpu,device,queue,family,VK_NULL_HANDLE,&source,raw,span,levels,1,&image)==VK_SUCCESS);
            else REQUIRE(ch_vk_texture_replace_encoded(image,VK_NULL_HANDLE,&source,raw,span,1)==VK_SUCCESS);
            REQUIRE(ch_vk_texture_upload_mips(gpu,device,queue,family,w,h,argb,levels,&reference)==VK_SUCCESS);
            for(unsigned level=0;level<levels;level++) {
                unsigned lw=w>>level,lh=h>>level;if(!lw)lw=1;if(!lh)lh=1;
                REQUIRE(ch_vk_texture_read_level_rgba(image,level,actual,n*4)==VK_SUCCESS);
                REQUIRE(ch_vk_texture_read_level_rgba(reference,level,expected,n*4)==VK_SUCCESS);
                REQUIRE(!memcmp(actual,expected,lw*lh*4));
            }
            REQUIRE(ch_vk_texture_replace_encoded(image,VK_NULL_HANDLE,&source,raw,span-1,1)==VK_ERROR_INITIALIZATION_FAILED);
            ch_vk_texture_destroy(reference);reference=NULL;compressed_cases++;
        }
        ch_vk_texture_destroy(image);image=NULL;
        free(argb);argb=NULL;free(raw);raw=NULL;free(actual);actual=NULL;free(expected);expected=NULL;
    }
    printf("PASS: %u GPU DXT1/3/5 cases against CPU decoder, exact mips and partial blocks\n",compressed_cases);
    printf("PASS: %u GPU texture decode cases, all ten supported 32-bit formats, rectangular Morton/pitched/odd sizes, exact mips, queued replacement and bounds\n",cases);failed=0;
done:
    ch_vk_texture_destroy(reference);ch_vk_texture_destroy(image);free(argb);free(raw);free(actual);free(expected);return failed;
}
