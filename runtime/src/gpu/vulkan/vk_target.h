/* Offscreen correctness path. Device/queue ownership stays with the host
 * (OpenXR must supply its runtime-selected device). Calls are serialized;
 * the caller must exclusively own the supplied queue during each call. */
#ifndef CHIHIRO_VK_TARGET_H
#define CHIHIRO_VK_TARGET_H
#include <vulkan/vulkan.h>
#include "vk_state.h"
#include "vk_texture.h"
#include "vk_fixed.h"
typedef struct ChVkTarget ChVkTarget;
/* Exact D32 -> guest Z24 -> D32 GPU round-trip, preserving stencil.
 * Synchronous validation entry point; does not enable resident rendering. */
VkResult ch_vk_target_quantize_depth(ChVkTarget *);
/* Synchronous validation path: buffer-to-depth fullscreen draw alternative. */
VkResult ch_vk_target_quantize_depth_draw(ChVkTarget *);
/* Synchronous validation path: direct input attachment, no depth copy. */
VkResult ch_vk_target_quantize_depth_input(ChVkTarget *);
/* Opt-in per-draw round-trip without an additional host submission/wait.
 * mode: 0 off, 1 compute, 2 buffer draw, 3 input-attachment draw, 4 same-subpass draw.
 * Mode 4 must first be selected before any pipeline exists. Switching finishes
 * pending work; its compatible renderpass remains until target destruction.
 * Mode 5 is experimental fragment-side Z24 rounding before depth comparison,
 * intentionally distinct from the legacy post-draw modes. It canonicalizes
 * existing storage on enable, and subsequent uploads/clears at their boundary. */
VkResult ch_vk_target_depth_quantization(ChVkTarget *,int mode);
/* Before the first pipeline creation, borrow a same-device driver cache.
 * Caller serializes ALL accesses to it and retains it until targets die. */
VkResult ch_vk_target_pipeline_cache(ChVkTarget *,VkPipelineCache);
typedef struct {
    uint64_t pipeline_builds,pipeline_hits,pipeline_evictions,geometry_allocations;
    uint64_t batch_draws,batch_clears,batch_submissions,combined_depth_copies,combined_texture_copies,combined_depth_readbacks,combined_color_readbacks,queued_depth_uploads,queued_color_readbacks;
    uint64_t coherent_staging_skips,staging_maintenance_bytes;
    uint64_t batch_pipeline_binds,batch_blend_updates,batch_push_updates;
    uint64_t depth_roundtrips_by_compare[8]; /* Batched full-surface conversions, VkCompareOp order. */
    uint64_t no_alpha_pipeline_builds,no_alpha_pipeline_hits;
    uint64_t specialized_pipeline_builds,specialized_pipeline_hits;
    uint64_t pipeline_create_ns,pipeline_create_max_ns;
    VkDeviceSize geometry_capacity;
} ChVkTargetResourceStats;
/* Diagnostic counters. Geometry is persistently mapped
 * and reused only after completion. Pipeline objects have a bounded LRU. */
void ch_vk_target_resource_stats(const ChVkTarget *,ChVkTargetResourceStats *);
typedef struct {
    int available; /* Profile enabled, timestamps enabled and queue support */
    uint64_t sync_render_submissions,sync_render_gpu_ns,sync_render_host_ns;
    uint64_t batch_submissions,batch_draws,batch_gpu_ns,batch_record_ns;
    uint64_t batch_finish_ns,batch_submit_ns,batch_wait_ns,batch_retire_ns;
    uint64_t depth_upload_wait_ns;
} ChVkTargetTimingStats;
/* Completed batch GPU samples and host phase totals. No wait is introduced by
 * this snapshot; pending work is not yet included in completed GPU totals. */
void ch_vk_target_timing_stats(const ChVkTarget *,ChVkTargetTimingStats *);
typedef struct {
    uint32_t kind; /* 1: soft spot/ring/flat beam; 2: sight glass/reticle */
    float color[4], shape[4], tint[4], reticle[4];
} ChVkEffect;
/* RGBA8 UNORM, tightly packed top-to-bottom rows. No guest memory writes. */
VkResult ch_vk_target_create(VkPhysicalDevice physical, VkDevice device,
                            VkQueue queue, uint32_t family,
                            uint32_t width, uint32_t height, ChVkTarget **out);
/* Explicit depth precision for guest target residency. D24S8 retains the
 * reference renderer's normalized 24-bit depth between draws. The default
 * constructor remains D32S8 for existing float-depth clients. Unsupported
 * precision fails rather than silently changing depth comparisons. */
