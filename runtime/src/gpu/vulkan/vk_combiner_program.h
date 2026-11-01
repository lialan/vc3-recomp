#ifndef CH_VK_COMBINER_PROGRAM_H
#define CH_VK_COMBINER_PROGRAM_H
#include "nv2a_backend.h"
#include <string.h>
/* Instruction words only. Factors/final constants stay in the per-draw SSBO.
 * The generated GLSL constants and C offset assertions share this layout. */
typedef struct {
    uint32_t color_icw[8];
    uint32_t alpha_icw[8];
    uint32_t color_ocw[8];
    uint32_t alpha_ocw[8];
    uint32_t final0;
    uint32_t final1;
    uint32_t control;
    uint32_t stage_program;
} ChVkCombinerProgram;
static inline void ch_vk_combiner_program(const Nv2aCombiner *rc,ChVkCombinerProgram *p) {
    memset(p,0,sizeof *p);
    unsigned n=rc->control&255;if(n>8) n=8;
    memcpy(p->color_icw,rc->color_icw,n*4);memcpy(p->alpha_icw,rc->alpha_icw,n*4);
    memcpy(p->color_ocw,rc->color_ocw,n*4);memcpy(p->alpha_ocw,rc->alpha_ocw,n*4);
    p->final0=rc->final0;p->final1=rc->final1;
    /* Only these flag bits are consumed by ch_rc_stage. Clamp the count
     * exactly as ch_rc_eval does; reserved bits cannot change the program. */
    p->control=n|(rc->control&0x11100u);p->stage_program=rc->stage_program;
}
#endif
