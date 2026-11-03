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
    /* Immersive VR (vk_immersive.h): 0 the guest surface as drawn; 1 a 3D
     * draw re-projected through each eye, 2 a flat 2D draw in each eye.
     * Non-zero draws two instances, one per eye, side by side in one
     * target. imm_gp: the game's projection kx, ky, cx, cy, 1/pz (surface
     * pixels); imm_vp: per eye, game camera (GL view space) to the eye's
     * GL clip space, column-major. Before attribute_format: fixed-function
     * uploads end at constants. */
    uint32_t imm_mode;
    /* Mode 2: textures sampled from another surface's eyes (side by side):
     * bit i stage i, bit 4 the plain texture; each eye samples its half. */
    uint32_t imm_sbs;
    float imm_gp[5],imm_vp[2][16];
    /* Dynamic resolution (vk_immersive.c): each eye's picture fills this
     * fraction of its half (left eye from the top-left corner, right eye
     * from the top-right), and eye images are sampled likewise. 1: all. */
    float imm_scale;
    /* Program suffix is omitted from fixed-function uploads. Instruction words
     * specialize the pipeline; only constants are uploaded per draw. */
    uint32_t attribute_format[16];
    uint32_t stream_bytes,attribute_offset[16],attribute_stride[16];
    float constants[192][4];
    uint32_t program_count,program[136][4];
} ChVkFixedParams;
typedef struct {
    ChVkFixedParams params;
    /* With stream_bytes==0, program vertices have sixteen 16-byte attributes
     * and fixed vertices have eight. Otherwise this points to encoded streams. */
    ChVkFixedVertex *vertices;
    uint16_t *indices;
    uint32_t vertex_count,index_count;
    unsigned borrowed; /* scratch-owned vertices/index bits; release still required */
} ChVkFixedGeometry;
/* Reuse preparation may return compact streams (params.stream_bytes nonzero);
 * vertices then names that many encoded bytes, not padded vertex records. */
/* Serialized scratch storage; no prepared geometry may outlive its next use.
 * Inputs must not alias this storage. Retains at most16MiB vertices+1MiB indices. */
typedef struct {void *vertices,*indices;size_t vertex_bytes,index_bytes;} ChVkFixedScratch;
VkResult ch_vk_raw_prepare_reuse(const Nv2aRawGeometry *,ChVkFixedGeometry *,ChVkFixedScratch *);
void ch_vk_fixed_scratch_release(ChVkFixedScratch *);
/* Only copy encoded attributes and expand triangle topology. All fixed transforms remain
 * in the shared GPU shader. Unsupported raw kinds/check-output requests return
 * FEATURE_NOT_PRESENT before any GPU work so the executor can use its CPU path. */
VkResult ch_vk_fixed_prepare(const Nv2aRawGeometry *,ChVkFixedGeometry *);
/* Context-write programs must be excluded by the executor: GPU invocations
 * have no ordered cross-vertex constant side effects. */
VkResult ch_vk_program_prepare(const Nv2aRawGeometry *,ChVkFixedGeometry *);
size_t ch_vk_raw_params_size(const ChVkFixedParams *);
/* Vertex programs interpreted in a loop over the uploaded words instead of
 * 136 specialized steps: on Qualcomm's proprietary driver (the Quest), whose
 * compiler did not finish the specialized module. CHIHIRO_VULKAN_VP_LOOP=0|1
 * overrides. Set once, before any pipeline is built. */
extern int ch_vk_vertex_program_loop;
void ch_vk_vertex_program_loop_init(VkPhysicalDevice physical);
size_t ch_vk_raw_vertex_stride(const ChVkFixedParams *);
void ch_vk_fixed_release(ChVkFixedGeometry *);
#endif
