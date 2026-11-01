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
    uint64_t count=raw->prim==5?raw->index_count:(uint64_t)(raw->index_count-2)*3;
    if(count%3 || count>UINT32_MAX || count>SIZE_MAX/sizeof(uint16_t))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    out->vertex_count=raw->max_index-raw->min_index+1;out->index_count=(uint32_t)count;
    out->indices=storage(scratch?&scratch->indices:NULL,scratch?&scratch->index_bytes:NULL,
        (size_t)count*sizeof *out->indices,1u<<20,&out->borrowed,2);
    if(!out->indices) {ch_vk_fixed_release(out);return VK_ERROR_OUT_OF_HOST_MEMORY;}
    for(unsigned i=0;i<raw->index_count;i++) if(raw->indices[i]>=out->vertex_count) goto unsupported;
    if(raw->prim==5) memcpy(out->indices,raw->indices,(size_t)count*sizeof *out->indices);
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
    out->vertices=storage(scratch?&scratch->vertices:NULL,scratch?&scratch->vertex_bytes:NULL,
        (size_t)out->vertex_count*sizeof *out->vertices,16u<<20,&out->borrowed,1);
    if(!out->vertices) {ch_vk_fixed_release(out);return VK_ERROR_OUT_OF_HOST_MEMORY;}
    int slot[8];ch_nv2a_fixed_roles(raw->ff,slot);
    for(unsigned a=0;a<8;a++) if(slot[a]>=16) goto unsupported;
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
    size_t bytes=(size_t)out->vertex_count*16*4*sizeof(float);
    out->vertices=storage(scratch?&scratch->vertices:NULL,scratch?&scratch->vertex_bytes:NULL,bytes,16u<<20,&out->borrowed,1);
    if(!out->vertices) {ch_vk_fixed_release(out);return VK_ERROR_OUT_OF_HOST_MEMORY;}
    memset(out->vertices,0,bytes);
    for(unsigned a=0;a<16;a++)
        if((inputs&(1u<<a)) && !snapshot_attribute(&raw->attr[a],out->vertex_count,out->vertices,16*4*sizeof(float),a,&out->params.attribute_format[a])) {ch_vk_fixed_release(out);return VK_ERROR_FEATURE_NOT_PRESENT;}
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
