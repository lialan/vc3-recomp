#ifndef CH_VK_MENU_H
#define CH_VK_MENU_H
#include "vk_target.h"
#include "../../xr/quest_menu.h"
typedef struct ChVkMenu ChVkMenu;
/* Reuses the reference CPU painter/hit-testing, without GL handles. Device
 * lifetime and queue serialization owned by host. Menu owns target/CPU pixels. */
VkResult ch_vk_menu_create(VkPhysicalDevice,VkDevice,VkQueue,uint32_t family,ChVkMenu **);
VkResult ch_vk_menu_paint(ChVkMenu *,const char *title,const XmItem *items,
    int count,int hover,const char *hint);
/* XM_W x XM_H straight-alpha RGBA8. For alpha-blended XR quad presentation
 * specify both BLEND_TEXTURE_SOURCE_ALPHA and UNPREMULTIPLIED_ALPHA flags. */
ChVkTarget *ch_vk_menu_target(ChVkMenu *);
void ch_vk_menu_destroy(ChVkMenu *);
#endif
