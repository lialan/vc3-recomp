/* Offscreen correctness path. Device/queue ownership stays with the host
 * (OpenXR must supply its runtime-selected device). Calls are serialized;
 * the caller must exclusively own the supplied queue during each call. */
#ifndef CHIHIRO_VK_TARGET_H
#define CHIHIRO_VK_TARGET_H
#include <vulkan/vulkan.h>
#include "vk_state.h"
#include "vk_texture.h"
typedef struct ChVkTarget ChVkTarget;
typedef struct {
    uint32_t kind; /* 1: soft spot/ring/flat beam; 2: sight glass/reticle */
    float color[4], shape[4], tint[4], reticle[4];
} ChVkEffect;
/* RGBA8 UNORM, tightly packed top-to-bottom rows. No guest memory writes. */
VkResult ch_vk_target_create(VkPhysicalDevice physical, VkDevice device,
                            VkQueue queue, uint32_t family,
                            uint32_t width, uint32_t height, ChVkTarget **out);
VkResult ch_vk_target_clear_read(ChVkTarget *target, const float rgba[4],
                                 void *pixels, VkDeviceSize bytes);
void ch_vk_target_destroy(ChVkTarget *target);
/* Opt-in resident color path. Existing draw/clear entry points still finish GPU
 * execution/query results, but leave their pixel output untouched. Explicit
 * read_color copies the completed target; depth/stencil APIs stay synchronous. */
void ch_vk_target_defer_color_readback(ChVkTarget *,int enabled);
VkResult ch_vk_target_read_color(ChVkTarget *,void *,VkDeviceSize);
/* Initial untextured indexed triangle path: clip-space xyzw and float RGBA (w=0 is shorthand for 1).
 * Non-NULL rgba clears all attachments (depth=1, stencil=0).
 * NULL rgba preserves all attachments; target must first be initialized with
 * a full clear. Guest winding is interpreted in surface coordinates (y down). */
typedef struct { float xy[27], rgba[4]; } ChVkColorVertex;
VkResult ch_vk_target_draw_read(ChVkTarget *target, const float rgba[4],
    const ChVkColorVertex *vertices, uint32_t vertex_count,
    const uint16_t *indices, uint32_t index_count, void *pixels, VkDeviceSize bytes);
/* Blend, color-mask and depth/stencil fields are implemented.
 * Depth uses normalized Vulkan [0,1] vertex z; guest depth-range conversion
 * belongs in the decoded-batch adapter. Alpha testing rounds alpha to 8 bits as the reference path does.
 * Culling accepts guest CW/CCW and front/back/both enums. */
VkResult ch_vk_target_draw_state_read(ChVkTarget *target, const float rgba[4],
    const ChVkColorVertex *vertices, uint32_t vertex_count,
    const uint16_t *indices, uint32_t index_count, const Nv2aRenderState *state,
    void *pixels, VkDeviceSize bytes);
/* Full-surface clears: flags color=1, depth=2, stencil=4; flags=0 only reads.
 * The first operation must clear all three. Unselected aspects are preserved.
 * These are host flags, not NV2A CLEAR_SURFACE channel bits. */
VkResult ch_vk_target_clear_aspects_read(ChVkTarget *target, uint32_t flags,
    const float rgba[4], float depth, uint32_t stencil, void *pixels, VkDeviceSize bytes);
void ch_vk_target_size(const ChVkTarget *target, uint32_t *width, uint32_t *height);
/* xy[0..3]=clip position; xy[4..5]=normalized UV. Texture must belong to
 * the same device and outlive this synchronous call. NULL means untextured. */
VkResult ch_vk_target_draw_texture_read(ChVkTarget *target, const float rgba[4],
    const ChVkColorVertex *vertices, uint32_t vertex_count,
    const uint16_t *indices, uint32_t index_count, const Nv2aRenderState *state,
    const ChVkTexture *texture, void *pixels, VkDeviceSize bytes);
