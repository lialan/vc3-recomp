#include "vk_nv2a.h"
#include "../nv2a_texture_cube.h"
#include "../nv2a_stage_fetch.h"
#include "vk_guest_layout.h"
#include "../nv2a_combiner_simple.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include "../d3d/d3d8_swizzle.h"
static uint64_t profile_us(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000u+t.tv_nsec/1000;
}
static void profile_draw(uint64_t start,uint64_t vertices,uint64_t textures,uint64_t raster,int reduced) {
    static uint64_t first,n,fast,vertex_us,texture_us,raster_us;
    static int detail=-1;
    if(detail<0) {const char *e=getenv("CHIHIRO_VULKAN_FRAME_COSTS");detail=e && atoi(e)!=0;}
    if(detail && raster-start>=1000) fprintf(stderr,"[Vulkan slow draw] geometry_ms=%.3f textures_ms=%.3f raster_ms=%.3f\n",
        (vertices-start)/1000.,(textures-vertices)/1000.,(raster-textures)/1000.);
    if(!first) first=start;
    n++;fast+=!!reduced;vertex_us+=vertices-start;texture_us+=textures-vertices;raster_us+=raster-textures;
    if(raster-first<1000000) return;
    fprintf(stderr,"[Vulkan draw] %llu batches (%llu reduced): geometry %.1fms textures %.1fms raster %.1fms\n",
        (unsigned long long)n,(unsigned long long)fast,vertex_us/1000.,texture_us/1000.,raster_us/1000.);
    first=raster;n=fast=vertex_us=texture_us=raster_us=0;
}
static void profile_rc(const Nv2aCombiner *rc) {
    static Nv2aCombiner seen[24];static unsigned count;
    if(!rc) return;
    for(unsigned i=0;i<count;i++) if(!memcmp(&seen[i],rc,sizeof *rc)) return;
    if(count==24) return;
    seen[count++]=*rc;
    fprintf(stderr,"[Vulkan RC] control=%08x stage=%08x final=%08x/%08x\n",rc->control,rc->stage_program,rc->final0,rc->final1);
    for(unsigned i=0;i<(rc->control&255) && i<8;i++)
        fprintf(stderr,"[Vulkan RC] stage%u color=%08x/%08x alpha=%08x/%08x\n",i,rc->color_icw[i],rc->color_ocw[i],rc->alpha_icw[i],rc->alpha_ocw[i]);
}
static int matches(ChVkTarget *t, const Nv2aSurface *s) {
    uint32_t w,h;
    ch_vk_target_size(t,&w,&h);
    return s && w && h && w==s->width && h==s->height;
}
VkResult ch_vk_nv2a_clear_read(ChVkTarget *t, const Nv2aSurface *s,
    uint32_t flags, uint32_t argb, uint32_t zstencil, void *pixels, VkDeviceSize bytes) {
    if(!matches(t,s)) return VK_ERROR_INITIALIZATION_FAILED;
    if(flags & ~0xf3u) return VK_ERROR_FEATURE_NOT_PRESENT;
    const float color[]={((argb>>16)&255)/255.f,((argb>>8)&255)/255.f,
                         (argb&255)/255.f,(argb>>24)/255.f};
    int partial=(flags & 0xf0) && (flags & 0xf0)!=0xf0;
    uint32_t host_flags=((flags & 0xf0) && !partial?1:0)|((flags & 1)?2:0)|((flags & 2)?4:0);
    /* A color-channel-only clear is already implemented by the masked draw
     * below. Do not force a read/submit of unchanged aspects before it. */
    VkResult result=partial && !host_flags?VK_SUCCESS:ch_vk_target_clear_aspects_read(t,host_flags,color,
        (float)((double)(zstencil>>8)/16777215.0),zstencil&255,pixels,bytes);
    if(result!=VK_SUCCESS || !partial) return result;
    /* A masked full-screen draw implements channel clears without affecting
     * depth/stencil. Clear semantics ignore guest blend/test/cull state. */
    ChVkColorVertex quad[4]={{{-1,-1,0,1},{0}},{{1,-1,0,1},{0}},
                            {{1,1,0,1},{0}},{{-1,1,0,1},{0}}};
    for(unsigned i=0;i<4;i++) for(unsigned c=0;c<4;c++) quad[i].rgba[c]=color[c];
    const uint16_t indices[]={0,1,2,0,2,3};
    Nv2aRenderState state={.color_mask=((flags & 0x10)?0x10000:0)|
        ((flags & 0x20)?0x100:0)|((flags & 0x40)?1:0)|((flags & 0x80)?0x1000000:0)};
    return ch_vk_target_draw_state_read(t,NULL,quad,4,indices,6,&state,pixels,bytes);
}
int ch_vk_decode_texture_faces(const Nv2aTexture *tx,ChVkDecodeTexture decode,uint32_t *argb) {
    if(!tx || !decode || !argb) return 0;
    if(!tx->cube) return decode(tx,argb);
    size_t stride=ch_nv2a_cube_face_bytes(tx),pixels=(size_t)tx->width*tx->height;
    if(!stride || (uint64_t)tx->offset+stride*6>UINT64_C(0x100000000)) return 0;
    for(unsigned f=0;f<6;f++) {
        Nv2aTexture face=*tx;face.cube=0;face.levels=1;face.offset+=(uint32_t)(stride*f);
        if(!decode(&face,argb+pixels*f)) return 0;
    }
    return 1;
}
static VkResult sample_image(ChVkTarget *target,const Nv2aTexture *tx,ChVkDecodeTexture decode,
    ChVkSampleResolver resolve,void *context,ChVkTexture **out,ChVkSampleImage *lease) {
    ChVkSampleImage image={0};
    VkResult result=resolve?resolve(context,tx,&image):VK_NOT_READY;
    *lease=image;
    if(result==VK_SUCCESS && image.gpu) {
        if(image.argb || !image.width || !image.height || ch_vk_texture_is_cube(image.gpu)!=!!tx->cube)
            return VK_ERROR_INITIALIZATION_FAILED;
        *out=image.gpu;return VK_SUCCESS;
    }
    /* Existing CPU resolvers expose a single plane. Cube faces retain the
     * reference guest-memory decoder unless the resolver supplies a GPU cube. */
    if(result==VK_SUCCESS && tx->cube) result=VK_NOT_READY;
    if(result==VK_SUCCESS) {
        if(!image.argb || !image.width || !image.height || (uint64_t)image.width*image.height>SIZE_MAX/4)
            return VK_ERROR_INITIALIZATION_FAILED;
        Nv2aTexture actual=*tx;actual.width=image.width;actual.height=image.height;
        return ch_vk_target_upload_texture_mips(target,image.width,image.height,image.argb,
            ch_nv2a_texture_mip_levels(&actual,1),out);
    }
    if(result!=VK_NOT_READY) return result;
    if(!decode) return VK_ERROR_FEATURE_NOT_PRESENT;
    if((uint64_t)tx->width*tx->height>SIZE_MAX/4/(tx->cube?6:1)) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t *argb=malloc((size_t)tx->width*tx->height*4*(tx->cube?6:1));
    if(!argb) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if(!ch_vk_decode_texture_faces(tx,decode,argb)) { free(argb); return VK_ERROR_FORMAT_NOT_SUPPORTED; }
    if(tx->cube) result=ch_vk_target_upload_cube(target,tx->width,argb,out);
    else result=ch_vk_target_upload_texture_mips(target,tx->width,tx->height,argb,
        ch_nv2a_texture_mip_levels(tx,1),out);
    free(argb); return result;
}
static VkResult draw_decoded(ChVkTarget *t, const Nv2aSurface *s,
    const Nv2aBatch *b, ChVkDecodeTexture decode, const ChVkEye *eye, int panel, ChVkSampleResolver resolve, void *context, void *pixels, VkDeviceSize bytes,const ChVkFixedGeometry *fixed) {
    if(!s || !s->width || !s->height || (!eye && !panel && !matches(t,s)) || !b || !b->state || (!fixed && (!b->vertices || !b->count || b->count%3)))
        return VK_ERROR_INITIALIZATION_FAILED;
    if(eye) {
        const VrGameProj *g=&eye->game_projection;
        if(!t || !isfinite(g->kx) || !isfinite(g->ky) || !isfinite(g->cx) || !isfinite(g->cy) ||
           !isfinite(g->inv_pz) || g->kx==0 || g->ky==0 || g->inv_pz==0 ||
           s->aa_sx<1 || s->aa_sx>2 || s->aa_sy<1 || s->aa_sy>2)
            return VK_ERROR_INITIALIZATION_FAILED;
        for(unsigned i=0;i<16;i++) if(!isfinite(eye->gl_eye_vp[i])) return VK_ERROR_INITIALIZATION_FAILED;
        if(b->vertices[0].rhw==1 || b->vertices[0].rhw<=0) return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if(!fixed && (b->raw || (b->ext && !b->rc) || (b->rc && !b->ext)))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    for(unsigned i=0;i<4;i++) if(!b->rc && b->stage[i] && (i!=0 || b->stage[i]!=b->texture)) return VK_ERROR_FEATURE_NOT_PRESENT;
    const char *force=getenv("CHIHIRO_VULKAN_FORCE_COMBINER");
    int modulate=!fixed && b->ext && b->stage[0] && !b->stage[0]->cube && (!force || !atoi(force))?
        ch_nv2a_combiner_modulate(b->rc):0;
    /* Division before interpolation is equivalent only for a constant q.
     * Varying projective q must retain the general fragment-stage divide. */
    if(modulate) for(uint32_t i=0;i<b->count;i++)
        if(!isfinite(b->ext[i].tex[0][3]) || b->ext[i].tex[0][3]!=b->ext[0].tex[0][3]) {modulate=0;break;}
    const Nv2aCombiner *rc=modulate?NULL:b->rc;
    const Nv2aTexture *plain=modulate?b->stage[0]:b->texture;
    VkSamplerCreateInfo sampler;
    if(plain && !rc) {
        const Nv2aTexture *tx=plain;
        if((!decode && !resolve) || ch_vk_sampler_state(tx,&sampler)!=VK_SUCCESS)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        if(!tx->width || !tx->height || (uint64_t)tx->width*tx->height>SIZE_MAX/4)
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    /* The initial indexed path is uint16. Do not truncate or draw a prefix. */
    if(!fixed && b->count>65535) return VK_ERROR_FEATURE_NOT_PRESENT;
    const char *profile_env=getenv("CHIHIRO_VULKAN_PROFILE");
    int profile=profile_env && atoi(profile_env)!=0;
    uint64_t start=profile?profile_us():0;
    if(profile) profile_rc(b->rc);
    ChVkColorVertex *v=fixed?NULL:calloc(b->count,sizeof(*v));
    uint16_t *indices=fixed?NULL:malloc((size_t)b->count*sizeof(*indices));
    if(!fixed && (!v || !indices)) { free(v); free(indices); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    float low=b->state->depth_min, high=b->state->depth_max;
    if(!(high>low)) { low=0; high=16777215.f; }
    ChVkTexture *stage_images[4]={0};
    ChVkSampleImage leases[4]={0},plain_lease={0};
    ChVkTexture *texture=NULL;
    VkResult result=VK_ERROR_INITIALIZATION_FAILED;
    if(!isfinite(low) || !isfinite(high)) goto done;
    uint32_t coordinate_masks[4]={0};
    if(rc) for(unsigned j=0;j<4;j++) coordinate_masks[j]=ch_nv_stage_coordinate_mask(
        (rc->stage_program>>(j*5))&31,b->stage[j]!=NULL,b->stage[j] && b->stage[j]->cube);
    for(uint32_t i=0;!fixed && i<b->count;i++) {
        const Nv2aVertex *g=&b->vertices[i];
        if(!isfinite(g->x) || !isfinite(g->y) || !isfinite(g->z) || !isfinite(g->rhw)) {
            fprintf(stderr,"[Vulkan] invalid guest vertex %u: position=%a/%a/%a rhw=%a\n",i,g->x,g->y,g->z,g->rhw);
            goto done;
        }
        float w=g->rhw>0 && g->rhw<1e30f?1/g->rhw:1;
        v[i].xy[0]=(g->x/s->width*2-1)*w;
        v[i].xy[1]=(g->y/s->height*2-1)*w;
        /* Vulkan depth is [0,1], reference GL remaps this to [-1,1]. */
        v[i].xy[2]=fminf(1,fmaxf(0,(g->z-low)/(high-low)))*w;
        v[i].xy[3]=w;
        if(eye) {
            if(g->rhw<=0) goto done;
            float position[3],clip[4];
            vr_unproject_gl(&eye->game_projection,g->x/s->aa_sx,g->y/s->aa_sy,w,position);
            for(unsigned k=0;k<4;k++) clip[k]=eye->gl_eye_vp[k]*position[0]+eye->gl_eye_vp[4+k]*position[1]+eye->gl_eye_vp[8+k]*position[2]+eye->gl_eye_vp[12+k];
            if(clip[3]==0) goto done; /* low-level legacy vertex encoding reserves w=0 */
            v[i].xy[0]=clip[0]; v[i].xy[1]=-clip[1];
            v[i].xy[2]=(clip[2]+clip[3])*.5f; v[i].xy[3]=clip[3];
        }
        if(plain && !rc) {
            /* Nv2aVertex.u/v are texels for this path for all format classes. */
            if(modulate) {
                const float *tc=b->ext[i].tex[0];
                float q=tc[3]==0?1:tc[3];
                int norm=d3d8_format_is_swizzled(plain->color) || d3d8_format_dxt_block_bytes(plain->color);
                v[i].xy[4]=tc[0]/q/(norm?1:plain->width);
                v[i].xy[5]=tc[1]/q/(norm?1:plain->height);
            } else {
                v[i].xy[4]=g->u/plain->width;
                v[i].xy[5]=g->v/plain->height;
            }
        }
        for(unsigned k=0;k<4;k++) if(!isfinite(v[i].xy[k])) {
            fprintf(stderr,"[Vulkan] invalid clip vertex %u component %u: value=%a\n",i,k,v[i].xy[k]);
            goto done;
        }
        v[i].rgba[0]=((g->diffuse>>16)&255)/255.f;
        v[i].rgba[1]=((g->diffuse>>8)&255)/255.f;
        v[i].rgba[2]=(g->diffuse&255)/255.f;
        v[i].rgba[3]=modulate==2?1:(g->diffuse>>24)/255.f;
        if(rc) {
            const Nv2aVertexEx *ex=&b->ext[i];
            v[i].xy[6]=((ex->specular>>16)&255)/255.f;
            v[i].xy[7]=((ex->specular>>8)&255)/255.f;
            v[i].xy[8]=(ex->specular&255)/255.f;
            v[i].xy[9]=(ex->specular>>24)/255.f;
            v[i].xy[10]=ex->fog;
            for(unsigned j=0;j<4;j++) {
                /* The reference passes consumed shader coordinates through,
                 * including NaN/Inf; these are not structural draw errors.
                 * Constant/unused components cannot affect output. */
                for(unsigned k=0;k<4;k++) v[i].xy[11+j*4+k]=(coordinate_masks[j]&(1u<<k))?ex->tex[j][k]:0;
            }
            for(unsigned k=6;k<11;k++) if(!isfinite(v[i].xy[k])) {
                fprintf(stderr,"[Vulkan] invalid combiner vertex %u component %u: value=%a stage_program=%08X\n",
                    i,k,v[i].xy[k],rc->stage_program);
                goto done;
            }
        }
        indices[i]=(uint16_t)i;
    }
    uint64_t vertices_done=profile?profile_us():0;
    if(plain && !rc) {
        const Nv2aTexture *tx=plain;
        result=sample_image(t,tx,decode,resolve,context,&texture,&plain_lease);
        if(result!=VK_SUCCESS) goto done;
        result=plain_lease.gpu?VK_SUCCESS:ch_vk_texture_set_sampler(texture,&sampler);
        if(result!=VK_SUCCESS) goto done;
    }
    /* Combiner descriptors are independently copied: stage[] is authoritative
     * rather than the convenience texture pointer's identity. */
    ChVkFixedParams params;
    if(fixed) {
        params=fixed->params;params.surface[0]=s->width;params.surface[1]=s->height;
        params.depth[0]=low;params.depth[1]=high;
        params.texture[0]=plain?plain->width:1;params.texture[1]=plain?plain->height:1;
    }
    if(rc) for(unsigned st=0;st<4;st++) {
        uint32_t mode=(rc->stage_program>>(st*5))&31;
        const Nv2aTexture *tx=b->stage[st];
        if(mode<1 || mode>3 || !tx) continue;
        VkSamplerCreateInfo sm;
        if((!decode && !resolve) || ch_vk_sampler_state(tx,&sm)!=VK_SUCCESS) { result=VK_ERROR_FEATURE_NOT_PRESENT; goto done; }
        if(!tx->width || !tx->height || (uint64_t)tx->width*tx->height>SIZE_MAX/4) { result=VK_ERROR_INITIALIZATION_FAILED; goto done; }
        result=sample_image(t,tx,decode,resolve,context,&stage_images[st],&leases[st]);
        if(result!=VK_SUCCESS) goto done;
        result=leases[st].gpu?VK_SUCCESS:ch_vk_texture_set_sampler(stage_images[st],&sm); if(result!=VK_SUCCESS) goto done;
        if(!tx->cube && !d3d8_format_is_swizzled(tx->color) && !d3d8_format_dxt_block_bytes(tx->color)) {
            if(fixed) {params.stage_scale[st][0]=1.f/tx->width;params.stage_scale[st][1]=1.f/tx->height;}
            else for(uint32_t i=0;i<b->count;i++) { v[i].xy[11+st*4]/=tx->width; v[i].xy[12+st*4]/=tx->height; }
        }
    }
    uint64_t textures_done=profile?profile_us():0;
    uint32_t samples=0;
    if(fixed) result=ch_vk_target_draw_fixed_read(t,&params,fixed->triangle_strip,fixed->vertices,fixed->vertex_count,
        fixed->indices,fixed->index_count,b->state,texture,b->zpass_out?&samples:NULL,rc,b->fog_color,
        (const ChVkTexture *const *)stage_images,pixels,bytes);
    else if(panel) result=ch_vk_target_draw_panel_read(t,v,b->count,indices,b->count,b->state,texture,
        rc,b->fog_color,(const ChVkTexture *const *)stage_images,panel==2,pixels,bytes);
    else result=ch_vk_target_draw_combiner_read(t,NULL,v,b->count,indices,b->count,b->state,texture,
        b->zpass_out?&samples:NULL,rc,b->fog_color,(const ChVkTexture *const *)stage_images,pixels,bytes);
    if(profile) profile_draw(start,vertices_done,textures_done,profile_us(),modulate);
    if(result==VK_SUCCESS && b->zpass_out) *b->zpass_out+=samples;
done:;
    /* Cache pins and owned uploads must outlive all recorded draws. A resolver
     * exposing a bare borrowed GPU image keeps the old synchronous lifetime. */
    int bare=plain_lease.gpu && !plain_lease.release;
    for(unsigned i=0;i<4;i++) bare|=leases[i].gpu && !leases[i].release;
    if(bare && ch_vk_target_pending(t)) {
        VkResult r=ch_vk_target_finish(t);if(result==VK_SUCCESS) result=r;
    }
    for(unsigned i=0;i<4;i++) {
        ChVkTexture *image=leases[i].gpu?leases[i].gpu:stage_images[i];
        VkResult r=ch_vk_target_retire_sample(t,image,leases[i].release,leases[i].release_context,!leases[i].gpu);
        if(result==VK_SUCCESS) result=r;
    }
    VkResult retired=ch_vk_target_retire_sample(t,plain_lease.gpu?plain_lease.gpu:texture,
        plain_lease.release,plain_lease.release_context,!plain_lease.gpu);
    if(result==VK_SUCCESS) result=retired;
    free(v); free(indices); return result;
}

VkResult ch_vk_nv2a_draw_resolved_read(ChVkTarget *t,const Nv2aSurface *s,const Nv2aBatch *b,
    ChVkDecodeTexture decode,ChVkSampleResolver resolve,void *context,void *pixels,VkDeviceSize bytes) {
    return draw_decoded(t,s,b,decode,NULL,0,resolve,context,pixels,bytes,NULL);
}

VkResult ch_vk_nv2a_draw_read(ChVkTarget *t, const Nv2aSurface *s,
    const Nv2aBatch *b, void *pixels, VkDeviceSize bytes) {
    return ch_vk_nv2a_draw_decoded_read(t,s,b,NULL,pixels,bytes);
}

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
/* The byte swap is its own inverse. Only disjoint planes enter the restricted
 * loop; aliased loads retain the original scalar access order. memcpy keeps
 * unaligned guest bases/rows valid. */
static int fast_color_io(void) {
    const char *v=getenv("CHIHIRO_VULKAN_COLOR_FAST_IO");
    if(!v) v=getenv("VC3_VULKAN_COLOR_FAST_IO");
    return !v || strcmp(v,"0");
}
static int separate_color_rows(const void *src,size_t src_pitch,const void *dst,size_t dst_pitch,uint32_t w,uint32_t h) {
    uintptr_t a=(uintptr_t)src,b=(uintptr_t)dst;
    size_t aspan=(size_t)(h-1)*src_pitch+(size_t)w*4,bspan=(size_t)(h-1)*dst_pitch+(size_t)w*4;
    return a<=b?b-a>=aspan:a-b>=bspan;
}
static void swap_color_rows(const unsigned char *restrict src,size_t src_pitch,unsigned char *restrict dst,size_t dst_pitch,
    uint32_t width,uint32_t height) {
    for(uint32_t y=0;y<height;y++) for(uint32_t x=0;x<width;x++) {
        /* Bounds are checked before entering. Bypass Android's fortified
         * wrapper here so these constant-size unaligned copies can vectorize. */
        uint32_t pixel;__builtin_memcpy(&pixel,src+(size_t)y*src_pitch+(size_t)x*4,4);
        pixel=(pixel&0xff00ff00u)|((pixel&255)<<16)|((pixel>>16)&255);
        __builtin_memcpy(dst+(size_t)y*dst_pitch+(size_t)x*4,&pixel,4);
    }
}
#endif
VkResult ch_vk_nv2a_store_color_state(const Nv2aSurface *s,const Nv2aRenderState *rs,const void *rgba,
    VkDeviceSize rgba_bytes,void *guest_base,size_t guest_bytes) {
    if(!s || !rgba || !guest_base || !s->width || !s->height)
        return VK_ERROR_INITIALIZATION_FAILED;
    if(s->bytes_per_pixel!=2 && s->bytes_per_pixel!=4) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    ChVkGuestLayout layout;
    VkResult result=ch_vk_guest_layout(rs?rs->surface_format:0,s->width,s->height,s->pitch,s->bytes_per_pixel,&layout);
    if(result!=VK_SUCCESS) return result;
    if((uint64_t)s->width*s->height>rgba_bytes/4 ||
       s->color_va>guest_bytes || layout.span>guest_bytes-s->color_va)
        return VK_ERROR_INITIALIZATION_FAILED;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if(s->bytes_per_pixel==4 && !layout.swizzled && fast_color_io() &&
       separate_color_rows(rgba,(size_t)s->width*4,(unsigned char *)guest_base+s->color_va,layout.pitch,s->width,s->height)) {
        swap_color_rows(rgba,(size_t)s->width*4,(unsigned char *)guest_base+s->color_va,
            layout.pitch,s->width,s->height);return VK_SUCCESS;
    }
#endif
    const unsigned char *src=rgba;
    volatile unsigned char *dst=(volatile unsigned char *)guest_base+s->color_va;
    for(uint32_t y=0;y<s->height;y++) for(uint32_t x=0;x<s->width;x++) {
        const unsigned char *p=src+((size_t)y*s->width+x)*4;
        uint32_t pixel=s->bytes_per_pixel==4?
            ((uint32_t)p[3]<<24)|((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2]:
            ((uint32_t)(p[0]>>3)<<11)|((uint32_t)(p[1]>>2)<<5)|(p[2]>>3);
        for(uint32_t b=0;b<s->bytes_per_pixel;b++)
            dst[(size_t)ch_vk_guest_offset(&layout,x,y)+b]=(unsigned char)(pixel>>(b*8));
    }
    return VK_SUCCESS;
}

VkResult ch_vk_nv2a_load_color_state(const Nv2aSurface *s,const Nv2aRenderState *rs,void *rgba,
    VkDeviceSize rgba_bytes,const void *guest_base,size_t guest_bytes) {
    if(!s || !rgba || !guest_base || !s->width || !s->height) return VK_ERROR_INITIALIZATION_FAILED;
    if(s->bytes_per_pixel!=2 && s->bytes_per_pixel!=4) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    ChVkGuestLayout layout;
    VkResult result=ch_vk_guest_layout(rs?rs->surface_format:0,s->width,s->height,s->pitch,s->bytes_per_pixel,&layout);
    if(result!=VK_SUCCESS) return result;
    if((uint64_t)s->width*s->height>rgba_bytes/4 ||
       s->color_va>guest_bytes || layout.span>guest_bytes-s->color_va) return VK_ERROR_INITIALIZATION_FAILED;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if(s->bytes_per_pixel==4 && !layout.swizzled && fast_color_io() &&
       separate_color_rows((const unsigned char *)guest_base+s->color_va,layout.pitch,rgba,(size_t)s->width*4,s->width,s->height)) {
        swap_color_rows((const unsigned char *)guest_base+s->color_va,layout.pitch,rgba,
            (size_t)s->width*4,s->width,s->height);return VK_SUCCESS;
    }
#endif
    const volatile unsigned char *src=(const volatile unsigned char *)guest_base+s->color_va;
    unsigned char *out=rgba;
    for(uint32_t y=0;y<s->height;y++) for(uint32_t x=0;x<s->width;x++) {
        uint32_t pixel=0;
        for(uint32_t b=0;b<s->bytes_per_pixel;b++) pixel|=(uint32_t)src[(size_t)ch_vk_guest_offset(&layout,x,y)+b]<<(b*8);
        unsigned char *p=out+((size_t)y*s->width+x)*4;
        if(s->bytes_per_pixel==4) { p[0]=pixel>>16; p[1]=pixel>>8; p[2]=pixel; p[3]=pixel>>24; }
        else { p[0]=((pixel>>11)&31)*255/31; p[1]=((pixel>>5)&63)*255/63; p[2]=(pixel&31)*255/31; p[3]=255; }
    }
    return VK_SUCCESS;
}

VkResult ch_vk_nv2a_store_color(const Nv2aSurface *s,const void *rgba,
    VkDeviceSize bytes,void *guest,size_t guest_bytes) {
    return ch_vk_nv2a_store_color_state(s,NULL,rgba,bytes,guest,guest_bytes);
}
VkResult ch_vk_nv2a_load_color(const Nv2aSurface *s,void *rgba,
    VkDeviceSize bytes,const void *guest,size_t guest_bytes) {
    return ch_vk_nv2a_load_color_state(s,NULL,rgba,bytes,guest,guest_bytes);
}

static int clear_covers_surface(const Nv2aSurface *s,const Nv2aRenderState *rs,uint32_t flags) {
    if(!s || !rs || !s->width || !s->height || (flags&~0xf3u)) return 0;
    if(!rs->clear_rect_set) return 1;
    return rs->clear_rect_set==3 && !(rs->clear_rect_h&65535) && !(rs->clear_rect_v&65535) &&
        (rs->clear_rect_h>>16)+1>=s->width && (rs->clear_rect_v>>16)+1>=s->height;
}
int ch_vk_nv2a_clear_depth_full(const Nv2aSurface *s,const Nv2aRenderState *rs,uint32_t flags) {
    return (flags&3)==3 && clear_covers_surface(s,rs,flags);
}
int ch_vk_nv2a_clear_color_full(const Nv2aSurface *s,const Nv2aRenderState *rs,uint32_t flags) {
    return (flags&0xf0)==0xf0 && clear_covers_surface(s,rs,flags);
}
static VkResult clear_state(ChVkTarget *t,const Nv2aSurface *s,
    const Nv2aRenderState *rs,uint32_t flags,uint32_t argb,uint32_t zs,int panel,void *pixels,VkDeviceSize bytes) {
    if(!t || !rs || !s || !s->width || !s->height || (!panel && !matches(t,s))) return VK_ERROR_INITIALIZATION_FAILED;
    if(!rs->clear_rect_set) {
        Nv2aSurface extent=*s; ch_vk_target_size(t,&extent.width,&extent.height);
        return ch_vk_nv2a_clear_read(t,&extent,flags,argb,zs,pixels,bytes);
    }
    if(rs->clear_rect_set!=3 || (flags & ~0xf3u)) return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t x0=rs->clear_rect_h&65535,x1=(rs->clear_rect_h>>16)+1;
    uint32_t y0=rs->clear_rect_v&65535,y1=(rs->clear_rect_v>>16)+1;
    if(x1>s->width) x1=s->width;
    if(y1>s->height) y1=s->height;
    if(x0>=x1 || y0>=y1 || !flags)
        return ch_vk_target_clear_aspects_read(t,0,NULL,0,0,pixels,bytes);
    /* Full extents need no geometry or clear-specific graphics pipeline.
     * Keep masked color+depth clears as one rectangle draw. */
    if(!x0 && !y0 && x1==s->width && y1==s->height &&
       (!(flags&0xf0) || (flags&0xf0)==0xf0)) {
        Nv2aSurface extent=*s;ch_vk_target_size(t,&extent.width,&extent.height);
        return ch_vk_nv2a_clear_read(t,&extent,flags,argb,zs,pixels,bytes);
    }
    float left=x0/(float)s->width*2-1,right=x1/(float)s->width*2-1;
    float top=y0/(float)s->height*2-1,bottom=y1/(float)s->height*2-1;
    float depth=(float)((double)(zs>>8)/16777215.0);
    ChVkColorVertex quad[4]={{{left,top,depth,1},{0}},{{right,top,depth,1},{0}},
        {{right,bottom,depth,1},{0}},{{left,bottom,depth,1},{0}}};
    for(unsigned i=0;i<4;i++) {
        quad[i].rgba[0]=((argb>>16)&255)/255.f; quad[i].rgba[1]=((argb>>8)&255)/255.f;
        quad[i].rgba[2]=(argb&255)/255.f; quad[i].rgba[3]=(argb>>24)/255.f;
    }
    const uint16_t indices[]={0,1,2,0,2,3};
    Nv2aRenderState state={.color_mask=((flags & 0x10)?0x10000:0)|((flags & 0x20)?0x100:0)|
        ((flags & 0x40)?1:0)|((flags & 0x80)?0x1000000:0),
        .depth_test_enable=!!(flags&1),.depth_write=!!(flags&1),.depth_func=0x207,
        .stencil_enable=!!(flags&2),.stencil_func=0x207,.stencil_ref=zs&255,
        .stencil_func_mask=255,.stencil_write_mask=255,.stencil_fail=0x1e00,
        .stencil_zfail=0x1e00,.stencil_zpass=0x1e01};
    return ch_vk_target_draw_state_read(t,NULL,quad,4,indices,6,&state,pixels,bytes);
}

VkResult ch_vk_nv2a_draw_decoded_read(ChVkTarget *t,const Nv2aSurface *s,const Nv2aBatch *b,
    ChVkDecodeTexture decode,void *pixels,VkDeviceSize bytes) {
    return draw_decoded(t,s,b,decode,NULL,0,NULL,NULL,pixels,bytes,NULL);
}
VkResult ch_vk_nv2a_draw_eye_read(ChVkTarget *t,const Nv2aSurface *s,const Nv2aBatch *b,
    ChVkDecodeTexture decode,const ChVkEye *eye,void *pixels,VkDeviceSize bytes) {
    if(!eye) return VK_ERROR_INITIALIZATION_FAILED;
    return draw_decoded(t,s,b,decode,eye,0,NULL,NULL,pixels,bytes,NULL);
}

VkResult ch_vk_nv2a_projection_from_raw(const Nv2aBatch *batch,VrGameProj *out) {
    if(!batch || !out || !batch->raw) return VK_ERROR_INITIALIZATION_FAILED;
    const Nv2aRawGeometry *raw=batch->raw;
    if(raw->kind!=NV2A_RAW_FIXED || !raw->ff) return VK_ERROR_FEATURE_NOT_PRESENT;
    const Nv2aFixedState *ff=raw->ff;
    VrGameProj projection;
    /* Output in logical guest pixels; draw_eye_read removes AA scale. */
    if(!vr_game_proj_from(ff->composite,ff->mv[0],ff->use_composite,ff->proj,
        1,1,ff->c59,&projection) || projection.kx==0 || projection.ky==0 ||
        !isfinite(projection.inv_pz) || projection.inv_pz==0)
        return VK_ERROR_INITIALIZATION_FAILED;
    *out=projection; return VK_SUCCESS;
}

VkResult ch_vk_nv2a_draw_panel_read(ChVkTarget *t,const Nv2aSurface *s,const Nv2aBatch *b,
    ChVkDecodeTexture decode,int coverage_alpha,void *pixels,VkDeviceSize bytes) {
    if(!t || !b || b->zpass_out || (coverage_alpha!=0 && coverage_alpha!=1)) return VK_ERROR_INITIALIZATION_FAILED;
    return draw_decoded(t,s,b,decode,NULL,coverage_alpha?2:1,NULL,NULL,pixels,bytes,NULL);
}

VkResult ch_vk_nv2a_clear_state_read(ChVkTarget *t,const Nv2aSurface *s,const Nv2aRenderState *rs,
    uint32_t flags,uint32_t argb,uint32_t zs,void *pixels,VkDeviceSize bytes) {
    return clear_state(t,s,rs,flags,argb,zs,0,pixels,bytes);
}
VkResult ch_vk_nv2a_clear_panel_read(ChVkTarget *t,const Nv2aSurface *s,const Nv2aRenderState *rs,
    uint32_t flags,uint32_t argb,uint32_t zs,void *pixels,VkDeviceSize bytes) {
    return clear_state(t,s,rs,flags,argb,zs,1,pixels,bytes);
}

VkResult ch_vk_nv2a_draw_host_read(ChVkTarget *t,const Nv2aSurface *s,const Nv2aBatch *b,
    ChVkDecodeTexture decode,const ChVkEye *eye,int coverage_alpha,ChVkSampleResolver resolve,void *context,
    void *pixels,VkDeviceSize bytes) {
    if(!t || !b || b->zpass_out || (coverage_alpha!=0 && coverage_alpha!=1) || (eye && coverage_alpha))
        return VK_ERROR_INITIALIZATION_FAILED;
    return draw_decoded(t,s,b,decode,eye,eye?0:coverage_alpha?2:1,resolve,context,pixels,bytes,NULL);
}

VkResult ch_vk_nv2a_draw_fixed_resolved_read(ChVkTarget *t,const Nv2aSurface *s,const Nv2aBatch *b,
    const ChVkFixedGeometry *fixed,ChVkDecodeTexture decode,ChVkSampleResolver resolve,void *context,void *pixels,VkDeviceSize bytes) {
    if(!fixed || !fixed->vertices || !fixed->indices || !b || !b->raw) return VK_ERROR_INITIALIZATION_FAILED;
    return draw_decoded(t,s,b,decode,NULL,0,resolve,context,pixels,bytes,fixed);
}
