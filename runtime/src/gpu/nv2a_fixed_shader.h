#ifndef CH_NV2A_FIXED_SHADER_H
#define CH_NV2A_FIXED_SHADER_H
/* Fixed-function arithmetic shared by the reference GL and native Vulkan
 * vertex shaders. Callers supply attribute/uniform globals and the output
 * varyings; API-specific clipping/presentation remains in their epilogues. */
#define CH_NV_FIXED_SHADER_FUNCTIONS \
vec4 fetch(int r, vec4 a) { \
  if (((u_packed >> r) & 1) != 0) { \
    int v = int(uint(a.x) | (uint(a.y) << 16)); \
    return vec4(float(bitfieldExtract(v, 0, 11)) / 1023.0, \
                float(bitfieldExtract(v, 11, 11)) / 1023.0, \
                float(bitfieldExtract(v, 22, 10)) / 511.0, 1.0); \
  } \
  if (((u_bgra >> r) & 1) != 0) a = a.zyxw; \
  return a * u_ascale[r]; \
} \
float fog_factor(float d) { \
  if (u_fogen == 0) return 1.0; \
  float f, px = u_fogp.x, py = u_fogp.y; \
  if (u_fogmode == 0x800u || u_fogmode == 0x802u) f = px + exp2(d * py * 16.0) - 1.5; \
  else if (u_fogmode == 0x801u || u_fogmode == 0x803u) f = px + exp2(-d * d * py * py * 32.0) - 1.5; \
  else f = px + d * py - 1.0; \
  if (u_fogmode == 0x802u || u_fogmode == 0x803u || u_fogmode == 0x804u) f = abs(f); \
  if (ch_nv_fixed_isnan(f)) f = 1.0; \
  return clamp(f, 0.0, 1.0); \
} \
vec4 lit_color(vec4 pos, vec4 nrm, vec4 vc) { \
  vec3 e = (u_mv[0] * vec4(pos.xyz, 1.0)).xyz; \
  vec3 n = mat3(u_mv[0]) * nrm.xyz; \
  float len = length(n); \
  if (len > 1e-8) n /= len; \
  if (u_flipn != 0 && dot(n, e) > 0.0) n = -n; \
  bool hv = u_hascol != 0; \
  vec3 o = u_samb.rgb; \
  for (int i = 0; i < 8; i++) { \
    int kind = u_lkind[i]; \
    if (kind == 0) continue; \
    vec3 l; float att = 1.0; \
    if (kind == 1) l = u_light[i * 5 + 2].xyz; \
    else { \
      l = u_light[i * 5 + 3].xyz - e; \
      float d = length(l), range = u_light[i * 5].w; \
      if (d > range && range > 0.0) continue; \
      if (d > 1e-8) l /= d; \
      vec3 a = u_light[i * 5 + 4].xyz; \
      att = a.x + a.y * d + a.z * d * d; \
      att = att > 1e-8 ? 1.0 / att : 1.0; \
    } \
    float ndl = dot(n, l); \
    if (ndl < 0.0) ndl = 0.0; \
    vec3 amb = u_light[i * 5].rgb, dif = u_light[i * 5 + 1].rgb; \
    if (hv && u_colmat.x != 0) amb *= vc.rgb; \
    if (hv && u_colmat.y != 0) dif *= vc.rgb; \
    o += att * (amb + dif * ndl); \
  } \
  return clamp(vec4(o, (hv && u_colmat.y != 0) ? vc.a : u_matalpha), 0.0, 1.0); \
} \
vec4 texgen(uvec4 mode, vec3 e, vec3 n, vec4 tc) { \
  float len = length(e); \
  vec3 u = len > 1e-8 ? e / len : vec3(0.0); \
  float d = dot(n, u); \
  vec3 r = u - 2.0 * d * n; \
  float m = 2.0 * sqrt(r.x * r.x + r.y * r.y + (r.z + 1.0) * (r.z + 1.0)); \
  for (int c = 0; c < 3; c++) { \
    if (mode[c] == 0x8511u) tc[c] = n[c]; \
    else if (mode[c] == 0x8512u) tc[c] = r[c]; \
    else if (mode[c] == 0x2402u && c < 2 && m > 1e-8) tc[c] = r[c] / m + 0.5; \
  } \
  if (mode.w == 0u) tc.w = 1.0; \
  return tc; \
} \
void ch_nv_fixed_vertex() { \
  vec4 pos = fetch(0, a_pos), wt = fetch(1, a_wt), nrm = fetch(2, a_nrm); \
  vec4 col = fetch(3, a_col); \
  vec4 t[4]; \
  t[0] = fetch(4, a_t0); t[1] = fetch(5, a_t1); t[2] = fetch(6, a_t2); t[3] = fetch(7, a_t3); \
  vec4 ww = vec4(1.0, 0.0, 0.0, 0.0); \
  int nm = 1; \
  if (u_skin != 0u) { \
    nm = u_skin <= 2u ? 2 : u_skin <= 4u ? 3 : 4; \
    for (int k = 0; k < 4; k++) ww[k] = k < nm ? wt[k] : 0.0; \
    if ((u_skin & 1u) != 0u) { \
      float last = 1.0; \
      for (int k = 0; k < nm - 1; k++) last -= wt[k]; \
      ww[nm - 1] = last; \
    } \
  } \
  vec4 eye = vec4(0.0); \
  vec3 en = vec3(0.0); \
  for (int k = 0; k < nm; k++) { \
    eye += ww[k] * (u_mv[k] * pos); \
    en += ww[k] * (mat3(u_mv[k]) * nrm.xyz); \
  } \
  vec4 clip = u_usecomp != 0 ? u_comp * pos : u_proj * eye; \
  float w = clip.w; \
  vec3 s = vec3((clip.xy / w + u_c59.xy) * u_aa, clip.z / w + u_c59.z); \
  vec4 d0 = u_lighting != 0 ? lit_color(pos, nrm, col) \
          : (u_hascol != 0 ? col : vec4(1.0)); \
  d0 = clamp(d0, 0.0, 1.0); \
  float elen = length(en); \
  if (elen > 1e-8) en /= elen; \
  for (int st = 0; st < 4; st++) { \
    if (u_tgon[st] != 0) t[st] = texgen(u_texgen[st], eye.xyz, en, t[st]); \
    if (u_tmon[st] != 0) t[st] = u_tm[st] * t[st]; \
  } \
  o_screen = vec4(s, w); \
  v_col = d0; \
  v_spec = vec4(0.0); \
  v_fog = fog_factor(0.0); \
  v_tc0 = t[0]; v_tc1 = t[1]; v_tc2 = t[2]; v_tc3 = t[3]; \
  v_uv = t[0].xy * u_uvs0 / u_texsize; \
} \

#ifdef CH_NV_SHADER_GLSL
bool ch_nv_fixed_isnan(float value) { return isnan(value); }
#else
#define CH_NV_FIXED_TEXT_INNER(...) #__VA_ARGS__
#define CH_NV_FIXED_TEXT(...) CH_NV_FIXED_TEXT_INNER(__VA_ARGS__)
#define CH_NV_FIXED_SHADER_SOURCE "bool ch_nv_fixed_isnan(float value) { return isnan(value); }\n" CH_NV_FIXED_TEXT(CH_NV_FIXED_SHADER_FUNCTIONS)
#endif
#endif