/* Explicit D32S8 target with input-attachment capability for modes 3/4.
 * Ordinary constructors retain their original image usage/allocation policy. */
VkResult ch_vk_target_create_input_depth(VkPhysicalDevice,VkDevice,VkQueue,
    uint32_t family,uint32_t width,uint32_t height,ChVkTarget **out);
VkResult ch_vk_target_create_depth(VkPhysicalDevice physical, VkDevice device,
    VkQueue queue, uint32_t family, uint32_t width, uint32_t height,
    VkFormat depth_format, ChVkTarget **out);
VkResult ch_vk_target_clear_read(ChVkTarget *target, const float rgba[4],
                                 void *pixels, VkDeviceSize bytes);
void ch_vk_target_destroy(ChVkTarget *target);
/* Opt-in resident color path. Without command batching, draw/clear entry points
 * still finish GPU execution/query results but leave their pixel output untouched. Explicit
 * read_color copies the completed target; depth/stencil APIs stay synchronous. */
void ch_vk_target_defer_color_readback(ChVkTarget *,int enabled);
/* Opt-in bounded command batches for resident color, no-query draws. Enable
 * before the first pipeline is created. Explicit reads/clears/transfers and
 * queries finish pending work; guest completion must call finish. Resource
 * reuse must remain enabled. Vertex/state data is copied before return; sampled
 * images must remain alive and immutable until finish. Default callers retain
 * synchronous semantics. */
VkResult ch_vk_target_batching(ChVkTarget *,int enabled);
#define CH_VK_MAX_BATCH_DRAWS 256u
/* Set before the first draw/pipeline. Zero selects the shared default (256);
 * 1..256 bounds descriptor reservations and pending draw/texture lifetimes. */
VkResult ch_vk_target_batch_limit(ChVkTarget *,uint32_t draws);
VkResult ch_vk_target_finish(ChVkTarget *);
uint32_t ch_vk_target_pending(const ChVkTarget *);
/* Transfer one immutable sampled-image lease to the completion boundary.
 * owned selects destruction; borrowed cache leases invoke release if non-NULL.
 * A call consumes the lease even if submission fails. */
VkResult ch_vk_target_retire_sample(ChVkTarget *,ChVkTexture *,
    void (*release)(void *,ChVkTexture *),void *,int owned);
/* Borrow output until end; do not operate on this target between begin/end.
 * Pending batches queue their readback; idle targets use the synchronous path. */
VkResult ch_vk_target_read_color_begin(ChVkTarget *,void *pixels,VkDeviceSize bytes);
VkResult ch_vk_target_read_color_end(ChVkTarget *);
VkResult ch_vk_target_read_color(ChVkTarget *,void *,VkDeviceSize);
/* Complete source draws, then make an owned immutable sampled texture.
 * Does not read or publish guest color memory. */
VkResult ch_vk_target_snapshot_texture(ChVkTarget *,uint32_t levels,ChVkTexture **);
/* Snapshot into a matching texture whose previous sampling has completed. */
VkResult ch_vk_target_refresh_texture(ChVkTarget *,ChVkTexture *);
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
/* With batching and deferred readback, eligible clears remain pending until
 * the usual finish/read/query boundary, just like resident draws. */
VkResult ch_vk_target_clear_aspects_read(ChVkTarget *target, uint32_t flags,
    const float rgba[4], float depth, uint32_t stencil, void *pixels, VkDeviceSize bytes);
void ch_vk_target_size(const ChVkTarget *target, uint32_t *width, uint32_t *height);
/* xy[0..3]=clip position; xy[4..5]=normalized UV. Texture must belong to
 * the same device and outlive the call, or finish when batching is enabled.
 * NULL means untextured. */
VkResult ch_vk_target_draw_texture_read(ChVkTarget *target, const float rgba[4],
    const ChVkColorVertex *vertices, uint32_t vertex_count,
    const uint16_t *indices, uint32_t index_count, const Nv2aRenderState *state,
    const ChVkTexture *texture, void *pixels, VkDeviceSize bytes);
/* CPU-decoded level-0 texture upload on this target's device/queue. */
VkResult ch_vk_target_upload_texture(ChVkTarget *target, uint32_t width, uint32_t height,
    const uint32_t *argb, ChVkTexture **out);
VkResult ch_vk_target_upload_texture_mips(ChVkTarget *,uint32_t width,uint32_t height,
    const uint32_t *argb,uint32_t levels,ChVkTexture **out);
VkResult ch_vk_target_upload_cube(ChVkTarget *,uint32_t size,const uint32_t *faces,ChVkTexture **out);
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
/* Select BGRA bytes for host color imports/readbacks (including paired depth
 * readbacks). GPU images and snapshot/verification APIs remain RGBA. Requires
 * a compute-capable graphics queue. Change only between complete host uses. */
