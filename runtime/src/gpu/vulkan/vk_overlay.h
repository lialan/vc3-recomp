#ifndef CH_VK_OVERLAY_H
#define CH_VK_OVERLAY_H
#include "vk_target.h"
#include "../../xr/quest_overlay.h"
/* Rigid handgun mesh including sight housings; separate from glass/reticle,
 * laser/dot/flash passes (not implemented here). Reuses reference mesh and
 * controller pose/recoil/slide transforms. Target color preserved, depth cleared
 * as reference qo_draw gun pass. Output gun pixels are opaque. */
VkResult ch_vk_overlay_gun_read(ChVkTarget *,const QoFrame *,const float gl_viewproj[16],
    void *pixels,VkDeviceSize bytes);
/* Complete reference overlay passes. sight_height comes from the game's profile
 * (qo_sight_height); target already contains the scene or transparent clear.
 * No input polling, compositor submission, or frame clearing is performed. */
VkResult ch_vk_overlay_read(ChVkTarget *,const QoFrame *,const float gl_viewproj[16],
    const float eye[3],float sight_height,void *pixels,VkDeviceSize bytes);
/* The weapon's (QO_WEAPON_*) model loaded now, on target's device, not at its
 * first frame: the rifle's mesh unpacked, its texture decoded and uploaded
 * (else the box rifle, said once); the shotgun's meshes and texture likewise
 * (else the handgun). The handgun has nothing to load. */
void ch_vk_overlay_prepare(ChVkTarget *,int weapon);
#endif
