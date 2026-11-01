#ifndef CH_NV2A_STAGE_FETCH_H
#define CH_NV2A_STAGE_FETCH_H
/* Shared reference fragment-stage contribution. Modes above PASSTHRU retain
 * the existing GL zero result; this does not implement NV2A bump/dot effects. */
#define CH_NV_STAGE_FETCH_FUNCTION \
vec4 ch_nv_stage_fetch(uint mode,vec4 tc,vec4 texel,uint enabled) { \
    if(mode==0u) return vec4(0.0,0.0,0.0,1.0); \
    if(mode==4u) return clamp(tc,0.0,1.0); \
    if(mode<=3u) return enabled!=0u?texel:vec4(1.0); \
    return vec4(0.0); \
}
#ifndef CH_NV_SHADER_GLSL
#define CH_NV_STAGE_TEXT_INNER(value) #value
#define CH_NV_STAGE_TEXT(value) CH_NV_STAGE_TEXT_INNER(value)
#define CH_NV_STAGE_FETCH_SOURCE CH_NV_STAGE_TEXT(CH_NV_STAGE_FETCH_FUNCTION)
#endif
#endif
