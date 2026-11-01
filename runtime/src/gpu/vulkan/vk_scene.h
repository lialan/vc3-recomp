#ifndef CH_VK_SCENE_H
#define CH_VK_SCENE_H
#include "vk_nv2a.h"
#include "../../host/vr_view.h"
typedef struct ChVkScene ChVkScene;
typedef struct {
    VkPhysicalDevice physical; VkDevice device; VkQueue queue; uint32_t family;
    uint32_t eye_width,eye_height,hud_width,hud_height;
    ChVkDecodeTexture decode;
} ChVkSceneConfig;
typedef struct {
    int immersive,hud;
    Vc3VrView view;
    VrGameProj projection;
    ChVkTarget *eyes[2],*hud_target;
    uint32_t guest_width,guest_height,world_batches;
} ChVkSceneFrame;
/* Serialized submission-thread world/HUD renderer. Guest rendering/writeback
 * must occur first. Host owns queue/device and frame pacing; this module owns
 * only extra eye/HUD targets and per-surface projection provenance.
 * Screen-shaped render targets sample native per-eye snapshots. Exact linear
 * ARGB8888/RGB565 bindings supported; GPU-owned aliases/reinterpretation fail.
 * Other textures retain the reference guest decoder. HUD samples left eye.
 * Any rendering error requires the host to stop; never submit a partial frame. */
VkResult ch_vk_scene_create(const ChVkSceneConfig *,ChVkScene **);
void ch_vk_scene_destroy(ChVkScene *);
/* Once per guest frame, after previous end. shown_va is host's latched previous
 * presented surface (no game-specific address). View copied, not retained. */
VkResult ch_vk_scene_begin(ChVkScene *,const Vc3VrView *,uint32_t shown_va);
VkResult ch_vk_scene_observe(ChVkScene *,const Nv2aSurface *,const Nv2aBatch *);
/* Exact CPU edit notification before guest draw. Copies RGBA immediately,
 * seeds/preserves native target backgrounds and marks edited sources CPU-owned
 * until a subsequent host clear/draw. CPU-owned sources use guest decoding;
 * a CPU-owned presented surface stays flat, as in the reference host. */
VkResult ch_vk_scene_import(ChVkScene *,const Nv2aSurface *,const void *,size_t,int initial);
VkResult ch_vk_scene_clear(ChVkScene *,const Nv2aSurface *,const Nv2aRenderState *,uint32_t,uint32_t,uint32_t);
VkResult ch_vk_scene_draw(ChVkScene *,const Nv2aSurface *,const Nv2aBatch *);
/* Selects next frame's projection from screen surface with most 3D batches,
 * following reference frame ownership. Returned targets are borrowed until
 * next begin/draw/destroy; host must copy images before crossing threads. */
VkResult ch_vk_scene_end(ChVkScene *,uint32_t presented_va,ChVkSceneFrame *);
#endif
