#ifndef CH_VK_GUEST_LAYOUT_H
#define CH_VK_GUEST_LAYOUT_H
#include <vulkan/vulkan.h>
#include <stdint.h>
#include "../d3d/d3d8_swizzle.h"

typedef struct {
    uint32_t pitch,bytes_per_pixel;
    uint32_t interleave_mask,interleave_bits;
    int tail_x;
    int swizzled;
    uint64_t span;
} ChVkGuestLayout;

static inline uint64_t ch_vk_guest_offset(const ChVkGuestLayout *l,uint32_t x,uint32_t y)
{
    if(l->swizzled) {
        /* Power-of-two rectangular allocations alternate X/Y bits until
         * the smaller dimension is exhausted, then append the larger one's
         * remaining bits. Unlike masking spread(y) directly with mask_y,
         * shifting its spread by one preserves the odd-position Y bits.
         * Layout validation caps both exponents at 15, so every shift fits. */
        uint32_t low=swizzle_spread(x&l->interleave_mask)|
            (swizzle_spread(y&l->interleave_mask)<<1);
        uint32_t tail=(l->tail_x?x:y)>>l->interleave_bits;
        return (uint64_t)(low|(tail<<(2*l->interleave_bits)))*l->bytes_per_pixel;
    }
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
        l.interleave_bits=wx<hy?wx:hy;
        l.interleave_mask=(1u<<l.interleave_bits)-1;
        l.tail_x=wx>hy;
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
