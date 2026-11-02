#include "vk_fixed.h"
#include "../nv2a_fixed_roles.h"
#include <stdlib.h>
#include <string.h>
static int snapshot_attribute(const Nv2aAttrib *a,uint32_t count,void *vertices,size_t stride,unsigned slot,uint32_t *format) {
    float value[4]={0,0,0,1};unsigned element=0;*format=0;
    if(a && !a->data) memcpy(value,a->value,sizeof value);
    if(a && a->data) {
        if(a->size<1 || a->size>4) return 0;
        element=a->type==0 || a->type==6?4:a->type==1 || a->type==5?a->size*2:a->type==4?a->size:a->type==2?a->size*4:0;
        uint64_t last=(uint64_t)(count-1)*a->stride;
        if(!element || last>a->bytes || element>a->bytes-last) return 0;
        *format=(a->size<<8)|(a->type+1);
    }
    for(uint32_t i=0;i<count;i++) {
        void *out=(unsigned char *)vertices+(size_t)i*stride+slot*16;
        memcpy(out,value,sizeof value);
        if(element) memcpy(out,(const unsigned char *)a->data+(size_t)i*a->stride,element);
    }
    return 1;
}
static void *storage(void **buffer,size_t *capacity,size_t bytes,size_t limit,unsigned *borrowed,unsigned bit) {
    if(!buffer || bytes>limit) return malloc(bytes);
    if(*capacity<bytes) {
        size_t size=4096;while(size<bytes) size*=2;
        void *grown=realloc(*buffer,size);if(!grown) return NULL;
        *buffer=grown;*capacity=size;
    }
    *borrowed|=bit;return *buffer;
}
/* Merge overlapping attribute spans before copying: interleaved streams must
 * not be snapshotted once per attribute. No numeric conversion or lane fill. */
