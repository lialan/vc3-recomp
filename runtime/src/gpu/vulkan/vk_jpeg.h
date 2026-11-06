#ifndef CH_VK_JPEG_H
#define CH_VK_JPEG_H
#include <stddef.h>
#include <stdint.h>
/* A JPEG in memory to 0xAARRGGBB pixels, rows top first (malloc'd: the
 * caller frees), *w x *h; NULL if it does not decode. stb_image
 * (third_party/stb), JPEG only. */
uint32_t *ch_jpeg_argb(const void *jpeg, size_t bytes, uint32_t *w, uint32_t *h);
#endif
