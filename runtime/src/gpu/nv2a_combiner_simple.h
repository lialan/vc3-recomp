#ifndef CH_NV2A_COMBINER_SIMPLE_H
#define CH_NV2A_COMBINER_SIMPLE_H
#include "nv2a_backend.h"
/* Exact algebraic subset: texture0*diffuse into R12, optionally followed by
 * identity stages/final. 1 multiplies alpha too; 2 selects texture alpha.
 * No heuristic skips: all input mappings, output mappings and live stages are
 * checked. Unused constant/mux flags cannot affect these products. */
static inline int ch_nv2a_rc_output_product(uint32_t out)
{
    return out==0xc0 || out==0xc00 || out==0xcc0;
}
static inline int ch_nv2a_combiner_modulate(const Nv2aCombiner *rc)
{
    if(!rc || (rc->stage_program!=1 && rc->stage_program!=2)) return 0;
    unsigned stages=rc->control & 255;
    if(!stages || stages>8) return 0;
    if(rc->color_icw[0]!=0x08040000u && rc->color_icw[0]!=0x04080000u) return 0;
    int alpha=rc->alpha_icw[0]==0x18140000u || rc->alpha_icw[0]==0x14180000u?1:
        rc->alpha_icw[0]==0x18200000u || rc->alpha_icw[0]==0x20180000u?2:0;
    if(!alpha) return 0;
    for(unsigned i=0;i<stages;i++) {
        if(!ch_nv2a_rc_output_product(rc->color_ocw[i]) || !ch_nv2a_rc_output_product(rc->alpha_ocw[i])) return 0;
        if(i && ((rc->color_icw[i]!=0x0c200000u && rc->color_icw[i]!=0x200c0000u) ||
                 (rc->alpha_icw[i]!=0x1c200000u && rc->alpha_icw[i]!=0x201c0000u))) return 0;
    }
    /* The low final flags affect R14 only; this identity never reads R14. */
    if((rc->final0 || rc->final1) &&
        (rc->final0!=0x0000000cu || (rc->final1 & 0xffffff00u)!=0x00001c00u)) return 0;
    return alpha;
}
#endif
