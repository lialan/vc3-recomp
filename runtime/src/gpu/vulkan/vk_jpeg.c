/* vk_jpeg.c - a JPEG in memory to ARGB pixels (vk_jpeg.h), for the VR
 * rifle's texture: stb_image (third_party/stb, public domain or MIT), its
 * JPEG decoder only, all of it static to this file. */
#include "vk_jpeg.h"
#include <limits.h>
#include <stdlib.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#include "../../../../third_party/stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

uint32_t *ch_jpeg_argb(const void *jpeg, size_t bytes, uint32_t *w, uint32_t *h)
{
    int x = 0, y = 0, n = 0;
    uint32_t *out = NULL;
    *w = *h = 0;
    if (!jpeg || !bytes || bytes > INT_MAX)
        return NULL;
    unsigned char *rgb = stbi_load_from_memory(jpeg, (int)bytes, &x, &y, &n, 3);
    if (!rgb)
        return NULL;
    if (x > 0 && y > 0 && (size_t)x <= SIZE_MAX / 4 / (size_t)y)
        out = malloc((size_t)x * (size_t)y * 4);
    if (out) {
        for (size_t i = 0; i < (size_t)x * (size_t)y; i++)
            out[i] = 0xff000000u | (uint32_t)rgb[i * 3] << 16 | (uint32_t)rgb[i * 3 + 1] << 8 | rgb[i * 3 + 2];
        *w = (uint32_t)x;
        *h = (uint32_t)y;
    }
    stbi_image_free(rgb);
    return out;
}
