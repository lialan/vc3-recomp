#ifndef CH_VK_INTERP_H
#define CH_VK_INTERP_H
/* Display frame interpolation on the GPU: a frame between two real ones,
 * for a title that makes fewer frames than the display shows (Silent Scope
 * presents every other vblank: 30 fps on a 60 Hz display). The OpenGL
 * renderer's interpolator (gl_backend.c) in compute: symmetric block
 * matching per 8x8 block, coarse to fine; per pixel the best of the
 * neighbouring vectors, trusted as far as its colour error allows; pixels
 * equal in both frames copied exactly (a still HUD stays as drawn); a scene
 * cut gives the nearer real frame. Works on host pixels (the frames a CPU
 * presenter already has), synchronously: upload, five passes, read back.
 * The queue must not be in use on another thread while it runs. */
#include <stdint.h>
#include <vulkan/vulkan.h>
typedef struct ChVkInterp ChVkInterp;
VkResult ch_vk_interp_create(VkPhysicalDevice,VkDevice,VkQueue,uint32_t family,VkPipelineCache,ChVkInterp **);
void ch_vk_interp_destroy(ChVkInterp *);
/* a, b: the earlier and later frame, RGBA8 w x h, tightly packed; out: the
 * frame at t (0 < t < 1) between them, the same layout. */
VkResult ch_vk_interp_make(ChVkInterp *,const void *a,const void *b,uint32_t w,uint32_t h,float t,void *out);
#endif
