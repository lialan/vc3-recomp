/* Texture decode on the GPU, and the counters that measure texture uploads.
 * See gl_texdec.c. Called from gl_backend.c on the executor (GL) thread. */
#ifndef VC3_GL_TEXDEC_H
#define VC3_GL_TEXDEC_H

#include <stddef.h>
#include <stdint.h>
#include "nv2a_backend.h"

/* VC3_TEX_STATS=1: per format, the uploads, texels, and the CPU time spent
 * decoding and handing them to GL; printed every 300 flips (cumulative). */
extern int g_texstat_on;
uint64_t texstat_now(void);
/* One upload of format `fmt`: `texels` decoded (all levels), `dec_ns` in the
 * decode (CPU decode, or the GPU path's raw copy and pass), `gl_ns` in the
 * GL calls that take the result (glTexImage2D, glGenerateMipmap). `path`:
 * 0 CPU decode, 1 GPU decode, 2 native compressed upload. */
void texstat_add(uint32_t fmt, int path, uint64_t texels, uint64_t dec_ns, uint64_t gl_ns);
void texstat_hash(uint64_t ns);
void texstat_flip(uint32_t frame);
void texstat_init(void);

/* ── GPU decode (VC3_TEX_GPU, default on) ── */

/* The storage texdec_upload gave a GL texture; zero for a new name. An
 * immutable one (ES compute: glTexStorage2D) can never change shape, so the
 * caller deletes the name and makes a fresh one before reusing it for a
 * texture of another size. */
typedef struct {
    uint32_t w, h, levels;
    int immut;
} TexdecAlloc;

/* 0: CPU decode (VC3_TEX_GPU=0, or no decoder here); 1: fragment pass;
 * 2: compute. Needs the GL context current. */
int texdec_mode(void);
/* The texture is one the GPU path decodes (format, palette, size). */
int texdec_can(const Nv2aTexture *t);
/* Decode t from guest memory into `tex`, which is bound to GL_TEXTURE_2D on
 * the active unit; `mips`: give it a mip chain (the guest's levels, else
 * glGenerateMipmap), *mipped says whether it got one. Returns 0 if not done
 * (the caller decodes on the CPU), 1 decoded on the GPU, 2 uploaded
 * compressed. With VC3_TEX_COMPARE=1 also checks it against the CPU decode. */
int texdec_upload(unsigned int tex, TexdecAlloc *a, const Nv2aTexture *t, int mips,
                  int *mipped);

#endif