static VkResult snapshot_streams(const Nv2aAttrib *const *attrs,unsigned slots,ChVkFixedGeometry *out,ChVkFixedScratch *scratch) {
    struct Span {uintptr_t first,last;size_t offset;} spans[16],groups[16];
    uintptr_t source[16];float defaults[16][4];unsigned n=0;
    for(unsigned a=0;a<slots;a++) {
        const Nv2aAttrib *v=attrs[a];uint64_t bytes=16;
        defaults[a][0]=defaults[a][1]=defaults[a][2]=0;defaults[a][3]=1;
        out->params.attribute_format[a]=0;out->params.attribute_stride[a]=0;
        if(v && v->data) {
            if(v->size<1 || v->size>4) return VK_ERROR_FEATURE_NOT_PRESENT;
            unsigned element=v->type==0 || v->type==6?4:v->type==1 || v->type==5?v->size*2:v->type==4?v->size:v->type==2?v->size*4:0;
            uint64_t last=(uint64_t)(out->vertex_count-1)*v->stride;
            if(!element || last>v->bytes || element>v->bytes-last) return VK_ERROR_FEATURE_NOT_PRESENT;
            bytes=last+element;source[a]=(uintptr_t)v->data;
            out->params.attribute_format[a]=(v->size<<8)|(v->type+1);out->params.attribute_stride[a]=v->stride;
        } else {
            if(v) memcpy(defaults[a],v->value,16);
            source[a]=(uintptr_t)defaults[a];
        }
        if(bytes>UINTPTR_MAX-source[a]) return VK_ERROR_FEATURE_NOT_PRESENT;
        spans[a]=(struct Span){source[a],source[a]+(uintptr_t)bytes,0};
    }
    for(unsigned a=1;a<slots;a++) {struct Span v=spans[a];unsigned j=a;
        while(j && spans[j-1].first>v.first) {spans[j]=spans[j-1];j--;}spans[j]=v;}
    for(unsigned a=0;a<slots;a++) {
        if(n && spans[a].first<=groups[n-1].last) {
            if(spans[a].last>groups[n-1].last) groups[n-1].last=spans[a].last;
        } else groups[n++]=spans[a];
    }
    size_t total=0,limit=(size_t)out->vertex_count*slots*16;
    for(unsigned g=0;g<n;g++) {
        uint64_t bytes=groups[g].last-groups[g].first;
        if(bytes>limit || total>limit-bytes || bytes>SIZE_MAX-3) return VK_NOT_READY;
        groups[g].offset=total;total+=(bytes+3)&~(size_t)3;
    }
    if(total>limit || total>UINT32_MAX-4) return VK_NOT_READY;
    total+=4; /* Last unaligned word may look into this zero guard. */
    out->vertices=storage(&scratch->vertices,&scratch->vertex_bytes,total,16u<<20,&out->borrowed,1);
    if(!out->vertices) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for(unsigned g=0;g<n;g++) {
        size_t bytes=groups[g].last-groups[g].first;
        memcpy((char *)out->vertices+groups[g].offset,(void *)groups[g].first,bytes);
        memset((char *)out->vertices+groups[g].offset+bytes,0,((bytes+3)&~(size_t)3)-bytes);
    }
    memset((char *)out->vertices+total-4,0,4);
    for(unsigned a=0;a<slots;a++) for(unsigned g=0;g<n;g++) if(source[a]>=groups[g].first && source[a]<groups[g].last) {
        out->params.attribute_offset[a]=(uint32_t)(groups[g].offset+source[a]-groups[g].first);break;
    }
    out->params.stream_bytes=(uint32_t)total;return VK_SUCCESS;
}
void ch_vk_fixed_scratch_release(ChVkFixedScratch *s) {
    if(s) {free(s->vertices);free(s->indices);memset(s,0,sizeof *s);}
}
void ch_vk_fixed_release(ChVkFixedGeometry *g) {
    if(!g) return;
    if(!(g->borrowed&1)) free(g->vertices);
    if(!(g->borrowed&2)) free(g->indices);
    memset(g,0,sizeof *g);
}
static VkResult prepare_topology(const Nv2aRawGeometry *raw,ChVkFixedGeometry *out,ChVkFixedScratch *scratch) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    memset(out,0,sizeof *out);
    if(!raw || raw->check_out || !raw->indices ||
       raw->max_index<raw->min_index || raw->max_index-raw->min_index>65535 ||
       raw->index_count<3 || (raw->prim!=5 && raw->prim!=6 && raw->prim!=7)) return VK_ERROR_FEATURE_NOT_PRESENT;
    /* Reusable Vulkan geometry preserves strips instead of expanding every
     * triangle on the CPU. Standalone preparation retains its list contract;
     * fans stay expanded for devices without portability triangle-fan support. */
    out->triangle_strip=scratch && raw->prim==6;
    uint64_t count=raw->prim==5 || out->triangle_strip?raw->index_count:(uint64_t)(raw->index_count-2)*3;
    if((!out->triangle_strip && count%3) || count>UINT32_MAX || count>SIZE_MAX/sizeof(uint16_t))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    out->vertex_count=raw->max_index-raw->min_index+1;out->index_count=(uint32_t)count;
    out->indices=storage(scratch?&scratch->indices:NULL,scratch?&scratch->index_bytes:NULL,
        (size_t)count*sizeof *out->indices,1u<<20,&out->borrowed,2);
    if(!out->indices) {ch_vk_fixed_release(out);return VK_ERROR_OUT_OF_HOST_MEMORY;}
    for(unsigned i=0;i<raw->index_count;i++) if(raw->indices[i]>=out->vertex_count) goto unsupported;
    if(raw->prim==5 || out->triangle_strip) memcpy(out->indices,raw->indices,(size_t)count*sizeof *out->indices);
    else for(unsigned i=0;i<raw->index_count-2;i++) {
        out->indices[i*3]=raw->indices[raw->prim==7?0:i+(i&1)];
        out->indices[i*3+1]=raw->indices[raw->prim==7?i+1:i+((i&1)^1)];
        out->indices[i*3+2]=raw->indices[i+2];
    }
    out->params.aa[0]=raw->aa_sx;out->params.aa[1]=raw->aa_sy;
    out->params.uv_scale0[0]=raw->uv_scale0[0];out->params.uv_scale0[1]=raw->uv_scale0[1];
    out->params.near_z=raw->near_z;out->params.vp_off_z=raw->vp_off_z;
    out->params.fog_enable=raw->fog_enable;out->params.fog_mode=raw->fog_mode;
    memcpy(out->params.fog_param,raw->fog_param,sizeof out->params.fog_param);
    for(unsigned i=0;i<4;i++) out->params.stage_scale[i][0]=out->params.stage_scale[i][1]=1;
    return VK_SUCCESS;
