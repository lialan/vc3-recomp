#include "vk_fixed.h"
#include "../nv2a_fixed_roles.h"
#include <stdlib.h>
#include <string.h>
static int fetch(const Nv2aAttrib *a,uint32_t index,float out[4]) {
    out[0]=out[1]=out[2]=0;out[3]=1;
    if(!a) return 1;
    if(!a->data) {memcpy(out,a->value,4*sizeof(float));return 1;}
    if(a->size<1 || a->size>4) return 0;
    unsigned element=a->type==0 || a->type==6?4:a->type==1 || a->type==5?a->size*2:a->type==4?a->size:a->type==2?a->size*4:0;
    uint64_t offset=(uint64_t)index*a->stride;
    if(!element || offset>a->bytes || element>a->bytes-offset) return 0;
    const unsigned char *p=(const unsigned char *)a->data+(size_t)offset;
    if(a->type==0) {out[0]=p[2]/255.f;out[1]=p[1]/255.f;out[2]=p[0]/255.f;out[3]=p[3]/255.f;}
    else if(a->type==2) memcpy(out,p,element);
    else if(a->type==4) for(unsigned i=0;i<a->size;i++) out[i]=p[i]/255.f;
    else if(a->type==1 || a->type==5) for(unsigned i=0;i<a->size;i++) {int16_t v;memcpy(&v,p+i*2,2);out[i]=a->type==1?v/32767.f:v;}
    else if(a->type==6) {uint32_t v;memcpy(&v,p,4);out[0]=((int32_t)(v<<21)>>21)/1023.f;out[1]=((int32_t)(v<<10)>>21)/1023.f;out[2]=((int32_t)v>>22)/511.f;}
    return 1;
}
void ch_vk_fixed_release(ChVkFixedGeometry *g) {
    if(!g) return;
    free(g->vertices);free(g->indices);memset(g,0,sizeof *g);
}
static VkResult prepare_topology(const Nv2aRawGeometry *raw,ChVkFixedGeometry *out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    memset(out,0,sizeof *out);
    if(!raw || raw->check_out || !raw->indices ||
       raw->max_index<raw->min_index || raw->max_index-raw->min_index>65535 ||
       raw->index_count<3 || (raw->prim!=5 && raw->prim!=6 && raw->prim!=7)) return VK_ERROR_FEATURE_NOT_PRESENT;
    uint64_t count=raw->prim==5?raw->index_count:(uint64_t)(raw->index_count-2)*3;
    if(count%3 || count>UINT32_MAX || count>SIZE_MAX/sizeof(uint16_t))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    out->vertex_count=raw->max_index-raw->min_index+1;out->index_count=(uint32_t)count;
    out->indices=malloc((size_t)count*sizeof *out->indices);
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

VkResult ch_vk_fixed_prepare(const Nv2aRawGeometry *raw,ChVkFixedGeometry *out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    memset(out,0,sizeof *out);
    if(!raw || raw->kind!=NV2A_RAW_FIXED || !raw->ff) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkResult r=prepare_topology(raw,out);if(r!=VK_SUCCESS) return r;
    out->vertices=calloc(out->vertex_count,sizeof *out->vertices);
    if(!out->vertices) {ch_vk_fixed_release(out);return VK_ERROR_OUT_OF_HOST_MEMORY;}
    int slot[8];ch_nv2a_fixed_roles(raw->ff,slot);
    for(unsigned a=0;a<8;a++) if(slot[a]>=16) goto unsupported;
    for(unsigned i=0;i<out->vertex_count;i++) for(unsigned a=0;a<8;a++)
        if(!fetch(slot[a]>=0?&raw->attr[slot[a]]:NULL,i,out->vertices[i].attributes[a])) goto unsupported;
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
VkResult ch_vk_program_prepare(const Nv2aRawGeometry *raw,ChVkFixedGeometry *out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    memset(out,0,sizeof *out);
    if(!raw || raw->kind!=NV2A_RAW_PROGRAM || !raw->program || !raw->constants ||
       !raw->program_len || raw->program_start>=136 || raw->program_len>136-raw->program_start)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    for(unsigned i=0;i<raw->program_len;i++)
        if(!!(raw->program[raw->program_start+i][3]&1)!=(i+1==raw->program_len))
            return VK_ERROR_FEATURE_NOT_PRESENT;
    VkResult r=prepare_topology(raw,out);
    if(r!=VK_SUCCESS) return r;
    uint32_t inputs=0;
    for(unsigned i=0;i<raw->program_len;i++) {
        const uint32_t *p=raw->program[raw->program_start+i];
        if(((p[2]>>26)&3)==2 || ((p[2]>>11)&3)==2 || ((p[3]>>28)&3)==2)
            inputs|=1u<<((p[1]>>9)&15);
    }
    out->vertices=calloc(out->vertex_count,16*4*sizeof(float));
    if(!out->vertices) {ch_vk_fixed_release(out);return VK_ERROR_OUT_OF_HOST_MEMORY;}
    float (*v)[16][4]=(void *)out->vertices;
    for(unsigned i=0;i<out->vertex_count;i++) for(unsigned a=0;a<16;a++)
        if((inputs&(1u<<a)) && !fetch(&raw->attr[a],i,v[i][a])) {ch_vk_fixed_release(out);return VK_ERROR_FEATURE_NOT_PRESENT;}
    out->params.program_count=raw->program_len;
    memcpy(out->params.program,raw->program+raw->program_start,raw->program_len*16);
    memcpy(out->params.constants,raw->constants,sizeof out->params.constants);
    return VK_SUCCESS;
}
