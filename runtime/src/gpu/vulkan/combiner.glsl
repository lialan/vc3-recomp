// Register-combiner arithmetic ported from the shared reference GL shader.
// Texture stage fetch/coordinate generation stays with the caller. Inputs here
// are the four stage results, matching nv2a_rc_eval (not raw sampler results).
#ifndef CH_RC_UNROLL
#define CH_RC_UNROLL
#endif
struct ChRc {
    uint color_icw[8]; uint alpha_icw[8]; uint color_ocw[8]; uint alpha_ocw[8];
    uint factor0[8]; uint factor1[8];
    uint final0; uint final1; uint final_c0; uint final_c1; uint control; uint stage_program;
};
vec4 rc_unpack(uint c) { return vec4((c>>16)&255u,(c>>8)&255u,c&255u,c>>24)/255.0; }
vec4 R[16];
float map_in(float x, uint b) {
  float p = max(x, 0.0);
  uint m = (b >> 5) & 7u;
  if (m == 0u) return p;
  if (m == 1u) return 1.0 - clamp(x, 0.0, 1.0);
  if (m == 2u) return 2.0 * p - 1.0;
  if (m == 3u) return -2.0 * p + 1.0;
  if (m == 4u) return p - 0.5;
  if (m == 5u) return -p + 0.5;
  if (m == 6u) return x;
  return -x;
}
float map_out(float x, uint m) {
  if (m == 0x08u) return x - 0.5;
  if (m == 0x10u) return x * 2.0;
  if (m == 0x18u) return (x - 0.5) * 2.0;
  if (m == 0x20u) return x * 4.0;
  if (m == 0x30u) return x * 0.5;
  return x;
}
vec3 rd_rgb(uint b) {
  vec4 r = R[b & 15u];
  vec3 v = ((b & 0x10u) != 0u) ? vec3(r.a) : r.rgb;
  return vec3(map_in(v.x, b), map_in(v.y, b), map_in(v.z, b));
}
float rd_a(uint b) {
  vec4 r = R[b & 15u];
  return map_in(((b & 0x10u) != 0u) ? r.a : r.b, b);
}
void ch_rc_stage(ChRc rc,uint s,uint flags) {
    uint icw = rc.color_icw[s], ocw = rc.color_ocw[s], aicw = rc.alpha_icw[s], aocw = rc.alpha_ocw[s];
    uint fl = ocw >> 12, afl = aocw >> 12;
    R[1] = rc_unpack(rc.factor0[(flags & 0x010u) != 0u ? s : 0u]);
    R[2] = rc_unpack(rc.factor1[(flags & 0x100u) != 0u ? s : 0u]);
    bool mux_cd = (flags & 1u) != 0u ? R[12].a >= 0.5
                                      : ((int(R[12].a * 255.0) & 1) != 0);
    vec3 A = rd_rgb(icw >> 24), B = rd_rgb(icw >> 16);
    vec3 C = rd_rgb(icw >> 8),  D = rd_rgb(icw);
    float aA = rd_a(aicw >> 24), aB = rd_a(aicw >> 16);
    float aC = rd_a(aicw >> 8),  aD = rd_a(aicw);
    vec3 pab = ((fl & 2u) != 0u) ? vec3(dot(A, B)) : A * B;
    vec3 pcd = ((fl & 1u) != 0u) ? vec3(dot(C, D)) : C * D;
    vec3 sum = ((fl & 4u) != 0u) ? (mux_cd ? pcd : pab) : pab + pcd;
    uint om = fl & 0x38u;
    vec3 ab = clamp(vec3(map_out(pab.x, om), map_out(pab.y, om), map_out(pab.z, om)), -1.0, 1.0);
    vec3 cd = clamp(vec3(map_out(pcd.x, om), map_out(pcd.y, om), map_out(pcd.z, om)), -1.0, 1.0);
    vec3 ms = clamp(vec3(map_out(sum.x, om), map_out(sum.y, om), map_out(sum.z, om)), -1.0, 1.0);
    float apab = aA * aB, apcd = aC * aD;
    float asum = ((afl & 4u) != 0u) ? (mux_cd ? apcd : apab) : apab + apcd;
    uint aom = afl & 0x38u;
    float aab = clamp(map_out(apab, aom), -1.0, 1.0);
    float acd = clamp(map_out(apcd, aom), -1.0, 1.0);
    float ams = clamp(map_out(asum, aom), -1.0, 1.0);
    uint d;
    d = (ocw >> 4) & 15u;
    if (d != 0u) { R[d].rgb = ab; if ((fl & 0x80u) != 0u) R[d].a = ab.z; }
    d = ocw & 15u;
    if (d != 0u) { R[d].rgb = cd; if ((fl & 0x40u) != 0u) R[d].a = cd.z; }
    d = (ocw >> 8) & 15u;
    if (d != 0u) R[d].rgb = ms;
    d = (aocw >> 4) & 15u; if (d != 0u) R[d].a = aab;
    d = aocw & 15u;        if (d != 0u) R[d].a = acd;
    d = (aocw >> 8) & 15u; if (d != 0u) R[d].a = ams;
    R[0] = vec4(0.0);
}
vec4 ch_rc_eval(ChRc rc, vec4 diffuse, vec4 specular, vec4 fog, vec4 texels[4]) {
  uint n = min(rc.control & 0xFFu, 8u), flags = rc.control >> 8;
#ifdef SPECIALIZED
  // Fixed array indices let specialization fold register addressing before
  // optimization. A loop can retain the interpreter's dynamic register array.
  R[0]=R[1]=R[2]=R[3]=R[4]=R[5]=R[6]=R[7]=R[8]=R[9]=R[10]=R[11]=R[12]=R[13]=R[14]=R[15]=vec4(0.0);
#else
  for (int i = 0; i < 16; i++) R[i] = vec4(0.0);
#endif
  R[3] = fog;
  R[4] = diffuse; R[5] = specular;
  R[8] = texels[0]; R[9] = texels[1]; R[10] = texels[2]; R[11] = texels[3];
  R[12].a = ((rc.stage_program & 31u) != 0u) ? texels[0].a : 1.0;
#ifdef SPECIALIZED
  if(n>0u) ch_rc_stage(rc,0u,flags);
  if(n>1u) ch_rc_stage(rc,1u,flags);
  if(n>2u) ch_rc_stage(rc,2u,flags);
  if(n>3u) ch_rc_stage(rc,3u,flags);
  if(n>4u) ch_rc_stage(rc,4u,flags);
  if(n>5u) ch_rc_stage(rc,5u,flags);
  if(n>6u) ch_rc_stage(rc,6u,flags);
  if(n>7u) ch_rc_stage(rc,7u,flags);
#else
  for(uint s=0u;s<n;s++) ch_rc_stage(rc,s,flags);
#endif
  vec4 o;
  if (rc.final0 != 0u || rc.final1 != 0u) {
    uint ff = rc.final1 & 0xFFu;
    R[1] = rc_unpack(rc.final_c0); R[2] = rc_unpack(rc.final_c1);
    vec3 a = ((ff & 0x40u) != 0u) ? 1.0 - R[5].rgb : R[5].rgb;
    vec3 b = ((ff & 0x20u) != 0u) ? 1.0 - R[12].rgb : R[12].rgb;
    R[14] = vec4(((ff & 0x80u) != 0u) ? clamp(a + b, 0.0, 1.0) : a + b, 0.0);
    vec3 E = rd_rgb(rc.final1 >> 24), F = rd_rgb(rc.final1 >> 16);
    R[15] = vec4(E * F, 0.0);
    vec3 A = rd_rgb(rc.final0 >> 24), B = rd_rgb(rc.final0 >> 16);
    vec3 C = rd_rgb(rc.final0 >> 8),  D = rd_rgb(rc.final0);
    o = vec4(D + C * (1.0 - A) + B * A, rd_a(rc.final1 >> 8));
  } else {
    o = R[12];
  }
  return clamp(o, 0.0, 1.0);
}
