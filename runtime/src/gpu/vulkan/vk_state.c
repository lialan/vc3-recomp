#include "vk_state.h"
#include <string.h>
static int factor(uint32_t f, VkBlendFactor *out) {
    switch(f) {
    case 0: *out=VK_BLEND_FACTOR_ZERO; break;
    case 1: *out=VK_BLEND_FACTOR_ONE; break;
    case 0x300: *out=VK_BLEND_FACTOR_SRC_COLOR; break;
    case 0x301: *out=VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR; break;
    case 0x302: *out=VK_BLEND_FACTOR_SRC_ALPHA; break;
    case 0x303: *out=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; break;
    case 0x304: *out=VK_BLEND_FACTOR_DST_ALPHA; break;
    case 0x305: *out=VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA; break;
    case 0x306: *out=VK_BLEND_FACTOR_DST_COLOR; break;
    case 0x307: *out=VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; break;
    case 0x308: *out=VK_BLEND_FACTOR_SRC_ALPHA_SATURATE; break;
    case 0x8001: *out=VK_BLEND_FACTOR_CONSTANT_COLOR; break;
    case 0x8002: *out=VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR; break;
    case 0x8003: *out=VK_BLEND_FACTOR_CONSTANT_ALPHA; break;
    case 0x8004: *out=VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA; break;
    default: return 0;
    }
    return 1;
}
VkResult ch_vk_blend_state(const Nv2aRenderState *s,
    VkPipelineColorBlendAttachmentState *out, float constants[4]) {
    if(!s || !out || !constants) return VK_ERROR_INITIALIZATION_FAILED;
    VkPipelineColorBlendAttachmentState b={0};
    if(s->color_mask & 0x00010000) b.colorWriteMask|=VK_COLOR_COMPONENT_R_BIT;
    if(s->color_mask & 0x00000100) b.colorWriteMask|=VK_COLOR_COMPONENT_G_BIT;
    if(s->color_mask & 0x00000001) b.colorWriteMask|=VK_COLOR_COMPONENT_B_BIT;
    if(s->color_mask & 0x01000000) b.colorWriteMask|=VK_COLOR_COMPONENT_A_BIT;
    b.blendEnable=!!s->blend_enable;
    b.srcColorBlendFactor=b.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;
    b.dstColorBlendFactor=b.dstAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;
    b.colorBlendOp=b.alphaBlendOp=VK_BLEND_OP_ADD;
    if(b.blendEnable) {
        switch(s->blend_eq) {
        case 0: /* Reference GL path treats an unset equation as ADD. */
        case 0x8006: b.colorBlendOp=VK_BLEND_OP_ADD; break;
        case 0x8007: b.colorBlendOp=VK_BLEND_OP_MIN; break;
        case 0x8008: b.colorBlendOp=VK_BLEND_OP_MAX; break;
        case 0x800a: b.colorBlendOp=VK_BLEND_OP_SUBTRACT; break;
        case 0x800b: b.colorBlendOp=VK_BLEND_OP_REVERSE_SUBTRACT; break;
        default: return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        b.alphaBlendOp=b.colorBlendOp;
        /* MIN/MAX ignore factors in both NV2A and Vulkan. */
        if(b.colorBlendOp!=VK_BLEND_OP_MIN && b.colorBlendOp!=VK_BLEND_OP_MAX) {
            if(!factor(s->blend_src,&b.srcColorBlendFactor) ||
               !factor(s->blend_dst,&b.dstColorBlendFactor)) return VK_ERROR_FEATURE_NOT_PRESENT;
            b.srcAlphaBlendFactor=b.srcColorBlendFactor;
            b.dstAlphaBlendFactor=b.dstColorBlendFactor;
        }
    }
    constants[0]=((s->blend_color>>16)&255)/255.0f;
    constants[1]=((s->blend_color>>8)&255)/255.0f;
    constants[2]=(s->blend_color&255)/255.0f;
    constants[3]=(s->blend_color>>24)/255.0f;
    *out=b;
    return VK_SUCCESS;
}

static int compare(uint32_t value, VkCompareOp *out) {
    if(value<0x200 || value>0x207) return 0;
    *out=(VkCompareOp)(value-0x200); return 1;
}
static int stencil_op(uint32_t value, VkStencilOp *out) {
    switch(value) {
    case 0x1e00: *out=VK_STENCIL_OP_KEEP; break;
    case 0: *out=VK_STENCIL_OP_ZERO; break;
    case 0x1e01: *out=VK_STENCIL_OP_REPLACE; break;
    case 0x1e02: *out=VK_STENCIL_OP_INCREMENT_AND_CLAMP; break;
    case 0x1e03: *out=VK_STENCIL_OP_DECREMENT_AND_CLAMP; break;
    case 0x150a: *out=VK_STENCIL_OP_INVERT; break;
    case 0x8507: *out=VK_STENCIL_OP_INCREMENT_AND_WRAP; break;
    case 0x8508: *out=VK_STENCIL_OP_DECREMENT_AND_WRAP; break;
    default: return 0;
    }
    return 1;
}
VkResult ch_vk_depth_state(const Nv2aRenderState *s, VkPipelineDepthStencilStateCreateInfo *out) {
    if(!s || !out) return VK_ERROR_INITIALIZATION_FAILED;
    VkPipelineDepthStencilStateCreateInfo d={.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable=!!s->depth_test_enable,.depthWriteEnable=!!s->depth_write,
        .depthCompareOp=VK_COMPARE_OP_ALWAYS,.stencilTestEnable=!!s->stencil_enable};
    if(d.depthTestEnable && !compare(s->depth_func,&d.depthCompareOp)) return VK_ERROR_FEATURE_NOT_PRESENT;
    if(d.stencilTestEnable) {
        d.front.compareMask=s->stencil_func_mask & 255;
        d.front.writeMask=s->stencil_write_mask & 255;
        d.front.reference=s->stencil_ref & 255;
        if(!compare(s->stencil_func,&d.front.compareOp) ||
           !stencil_op(s->stencil_fail,&d.front.failOp) ||
           !stencil_op(s->stencil_zfail,&d.front.depthFailOp) ||
           !stencil_op(s->stencil_zpass,&d.front.passOp)) return VK_ERROR_FEATURE_NOT_PRESENT;
        d.back=d.front;
    }
    *out=d; return VK_SUCCESS;
}

static int address_mode(uint32_t guest,VkSamplerAddressMode *out) {
    switch(guest) {
    case 1: *out=VK_SAMPLER_ADDRESS_MODE_REPEAT; return 1;
    case 2: *out=VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; return 1;
    case 3: *out=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; return 1;
    default: return 0;
    }
}
VkResult ch_vk_sampler_state(const Nv2aTexture *s,VkSamplerCreateInfo *out) {
    if(!s || !out) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t min=(s->filter>>16)&255,mag=(s->filter>>24)&15;
    /* Point/linear filters use level zero. Filters 3-6 select the reference
     * generated chain when eligible, otherwise the reference linear fallback. */
    if(s->cube || min<1 || min>6 || (mag!=1 && mag!=2))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkSamplerCreateInfo sampler={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter=mag==1?VK_FILTER_NEAREST:VK_FILTER_LINEAR,
        .minFilter=min==1 || min==3 || min==5?VK_FILTER_NEAREST:VK_FILTER_LINEAR,
        .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    uint32_t levels=ch_nv2a_texture_mip_levels(s,1);
    if(min>=3 && levels==1) sampler.minFilter=VK_FILTER_LINEAR;
    sampler.mipmapMode=min>=5?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.maxLod=(float)(levels-1);
    if(!address_mode(s->addr_u,&sampler.addressModeU) || !address_mode(s->addr_v,&sampler.addressModeV))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    *out=sampler; return VK_SUCCESS;
}
