#ifndef CH_VK_FIXED_H
#define CH_VK_FIXED_H
#include "nv2a_backend.h"
#include <stddef.h>
#include <vulkan/vulkan.h>
/* Role-ordered 16-byte attribute snapshots; float4 when format is zero. Shared shader arithmetic performs skinning,
 * lighting, texgen and transforms on the GPU; these are not screen vertices. */
typedef struct { float attributes[8][4]; } ChVkFixedVertex;
/* Scalar-only layout, mirrored from these C declarations into std430 by the
 * build generator, with compiler offsetof/sizeof assertions. */
typedef struct {
    Nv2aFixedState state;
    float surface[2],depth[2],texture[2],aa[2],uv_scale0[2];
    float stage_scale[4][2];
    uint32_t near_z,fog_enable,fog_mode;
    float vp_off_z,fog_param[2];
    /* Program suffix is omitted from fixed-function uploads. Instruction words
     * specialize the pipeline; only constants are uploaded per draw. */
    uint32_t attribute_format[16];
    float constants[192][4];
    uint32_t program_count,program[136][4];
} ChVkFixedParams;
typedef struct {
    ChVkFixedParams params;
    /* Program vertices contain sixteen 16-byte attributes (256-byte stride).
     * Fixed vertices retain their original eight-attribute layout. */
    ChVkFixedVertex *vertices;
    uint16_t *indices;
    uint32_t vertex_count,index_count;
} ChVkFixedGeometry;
/* Only copy encoded attributes and expand triangle topology. All fixed transforms remain
 * in the shared GPU shader. Unsupported raw kinds/check-output requests return
 * FEATURE_NOT_PRESENT before any GPU work so the executor can use its CPU path. */
VkResult ch_vk_fixed_prepare(const Nv2aRawGeometry *,ChVkFixedGeometry *);
/* Context-write programs must be excluded by the executor: GPU invocations
 * have no ordered cross-vertex constant side effects. */
VkResult ch_vk_program_prepare(const Nv2aRawGeometry *,ChVkFixedGeometry *);
size_t ch_vk_raw_params_size(const ChVkFixedParams *);
size_t ch_vk_raw_vertex_stride(const ChVkFixedParams *);
void ch_vk_fixed_release(ChVkFixedGeometry *);
#endif
