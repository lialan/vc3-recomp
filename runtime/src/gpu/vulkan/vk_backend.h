#ifndef CHIHIRO_VK_BACKEND_H
#define CHIHIRO_VK_BACKEND_H
#include "vk_nv2a.h"
typedef struct ChVkBackend ChVkBackend;
typedef struct {
    VkPhysicalDevice physical; VkDevice device; VkQueue queue; uint32_t family;
    /* Optional paired host queue lock. Covers each complete callback, including
     * rendering hooks, and destruction. Hooks must not acquire it again.
     * Use when presentation and guest rendering share a VkQueue. */
    void (*lock_queue)(void *);
    void (*unlock_queue)(void *);
    void *queue_context;
    void *guest_base; size_t guest_bytes;
    ChVkDecodeTexture decode;
    /* Set only when host enabled VkPhysicalDeviceFeatures.occlusionQueryPrecise. */
    int precise_queries;
    /* Keep ARGB8888 targets resident between draws. Flush publishes guest
     * pixels at fences/flip or before texture decoding samples a dirty target.
     * RGB565 retains strict per-draw quantized writeback. Render routing hooks
     * currently require the strict path (resident_color=0). */
    int resident_color;
    int cache_textures; /* exact level-zero storage/palette/sampler cache */
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
    /* Completed flat guest image at flip, including intervening CPU writes.
     * Tightly packed RGBA8, top-left origin, straight guest alpha. Borrowed
     * until return: copy into a host-owned frame for another thread. No Vulkan
     * queue/target ownership is transferred. Nonzero unknown VA is an error.
     * Called before render_flip; zero presented VA produces no image. */
    VkResult (*present_rgba)(void *,const Nv2aSurface *,const void *,size_t);
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
#endif
