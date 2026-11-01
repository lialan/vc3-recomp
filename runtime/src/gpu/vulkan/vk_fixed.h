#ifndef CH_VK_FIXED_H
#define CH_VK_FIXED_H
#include "nv2a_backend.h"
#include <vulkan/vulkan.h>
/* Role-ordered float attributes. Shared shader arithmetic performs skinning,
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
} ChVkFixedParams;
typedef struct {
    ChVkFixedParams params;
    ChVkFixedVertex *vertices;
    uint16_t *indices;
    uint32_t vertex_count,index_count;
} ChVkFixedGeometry;
/* Only unpack attributes and triangle topology. All fixed transforms remain
 * in the shared GPU shader. Unsupported raw kinds/check-output requests return
 * FEATURE_NOT_PRESENT before any GPU work so the executor can use its CPU path. */
VkResult ch_vk_fixed_prepare(const Nv2aRawGeometry *,ChVkFixedGeometry *);
void ch_vk_fixed_release(ChVkFixedGeometry *);
#endif
