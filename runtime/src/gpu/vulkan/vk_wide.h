#ifndef CH_VK_WIDE_H
#define CH_VK_WIDE_H
/* Flat 16:9 (hor+): the game's 4:3 picture unchanged, the screen wider.
 *
 * Each screen-shaped guest surface (4:3) gets a wide copy, as tall as the
 * surface and 16:9 wide, that every draw and clear into the surface also
 * goes to. In it the game's picture lands 1:1 in the middle (clip x times
 * guest width / wide width: immersive.glsl mode 3) and the 3D view carries
 * on into the sides, as far as the game's own culling lets it. 2D across
 * the whole width that is a flat colour (fades, letterbox bars), and
 * full-screen 2D that shows another screen-shaped surface (a composite of a
 * view, sampled from its wide copy) or is drawn during play (a sky, a
 * flash), spans the wide picture edge to edge instead; a full-screen
 * picture outside play (a menu background, a movie) stays 4:3 in the
 * middle, with whatever the sides were cleared to.
 *
 * The guest surfaces are drawn as before (the game reads and samples them);
 * the wide copies are host-only. The presented surface's wide copy is what
 * the host shows. Same machinery as the immersive eyes (vk_immersive.c):
 * one more target per surface, the same draw path, one instance. */
#include "vk_nv2a.h"
#include "vk_fixed.h"
#include "vk_texture_cache.h"
typedef struct ChVkWide ChVkWide;
typedef struct {
    VkPhysicalDevice physical; VkDevice device; VkQueue queue; uint32_t family;
    VkPipelineCache pipeline_cache;
    uint32_t batch_limit;
    ChVkDecodeTexture decode;
    ChVkTextureCache *textures;   /* wide copies sampled by stretched composites (NULL: lent only) */
} ChVkWideConfig;
VkResult ch_vk_wide_create(const ChVkWideConfig *,ChVkWide **);
void ch_vk_wide_destroy(ChVkWide *);
/* s: the guest surface as drawn (the target's extent). Not screen-shaped:
 * nothing. Draws without prepared GPU geometry are not mirrored (counted). */
VkResult ch_vk_wide_clear(ChVkWide *,const Nv2aSurface *s,const Nv2aRenderState *,uint32_t flags,uint32_t argb,uint32_t zs);
VkResult ch_vk_wide_draw(ChVkWide *,const Nv2aSurface *s,const Nv2aBatch *,const ChVkFixedGeometry *,
    ChVkSampleResolver,void *context);
/* The flip: the presented surface's wide picture, read back (RGBA, owned by
 * the module until the next call); VK_NOT_READY when it has none. */
VkResult ch_vk_wide_flip(ChVkWide *,uint32_t presented,const void **rgba,uint32_t *width,uint32_t *height);
#endif
