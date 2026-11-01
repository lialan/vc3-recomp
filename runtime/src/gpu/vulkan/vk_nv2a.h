#ifndef CHIHIRO_VK_NV2A_H
#define CHIHIRO_VK_NV2A_H
#include "vk_target.h"
#include <stddef.h>
#include "../../host/vr_math.h"
/* Decoded command adapter, no global backend registration or presentation yet.
 * Target dimensions must match the guest surface. Draws preserve attachments;
 * explicit guest clear must initialize them before first draw. Unsupported
 * combiner/raw batches fail without submitting a partial draw.
 * draw_read rejects textures; draw_decoded_read accepts the documented subset.
 * No guest memory is written by these readback APIs. */
VkResult ch_vk_nv2a_clear_read(ChVkTarget *target, const Nv2aSurface *surface,
    uint32_t flags, uint32_t argb, uint32_t zstencil, void *pixels, VkDeviceSize bytes);
/* True only for a valid clear replacing both components across the full target. */
int ch_vk_nv2a_clear_depth_full(const Nv2aSurface *,const Nv2aRenderState *,uint32_t flags);
VkResult ch_vk_nv2a_draw_read(ChVkTarget *target, const Nv2aSurface *surface,
    const Nv2aBatch *batch, void *pixels, VkDeviceSize bytes);
/* Supply nv2a_backend_decode_texture in the runtime; dependency injection keeps
 * standalone GPU fixtures independent of the executor's guest-memory globals.
 * Sampling supports generated 2D mips and six-face directional cube maps. */
typedef int (*ChVkDecodeTexture)(const Nv2aTexture *, uint32_t *argb);
/* Output capacity: width*height uint32 pixels, or six such planes for cube. */
int ch_vk_decode_texture_faces(const Nv2aTexture *,ChVkDecodeTexture,uint32_t *);
VkResult ch_vk_nv2a_draw_decoded_read(ChVkTarget *target, const Nv2aSurface *surface,
    const Nv2aBatch *batch, ChVkDecodeTexture decode, void *pixels, VkDeviceSize bytes);
/* Explicit linear color-surface writeback after a completed readback.
 * Guest storage and rgba must not alias. Validates the entire span before any
 * ordered byte writes. Supports ARGB8888 and RGB565; padding is untouched.
 * Swizzled surfaces and depth/stencil export require additional metadata/APIs. */
VkResult ch_vk_nv2a_store_color(const Nv2aSurface *surface,
    const void *rgba, VkDeviceSize rgba_bytes, void *guest_base, size_t guest_bytes);
VkResult ch_vk_nv2a_load_color(const Nv2aSurface *surface, void *rgba,
    VkDeviceSize rgba_bytes, const void *guest_base, size_t guest_bytes);
/* State-aware color transfers honor pitched and Morton render-target layouts.
 * The legacy functions above retain their explicit linear contract. */
VkResult ch_vk_nv2a_store_color_state(const Nv2aSurface *,const Nv2aRenderState *,
    const void *,VkDeviceSize,void *,size_t);
VkResult ch_vk_nv2a_load_color_state(const Nv2aSurface *,const Nv2aRenderState *,
    void *,VkDeviceSize,const void *,size_t);
/* State-aware clear: inclusive rectangle bounds, clipped to target extent.
 * Rectangle draws ignore guest blend/alpha/cull tests, preserve outside pixels.
 * Initialized target required for a preserving clear; backend imports guest data. */
VkResult ch_vk_nv2a_clear_state_read(ChVkTarget *,const Nv2aSurface *,
    const Nv2aRenderState *,uint32_t flags,uint32_t argb,uint32_t zstencil,
    void *pixels,VkDeviceSize bytes);
typedef struct {
    VrGameProj game_projection;
    float gl_eye_vp[16]; /* shared vr_eye_matrix output, column-major */
} ChVkEye;
/* Explicit immersive 3D batch path; host classifies/routs HUD/menu batches
 * separately. Allows native eye target dimensions independent of guest surface.
 * Uses shared inverse guest projection, then GL->Vulkan clip conversion. */
VkResult ch_vk_nv2a_draw_eye_read(ChVkTarget *,const Nv2aSurface *,const Nv2aBatch *,
    ChVkDecodeTexture,const ChVkEye *,void *pixels,VkDeviceSize bytes);
/* Recover logical guest-pixel projection from fixed-function raw metadata.
 * No cache/guest addresses. Output untouched on failure. Program batches need
 * separate provenance/recovery; absence of fixed metadata is explicit. */
VkResult ch_vk_nv2a_projection_from_raw(const Nv2aBatch *,VrGameProj *);
/* Host screen/HUD draw at independent target resolution using guest pixel
 * coordinates. No guest visibility queries: caller must clear zpass_out in its
 * borrowed batch copy after guest execution. coverage_alpha selects the GL
 * reference HUD alpha-coverage accumulation. Guest APIs remain unchanged. */
VkResult ch_vk_nv2a_draw_panel_read(ChVkTarget *,const Nv2aSurface *,const Nv2aBatch *,
    ChVkDecodeTexture,int coverage_alpha,void *,VkDeviceSize);
/* Same clear semantics mapped from guest coordinates to host target extent. */
VkResult ch_vk_nv2a_clear_panel_read(ChVkTarget *,const Nv2aSurface *,const Nv2aRenderState *,
    uint32_t,uint32_t,uint32_t,void *,VkDeviceSize);
/* Optional host-native image for a guest texture binding. VK_NOT_READY uses
 * the normal guest decoder; all errors stop the draw. Returned ARGB pixels are
 * borrowed until the next resolver call (upload completes synchronously).
 * Guest UV normalization still uses the original texture dimensions. */
typedef struct {
    const uint32_t *argb; uint32_t width,height;
    /* Alternative immutable image with its final sampler already installed.
     * Pin stays valid across further resolver calls until release after GPU
     * completion. NULL release means host-owned through the whole draw. */
    ChVkTexture *gpu;
    void (*release)(void *,ChVkTexture *);
    void *release_context;
} ChVkSampleImage;
typedef VkResult (*ChVkSampleResolver)(void *,const Nv2aTexture *,ChVkSampleImage *);
VkResult ch_vk_nv2a_draw_host_read(ChVkTarget *,const Nv2aSurface *,const Nv2aBatch *,
    ChVkDecodeTexture,const ChVkEye *,int coverage_alpha,ChVkSampleResolver,void *,void *,VkDeviceSize);
/* Guest-surface variant retains visibility queries and guest pixel coordinates. */
VkResult ch_vk_nv2a_draw_resolved_read(ChVkTarget *,const Nv2aSurface *,const Nv2aBatch *,
    ChVkDecodeTexture,ChVkSampleResolver,void *,void *,VkDeviceSize);
/* Prepared raw fixed geometry uses the same texture resolver/coherence path as
 * decoded draws; only the vertex transformation is performed on the GPU. */
VkResult ch_vk_nv2a_draw_fixed_resolved_read(ChVkTarget *,const Nv2aSurface *,const Nv2aBatch *,
    const ChVkFixedGeometry *,ChVkDecodeTexture,ChVkSampleResolver,void *,void *,VkDeviceSize);
#endif