VkResult ch_vk_target_color_bgra(ChVkTarget *target,int enabled);
/* 0 RGBA, 1 linear BGRA, 2 full rectangular power-of-two Morton BGRA.
 * Host buffers retain width*height*4 bytes; mode2 uses guest Morton order. */
VkResult ch_vk_target_color_layout(ChVkTarget *target,int mode);
VkResult ch_vk_target_import_color(ChVkTarget *target, const void *rgba, VkDeviceSize bytes);
/* Explicit host representation: normalized float32 depth + uint8 stencil.
 * Transfers preserve color and finish in depth/stencil attachment layout.
 * This API does not infer a guest depth format/pitch from zeta_va. */
VkResult ch_vk_target_read_depth_stencil(ChVkTarget *target, float *depth,
    uint8_t *stencil, size_t pixels);
VkResult ch_vk_target_write_depth_stencil(ChVkTarget *target, const float *depth,
    const uint8_t *stencil, size_t pixels);
/* Row-major guest words: depth in bits31..8, stencil in7..0. Native D24
 * uses integer transfers; D32 uses exact Z24 pack/unpack compute conversion
 * on a compute-capable graphics queue. D32 pack rounds like the CPU oracle. */
/* Read depth/stencil and color in one completed submission, using independent
 * staging. packed selects Z24/S8 guest words; otherwise depth is float[]. */
/* Two-phase depth/optional-color readback. Begin borrows output buffers until
 * end; no other operation on this target is allowed between them. Independent
 * targets may be submitted on the same serialized queue before any end call.
 * End waits for GPU completion before copying results or releasing resources. */
VkResult ch_vk_target_read_depth_begin(ChVkTarget *,void *depth,uint8_t *stencil,
    size_t pixels,int packed,void *rgba,size_t bytes);
VkResult ch_vk_target_read_depth_end(ChVkTarget *);
VkResult ch_vk_target_read_depth_color(ChVkTarget *,void *depth,uint8_t *stencil,
    size_t pixels,int packed,void *rgba,size_t color_bytes);
/* Packed guest transfers require a compute-capable queue for D24 and D32.
 * Transfer order only; float/stencil APIs remain linear.
 * Morton requires full power-of-two rectangular dimensions and compute queue. */
VkResult ch_vk_target_depth_morton(ChVkTarget *,int enabled);
VkResult ch_vk_target_read_z24s8(ChVkTarget *,uint32_t *words,size_t pixels);
/* Batched packed uploads may return after queue submission. Input bytes are
 * copied before return; same-queue draws, readback, finish and destruction
 * preserve ordering and completion. */
VkResult ch_vk_target_write_z24s8(ChVkTarget *,const uint32_t *words,size_t pixels);
/* Exact same-device/queue/extent/format copy. Completes both pending targets,
 * preserves their colors and restores depth/stencil attachment layouts. */
VkResult ch_vk_target_copy_depth_stencil(ChVkTarget *source,ChVkTarget *destination);
/* Premultiplied effect shading; xy[4..5] is local effect UV. */
VkResult ch_vk_target_draw_effect_read(ChVkTarget *,const ChVkColorVertex *,uint32_t,
    const uint16_t *,uint32_t,const Nv2aRenderState *,const ChVkEffect *,void *,VkDeviceSize);
/* Host panel variant: no guest query count. coverage_alpha uses GL reference
 * HUD's ONE / ONE_MINUS_SRC_ALPHA alpha factors, retaining guest RGB factors,
 * blend equation and color mask. Without blending, alpha remains guest output. */
VkResult ch_vk_target_draw_panel_read(ChVkTarget *,const ChVkColorVertex *,uint32_t,
    const uint16_t *,uint32_t,const Nv2aRenderState *,const ChVkTexture *,
    const Nv2aCombiner *,uint32_t,const ChVkTexture *const[4],int coverage_alpha,void *,VkDeviceSize);
/* Raw fixed-function GPU transforms. Host must enable shaderClipDistance and
 * depthClamp on the device. Params/vertices are copied before submission;
 * all resources and optional query results remain synchronous. */
VkResult ch_vk_target_draw_fixed_read(ChVkTarget *,const ChVkFixedParams *,
    const ChVkFixedVertex *,uint32_t,const uint16_t *,uint32_t,
    const Nv2aRenderState *,const ChVkTexture *,uint32_t *,const Nv2aCombiner *,
    uint32_t,const ChVkTexture *const[4],void *,VkDeviceSize);
#endif