/* CPU-decoded level-0 texture upload on this target's device/queue. */
VkResult ch_vk_target_upload_texture(ChVkTarget *target, uint32_t width, uint32_t height,
    const uint32_t *argb, ChVkTexture **out);
/* Host MUST have enabled occlusionQueryPrecise on VkDevice before opting in. */
VkResult ch_vk_target_enable_queries(ChVkTarget *target);
/* samples is replaced only after successful completion; optional NULL. */
VkResult ch_vk_target_draw_count_read(ChVkTarget *target, const float rgba[4],
    const ChVkColorVertex *vertices, uint32_t vertex_count,
    const uint16_t *indices, uint32_t index_count, const Nv2aRenderState *state,
    const ChVkTexture *texture, uint32_t *samples, void *pixels, VkDeviceSize bytes);
/* Copy initialized target to an externally acquired, same-device RGBA8 image
 * of identical extent. Discards destination contents; ends in color-attachment
 * layout. Optional diagnostic readback requires destination TRANSFER_SRC usage.
 * Caller excludes all other queue/image use until this synchronous call returns. */
VkResult ch_vk_target_export_image(ChVkTarget *target, VkImage destination,
    void *verification_rgba, VkDeviceSize verification_bytes);
VkFence ch_vk_target_completion_fence(const ChVkTarget *target);
VkDevice ch_vk_target_device(const ChVkTarget *target);
/* Fragment-combiner path accepts NONE, projective 2D/3D, noncube mode 3
 * and PASSTHRU (0..4). Stage images must belong to the same device. xy[6..9] specular, [10] fog,
 * [11..26] four stage coordinates. Guest adapter supplies these from ext. */
VkResult ch_vk_target_draw_combiner_read(ChVkTarget *target, const float rgba[4],
    const ChVkColorVertex *vertices, uint32_t vertex_count, const uint16_t *indices,
    uint32_t index_count, const Nv2aRenderState *state, const ChVkTexture *texture,
    uint32_t *samples, const Nv2aCombiner *rc, uint32_t fog_color,
    const ChVkTexture *const stage_images[4], void *pixels, VkDeviceSize bytes);
/* Replace color from tightly packed RGBA8; retain initialized depth/stencil.
 * Caller serializes queue use. Imported image ends in transfer-source layout. */
VkResult ch_vk_target_import_color(ChVkTarget *target, const void *rgba, VkDeviceSize bytes);
/* Explicit host representation: normalized float32 depth + uint8 stencil.
 * Transfers preserve color and finish in depth/stencil attachment layout.
 * This API does not infer a guest depth format/pitch from zeta_va. */
VkResult ch_vk_target_read_depth_stencil(ChVkTarget *target, float *depth,
    uint8_t *stencil, size_t pixels);
VkResult ch_vk_target_write_depth_stencil(ChVkTarget *target, const float *depth,
    const uint8_t *stencil, size_t pixels);
/* Premultiplied effect shading; xy[4..5] is local effect UV. */
VkResult ch_vk_target_draw_effect_read(ChVkTarget *,const ChVkColorVertex *,uint32_t,
    const uint16_t *,uint32_t,const Nv2aRenderState *,const ChVkEffect *,void *,VkDeviceSize);
/* Host panel variant: no guest query count. coverage_alpha uses GL reference
 * HUD's ONE / ONE_MINUS_SRC_ALPHA alpha factors, retaining guest RGB factors,
 * blend equation and color mask. Without blending, alpha remains guest output. */
VkResult ch_vk_target_draw_panel_read(ChVkTarget *,const ChVkColorVertex *,uint32_t,
    const uint16_t *,uint32_t,const Nv2aRenderState *,const ChVkTexture *,
    const Nv2aCombiner *,uint32_t,const ChVkTexture *const[4],int coverage_alpha,void *,VkDeviceSize);
#endif
