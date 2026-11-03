#ifndef CH_VK_IMMERSIVE_H
#define CH_VK_IMMERSIVE_H
/* Immersive VR on the GPU: the game's 3D around the player, each eye drawn
 * from the head's pose -- the OpenGL renderer's immersive view, in Vulkan.
 *
 * Every 3D vertex's game-camera position is recovered from the screen
 * position and w the game's transform produced, through the game's own
 * projection (read from the fixed-function matrices: vr_math.h), then drawn
 * through each eye's matrix (immersive.glsl). Both eyes go side by side
 * into one target per screen-shaped surface, in one draw (two instances).
 * The game's 2D that does not fill the shown surface (score, ammo, marks)
 * goes to a separate HUD target the host hangs at the game's view; other 2D
 * (fades, full-screen effects) is drawn flat into each eye.
 *
 * The guest surfaces keep being drawn as before: the game reads and samples
 * them. The eye/HUD targets are host-only and never reach guest memory.
 * Frames without 3D (movies, menus) stay flat, shown on the screen. */
#include "vk_nv2a.h"
#include "vk_fixed.h"
#include "vk_texture_cache.h"
#include "../../host/vr_view.h"
typedef struct ChVkImmersive ChVkImmersive;
typedef struct {
    Vc3VrView view;            /* the head pose the frame was drawn with */
    VrGameProj projection;     /* the frame's dominant game projection, */
    int projection_ok;         /* zoom-normalised, in surface pixels */
    uint32_t guest_width,guest_height;   /* the surface the scene went into */
    int hud;                   /* the HUD target holds this frame's HUD */
    uint32_t eye_width,eye_height;       /* each eye; the target is 2x wide */
    float scale;    /* dynamic resolution: the fraction of each eye's half drawn
                       (left eye from the top-left, right eye from the top-right) */
} ChVkImmFrame;
typedef struct {
    VkPhysicalDevice physical; VkDevice device; VkQueue queue; uint32_t family;
    VkPipelineCache pipeline_cache;
    uint32_t eye_width,eye_height;
    uint32_t batch_limit;
    ChVkDecodeTexture decode;
    ChVkTextureCache *textures;   /* eye images sampled by flat draws (NULL: guest pixels) */
    /* Diagnostics (CHIHIRO_VULKAN_IMMERSIVE_DUMP=dir): a guest surface's target. */
    ChVkTarget *(*guest_target)(void *,uint32_t va);
    void *guest_context;
} ChVkImmersiveConfig;
VkResult ch_vk_immersive_create(const ChVkImmersiveConfig *,ChVkImmersive **);
void ch_vk_immersive_destroy(ChVkImmersive *);
/* Is this frame drawn immersive (decided at the previous flip)? */
int ch_vk_immersive_on(const ChVkImmersive *);
/* After the guest's own clear/draw of the same surface. Batches the GPU
 * transforms (fixed == NULL: CPU-transformed) are not mirrored. */
VkResult ch_vk_immersive_clear(ChVkImmersive *,const Nv2aSurface *,const Nv2aRenderState *,
    uint32_t flags,uint32_t argb,uint32_t zs);
VkResult ch_vk_immersive_draw(ChVkImmersive *,const Nv2aSurface *,const Nv2aBatch *,
    const ChVkFixedGeometry *fixed,ChVkSampleResolver,void *resolver_context);
/* With the guest's screen-shaped draws skipped (only the eyes are drawn),
 * a guest draw into another target that samples such a surface -- an effect
 * map made from the scene -- samples its eyes (the left one) instead. Returns
 * 1 and draws when it applies, 0 to draw as usual. */
int ch_vk_immersive_guest_draw(ChVkImmersive *,ChVkTarget *,const Nv2aSurface *,const Nv2aBatch *,
    const ChVkFixedGeometry *,ChVkSampleResolver,void *,void *pixels,VkDeviceSize bytes,VkResult *);
/* At the flip, after the guest's: the frame's dominant projection; whether
 * the presented surface is shown immersive (eyes/hud set, borrowed until the
 * next draw); then the next frame's view (NULL or !on: flat). */
VkResult ch_vk_immersive_flip(ChVkImmersive *,uint32_t presented,const Vc3VrView *next,
    ChVkImmFrame *out,ChVkTarget **eyes,ChVkTarget **hud);
#endif
