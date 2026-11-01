#include "vk_menu.h"
#include <stdlib.h>
#include <string.h>
struct ChVkMenu { ChVkTarget *target; unsigned char *rgba; };
#define MENU_BYTES ((size_t)XM_W*XM_H*4)
void ch_vk_menu_destroy(ChVkMenu *menu) {
    if(!menu) return;
    ch_vk_target_destroy(menu->target); free(menu->rgba); free(menu);
}
ChVkTarget *ch_vk_menu_target(ChVkMenu *menu) { return menu?menu->target:NULL; }
VkResult ch_vk_menu_create(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,uint32_t family,ChVkMenu **out) {
    if(!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out=NULL;
    ChVkMenu *menu=calloc(1,sizeof(*menu)); if(!menu) return VK_ERROR_OUT_OF_HOST_MEMORY;
    menu->rgba=malloc(MENU_BYTES);
    if(!menu->rgba) { free(menu); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    VkResult r=ch_vk_target_create(gpu,device,queue,family,XM_W,XM_H,&menu->target);
    if(r==VK_SUCCESS) {
        const float transparent[]={0,0,0,0};
        r=ch_vk_target_clear_read(menu->target,transparent,menu->rgba,MENU_BYTES);
    }
    if(r!=VK_SUCCESS) { ch_vk_menu_destroy(menu); return r; }
    *out=menu; return VK_SUCCESS;
}
VkResult ch_vk_menu_paint(ChVkMenu *menu,const char *title,const XmItem *items,int count,int hover,const char *hint) {
    if(!menu || !title || !hint || count<0 || count>XM_ROWS || (count && !items) || hover< -1 || hover>=count)
        return VK_ERROR_INITIALIZATION_FAILED;
    for(int i=0;i<count;i++) if(!items[i].label || !memchr(items[i].value,0,sizeof(items[i].value)))
        return VK_ERROR_INITIALIZATION_FAILED;
    xm_paint(menu->rgba,title,items,count,hover,hint);
    return ch_vk_target_import_color(menu->target,menu->rgba,MENU_BYTES);
}