unsupported:
    ch_vk_fixed_release(out);return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VkResult prepare_fixed(const Nv2aRawGeometry *raw,ChVkFixedGeometry *out,ChVkFixedScratch *scratch) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    memset(out,0,sizeof *out);
    if(!raw || raw->kind!=NV2A_RAW_FIXED || !raw->ff) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkResult r=prepare_topology(raw,out,scratch);if(r!=VK_SUCCESS) return r;
    int slot[8];ch_nv2a_fixed_roles(raw->ff,slot);
    const Nv2aAttrib *attrs[8];
    for(unsigned a=0;a<8;a++) {if(slot[a]>=16) goto unsupported;attrs[a]=slot[a]>=0?&raw->attr[slot[a]]:NULL;}
    if(scratch) {
        r=snapshot_streams(attrs,8,out,scratch);
        if(r==VK_SUCCESS) {out->params.state=*raw->ff;return r;}
        if(r!=VK_NOT_READY) {ch_vk_fixed_release(out);return r;}
    }
    out->vertices=storage(scratch?&scratch->vertices:NULL,scratch?&scratch->vertex_bytes:NULL,
        (size_t)out->vertex_count*sizeof *out->vertices,16u<<20,&out->borrowed,1);
    if(!out->vertices) {ch_vk_fixed_release(out);return VK_ERROR_OUT_OF_HOST_MEMORY;}
    for(unsigned a=0;a<8;a++)
        if(!snapshot_attribute(slot[a]>=0?&raw->attr[slot[a]]:NULL,out->vertex_count,out->vertices,sizeof *out->vertices,a,&out->params.attribute_format[a])) goto unsupported;
    out->params.state=*raw->ff;return VK_SUCCESS;
unsupported:
    ch_vk_fixed_release(out);return VK_ERROR_FEATURE_NOT_PRESENT;
}
/* Same borrowed attribute/topology contract as fixed geometry, but all sixteen
 * registers are passed through to the vertex program on the GPU. */
size_t ch_vk_raw_params_size(const ChVkFixedParams *p) {
    return p->program_count?offsetof(ChVkFixedParams,program_count):offsetof(ChVkFixedParams,constants);
}
size_t ch_vk_raw_vertex_stride(const ChVkFixedParams *p) {
    return p->program_count?16*4*sizeof(float):sizeof(ChVkFixedVertex);
}
static VkResult prepare_program(const Nv2aRawGeometry *raw,ChVkFixedGeometry *out,ChVkFixedScratch *scratch) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    memset(out,0,sizeof *out);
    if(!raw || raw->kind!=NV2A_RAW_PROGRAM || !raw->program || !raw->constants ||
       !raw->program_len || raw->program_start>=136 || raw->program_len>136-raw->program_start)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    for(unsigned i=0;i<raw->program_len;i++)
        if(!!(raw->program[raw->program_start+i][3]&1)!=(i+1==raw->program_len))
            return VK_ERROR_FEATURE_NOT_PRESENT;
    VkResult r=prepare_topology(raw,out,scratch);
    if(r!=VK_SUCCESS) return r;
    uint32_t inputs=0;
    for(unsigned i=0;i<raw->program_len;i++) {
        const uint32_t *p=raw->program[raw->program_start+i];
        if(((p[2]>>26)&3)==2 || ((p[2]>>11)&3)==2 || ((p[3]>>28)&3)==2)
            inputs|=1u<<((p[1]>>9)&15);
    }
    if(scratch) {
        const Nv2aAttrib zero={0};const Nv2aAttrib *attrs[16];
        for(unsigned a=0;a<16;a++) attrs[a]=(inputs&(1u<<a))?&raw->attr[a]:&zero;
        r=snapshot_streams(attrs,16,out,scratch);
        if(r==VK_SUCCESS) goto prepared;
        if(r!=VK_NOT_READY) {ch_vk_fixed_release(out);return r;}
    }
    size_t bytes=(size_t)out->vertex_count*16*4*sizeof(float);
    out->vertices=storage(scratch?&scratch->vertices:NULL,scratch?&scratch->vertex_bytes:NULL,bytes,16u<<20,&out->borrowed,1);
    if(!out->vertices) {ch_vk_fixed_release(out);return VK_ERROR_OUT_OF_HOST_MEMORY;}
    memset(out->vertices,0,bytes);
    for(unsigned a=0;a<16;a++)
        if((inputs&(1u<<a)) && !snapshot_attribute(&raw->attr[a],out->vertex_count,out->vertices,16*4*sizeof(float),a,&out->params.attribute_format[a])) {ch_vk_fixed_release(out);return VK_ERROR_FEATURE_NOT_PRESENT;}
prepared:
    out->params.program_count=raw->program_len;
    memcpy(out->params.program,raw->program+raw->program_start,raw->program_len*16);
    memcpy(out->params.constants,raw->constants,sizeof out->params.constants);
    return VK_SUCCESS;
}

VkResult ch_vk_fixed_prepare(const Nv2aRawGeometry *raw,ChVkFixedGeometry *out) {return prepare_fixed(raw,out,NULL);}
VkResult ch_vk_program_prepare(const Nv2aRawGeometry *raw,ChVkFixedGeometry *out) {return prepare_program(raw,out,NULL);}
VkResult ch_vk_raw_prepare_reuse(const Nv2aRawGeometry *raw,ChVkFixedGeometry *out,ChVkFixedScratch *scratch) {
    return raw && raw->kind==NV2A_RAW_PROGRAM?prepare_program(raw,out,scratch):prepare_fixed(raw,out,scratch);
}
