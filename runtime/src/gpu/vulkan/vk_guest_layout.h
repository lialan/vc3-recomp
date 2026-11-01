#ifndef CH_VK_GUEST_LAYOUT_H
#define CH_VK_GUEST_LAYOUT_H
#include <vulkan/vulkan.h>
#include <stdint.h>
#include "../d3d/d3d8_swizzle.h"

typedef struct {
    uint32_t pitch,bytes_per_pixel,mask_x,mask_y;
    int swizzled;
    uint64_t span;
} ChVkGuestLayout;

static inline uint64_t ch_vk_guest_offset(const ChVkGuestLayout *l,uint32_t x,uint32_t y)
{
    if(l->swizzled)
        return (uint64_t)(swizzle_deposit(x,l->mask_x)|swizzle_deposit(y,l->mask_y))*l->bytes_per_pixel;
    return (uint64_t)y*l->pitch+(uint64_t)x*l->bytes_per_pixel;
}

static inline VkResult ch_vk_guest_layout(uint32_t format,uint32_t width,uint32_t height,
    uint32_t pitch,uint32_t bpp,ChVkGuestLayout *out)
{
    if(!out || !width || !height || (bpp!=2 && bpp!=4)) return VK_ERROR_INITIALIZATION_FAILED;
    ChVkGuestLayout l={.pitch=pitch,.bytes_per_pixel=bpp};
    uint32_t type=(format>>8)&15;
    if(type==2) {
        uint32_t wx=(format>>16)&255,hy=format>>24;
        if(wx>15 || hy>15 || width>(1u<<wx) || height>(1u<<hy)) return VK_ERROR_INITIALIZATION_FAILED;
        l.swizzled=1;
        xbox_swizzle_masks(1u<<wx,1u<<hy,&l.mask_x,&l.mask_y);
    } else if(type!=0 && type!=1) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    } else if((uint64_t)width*bpp>pitch) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    l.span=ch_vk_guest_offset(&l,width-1,height-1)+bpp;
    *out=l;
    return VK_SUCCESS;
}
#endif
