#ifndef CHIHIRO_VK_BACKEND_H
#define CHIHIRO_VK_BACKEND_H
#include "vk_nv2a.h"
#include "../guest_write_watch.h"
#include "vk_immersive.h"
typedef struct ChVkBackend ChVkBackend;
typedef struct {
    VkPhysicalDevice physical; VkDevice device; VkQueue queue; uint32_t family;
    /* Optional host-owned cache, created on device. Host keeps it alive until
     * backend destruction and serializes external use with backend callbacks.
     * Null creates a private cache. No ownership is transferred. */
    VkPipelineCache pipeline_cache;
    /* Optional paired host queue lock. Covers each complete callback, including
     * rendering hooks, and destruction. Hooks must not acquire it again.
     * Use when presentation and guest rendering share a VkQueue. */
    void (*lock_queue)(void *);
    void (*unlock_queue)(void *);
    void *queue_context;
    void *guest_base; size_t guest_bytes;
    /* Optional shared tracker with host alias/fault/syscall hooks installed.
     * Unsupported pages retain exact comparisons. Host owns it until destroy. */
    ChGuestWriteWatch *guest_write_watch;
    ChVkDecodeTexture decode;
    /* Set only when host enabled VkPhysicalDeviceFeatures.occlusionQueryPrecise. */
    int precise_queries;
    int gpu_texture_decode; /* common ARGB/XRGB on compute-capable graphics queue */
    int gpu_morton_color; /* full power-of-two Morton ARGB transfer layouts */
    int gpu_color_pack; /* linear resident ARGB color transfers on compute queue */
    int size_aware_texture_eviction; /* byte-budget eviction avoids bursts of tiny cold images */
    int gpu_morton_depth; /* full power-of-two Morton Z24S8 transfer layouts */
    int gpu_depth_pack; /* D24/D32 guest Z24/S8 conversion on compute-capable graphics queue */
    int raw_fixed; /* raw fixed/program GPU transforms; device enabled shaderClipDistance + depthClamp */
    /* Keep ARGB8888 targets resident between draws. Flush publishes guest
     * pixels at fences/flip or before texture decoding samples a dirty target.
     * RGB565 retains strict per-draw quantized writeback. Render routing hooks
     * currently require the strict path (resident_color=0). */
    int resident_color;
    int resident_depth; /* Z24: native D24 or opt-in quantized D32; other layouts stay strict */
    int multiple_depth; /* opt-in disjoint dirty depth owners */
    int reuse_clean_depth; /* retain published depth images across owner switches */
    int strict_depth_switches; /* diagnostic: CPU roundtrips even for identical zeta */
    int strict_depth_clears; /* diagnostic: import depth before full replacement */
    int quantized_depth; /* opt-in exact GPU Z24 roundtrip on D32 fallback */
    int depth_quant_draw; /* opt-in graphics roundtrip; requires quantized_depth */
    int depth_quant_fragment; /* experimental native-D24-style incoming rounding; requires quantized_depth */
    int depth_quant_inline; /* experimental same-subpass Z24; requires quantized_depth */
    int depth_float_transfers; /* diagnostic: normalized-float staging instead of exact packed D24 */
    int batch_draws; /* resident no-query draws, complete before guest fences */
    uint32_t batch_draw_limit; /* zero: shared default; otherwise 1..CH_VK_MAX_BATCH_DRAWS */
    int strict_texture_checks; /* disable tracked texture comparisons for diagnostics */
    int cache_textures; /* exact level-zero storage/palette/sampler cache */
    int sample_render_targets; /* opt-in immutable GPU snapshots; requires both resident_color/cache_textures */
    /* Guest kickoff/semaphore/report completions submit pending GPU work
     * without waiting or publishing resident color/depth to guest RAM, as the
     * OpenGL renderer does: only for titles that never read rendered pixels or
     * depth from the CPU. Visibility counts come from unbatched query draws,
     * already complete. Flips, CPU-decoded sampling and surface switches still
     * publish. Ignored with render routing hooks. */
    int lazy_completion;
    /* With lazy completion and a GPU presenter, flips publish nothing and wait
     * for nothing either (vk_backend.c flip()), and frames pipeline: a guest
     * completion submits a target only while none of its batches is on the
     * GPU, and a surface switching between linear and swizzled storage
     * converts its colors on the CPU. CHIHIRO_VULKAN_LAZY_FLIP, else the
     * game's profile (gpu.lazy_flip). */
    int lazy_flip;
    /* Guest completions and flips publish resident color but leave dirty
     * resident depth on the GPU, as the OpenGL renderer always does. Depth
     * still reaches guest RAM before CPU-decoded sampling and on owner
     * switches. Only for titles that never read depth from the CPU. */
    int completion_keep_depth;
    uint32_t texture_cache_entries; /* zero: default capacity */
    size_t texture_cache_bytes; /* zero: default byte budget */
    /* Synchronous host rendering hooks, after guest-visible completion.
     * All pointers borrowed until hook returns; never retain decoded pointers.
     * Host routes world/HUD/postprocess explicitly. Errors stop guest progress.
     * Hooks run on serialized submission thread and must not reenter backend.
     * flip hook receives presented guest address for host target selection. */
    /* Optional metadata observation under RECOMP_GPU_XF=ff/all. Always declines
     * raw rendering so executor still produces CPU-transformed batches. Host
     * must explicitly enable that offer; this does not select GPU transforms. */
    VkResult (*observe_raw)(void *,const Nv2aSurface *,const Nv2aBatch *);
    void *render_context;
    /* Before draw/clear/flip, exact guest color comparisons against our last
     * canonical writeback detect CPU edits (including sampled source targets).
     * initial=1 seeds a new surface; initial=0 invalidates stale stereo data.
     * RGBA borrowed until return; padding changes are not pixel changes.
     * These notifications precede guest GPU work, unlike render_* hooks. */
    VkResult (*guest_color_changed)(void *,const Nv2aSurface *,const void *,size_t,int initial);
    VkResult (*render_clear)(void *,const Nv2aSurface *,const Nv2aRenderState *,uint32_t,uint32_t,uint32_t);
    VkResult (*render_draw)(void *,const Nv2aSurface *,const Nv2aBatch *);
    VkResult (*render_flip)(void *,uint32_t presented_va);
    /* GPU-native alternative, preferred over the CPU callbacks below. Target
     * includes current CPU edits. Borrowed until return: copy for another
     * thread while the backend queue lock is held. VK_NOT_READY drops only
     * the host frame under bounded mailbox pressure. No ownership transfer. */
    VkResult (*present_target)(void *,const Nv2aSurface *,ChVkTarget *);
    /* Completed flat guest image at flip, including intervening CPU writes.
     * Tightly packed RGBA8, top-left origin, straight guest alpha. Borrowed
     * until return. Called before render_flip; zero presented VA has no image. */
    VkResult (*present_rgba)(void *,const Nv2aSurface *,const void *,size_t);
    /* Optional native guest-order handoff; requires present_rgba as fallback. */
    VkResult (*present_bgra)(void *,const Nv2aSurface *,const void *,size_t);
    /* Immersive VR (vk_immersive.h), optional; requires raw_fixed. At each
     * flip, the next frame's head view (0: flat). A frame drawn immersive
     * goes to present_immersive instead of present_target: eyes, both side
     * by side (2*immersive_eye_width x immersive_eye_height), the HUD
     * (NULL: none), premultiplied, and the lens picture (NULL: none; see
     * ChVkImmFrame.lens). Same borrowing rules as present_target. */
    int (*immersive_view)(void *,Vc3VrView *);
    VkResult (*present_immersive)(void *,const Nv2aSurface *,ChVkTarget *eyes,ChVkTarget *hud,ChVkTarget *lens,
        const ChVkImmFrame *);
    uint32_t immersive_eye_width,immersive_eye_height;
    float immersive_lens_k;   /* the game's usual lens, pixels at 640 wide (0: 772.5) */
    uint32_t (*immersive_lens)(void);   /* the lens picture's guest surface (vr_view.h; NULL/0: none) */
    VrGameProj immersive_program_projection;   /* ChVkImmersiveConfig.program_projection */
    /* Flat 16:9 (vk_wide.h), optional; requires raw_fixed and present_rgba.
     * At each flip, non-zero to draw wide from then on: the presented
     * surface's wide picture (its height, 16:9) goes to present_rgba at its
     * own size instead of the guest surface. */
    int (*wide_view)(void *);
    /* Called on fatal rendering errors. Must stop execution; abort follows if
     * it returns. No fallback or continued guest fence progress is allowed. */
    void (*fatal)(VkResult result, const char *operation);
} ChVkBackendConfig;
/* Experimental synchronous color-writeback backend. Host must explicitly opt
 * in only when supported semantics suffice; this is not a complete renderer.
 * One globally attached instance (Nv2aBackend has no user pointer), calls on
 * one serialized executor thread. No host/device/guest-memory ownership. */
VkResult ch_vk_backend_create(const ChVkBackendConfig *config, ChVkBackend **out);
VkResult ch_vk_backend_attach(ChVkBackend *backend);
/* Detach/destroy only after executor submission is stopped and drained. */
void ch_vk_backend_destroy(ChVkBackend *backend);
const Nv2aBackend *ch_vk_backend_callbacks(void);
/* The target drawn for the guest color surface at color_va (NULL: none).
 * Only on the executor thread, e.g. from a present callback. */
ChVkTarget *ch_vk_backend_target(ChVkBackend *backend, uint32_t color_va);
#endif
