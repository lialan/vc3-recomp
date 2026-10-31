/* Texture decode on the GPU, and the counters that measure texture uploads.
 * See gl_texdec.c. Called from gl_backend.c on the executor (GL) thread. */
#ifndef VC3_GL_TEXDEC_H
#define VC3_GL_TEXDEC_H

#include <stddef.h>
#include <stdint.h>

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

#endif
