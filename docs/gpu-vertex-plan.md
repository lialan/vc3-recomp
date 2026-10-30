# GPU vertex processing plan

Design for moving NV2A vertex work off the CPU (planning pass, 2026-10-30).

I've read the full draw path. One finding changes the order you proposed: **fixed-function batches, not vertex programs, carry nearly all of VC3's vertex work.** So Stage B should ship first. The GL-side cleanup it depends on comes before both.

## 0. Where the vertex work actually is

From the last full summary block in `run_po.log` (lines 9415–9425):
- Vertex programs: 93,696 batches but only 434,493 vertices, about 4.6 per batch. These are HUD, sprite and composite quads (attributes `v0 float4, v3 D3DCOLOR, v9..12 float2`).
- Fixed function: 97,812 batches carrying almost all of the 9.8M triangles, about 100 per batch. They use skinning modes 1/3/5, texgen 0x8511/0x8512 and texture matrices on stages 0–2.

The CPU path transforms once per index, with no vertex cache (`raster_batch_program`, `nv2a_pb_exec.c:3335`). Per vertex, the fixed-function path does this:
- `ffp_skinned_clip`: skinning with up to 4 matrices.
- `lit_color`: a second model-view transform and an 8-light loop that reads `s_reg`.
- `ffp_texgen`: calls `ffp_eye` once per texgen stage, so the skin blend is redone up to 4 more times.
- Texture-matrix multiplies.

Then each triangle is expanded into 3×(28 + 84) bytes of `Nv2aVertex`+`Nv2aVertexEx` and uploaded again.

**Estimate:** Stage B gets most of the fps gain, likely 85–95% of the vertex-side saving. Stage A matters for completeness and other titles, but for VC3 it saves roughly 2 vertices' worth of interpreter time per batch.

Before you commit, add a `[PROF]` line timing `raster_batch_program` per kind (vertex program vs fixed function) and `s_backend->draw`. That confirms the executor thread is bound by fixed-function transform rather than texture hashing (`tex_bind` → `hash_sampled` on every bind, `gl_backend.c:864`) or readback.

## 1. How a draw flows today

**State capture** (`nv2a_pb_exec_method`, line 4146):
- Every subchannel-0 method is mirrored into `s_reg[]` (line 4176).
- Vertex arrays: `SET_VERTEX_DATA_ARRAY_OFFSET` goes through `dma_resolve` into `attr[i].offset`. `_FORMAT` gives type (low nibble), size (bits 4–7, where 7 means 3) and stride (bits 8–15) (lines 4683–4699, `VertexAttr` at 245).
- Vertex programs: `0x0B00` program words, `0x0B80` constant words, `0x1E9C/1EA0/1EA4` load/start/constant-load slots, all sent to `nv2a_vsh_interp.c`.
- Some constants arrive by method:
  - the composite matrix `0x0680..` goes to c[0..3] (line 4611)
  - viewport offset/scale go to c[59]/c[58]
  - fog plane goes to c[57]
  - eye position goes to c[56]
- `TRANSFORM_EXECUTION_MODE` sets `xf_mode` (line 4585).

**Index and vertex sources**, all collected between BEGIN and END (line 4387):
- `DRAW_ARRAYS` expands start..start+count into `idx[]` (4416).
- `ARRAY_ELEMENT16/32` append indices (4436/4444).
- `INLINE_ARRAY` buffers dwords into `inline_buf` (4409). `draw_inline_array` (3789) then rewrites `attr[]` as dword-padded offsets into the payload, sets `inline_active`, and builds indices 0..n−1.
- Immediate mode `SET_VERTEX*` (3907) writes `imm_attr[16][4]`. Writing the position emits a 16×float4 vertex into `inline_buf`. `draw_immediate` (3847) describes that as float4 arrays.
- All three paths end in `draw_primitive` → `raster_batch` (3471).

**Attribute fetch:** `fetch_attr` (554) runs once per attribute read and returns (0,0,0,1) on failure. Type handling:

| Type | Format | Conversion |
|---|---|---|
| 0 | D3DCOLOR | BGRA bytes → RGBA |
| 2 | float | as is |
| 4 | ubyte | /255 |
| 1 | short | /32767 |
| 5 | short | raw |
| 6 | packed 11:11:10 | /1023, /1023, /511 |
| others | — | fail |

It also fails when offset or stride is 0, or when the span is out of bounds.

**Path selection** (3479):
- `batch_uses_program()`: `(xf_mode&3)==2` (3275).
- `batch_is_ffp()`: `composite_set && !program && !inline_active` (1751).
- Anything else goes to the screen-space heuristic path (`raster_indexed`, 2378).

**Vertex program path** (`transform_vertex`, 3284):
- An attribute with `size==0` uses its `imm_attr` current value.
- `nv2a_vsh_run` (`vsh_interp.c:332`) is straight-line from `s_start_slot` to FINAL, with no branches.
- Temps R0–R11; R12 aliases oPos; a source mux of 0 or 3 reads a constant.
- a0 relative addressing: an out-of-range index reads c[0].
- When a MAC and ILU op pair up, the ILU writes R1. Both units read their sources before either writes.
- Output registers: 0 oPos, 3 oD0, 4 oD1, 5 oFog, 9–12 oT0–3. oPts, oB0 and oB1 are dropped.
- If `cxt_write` is set, a program can write c[] (it persists across vertices).
- Defaults: D0/D1 alpha 1, T.w 1. D0/D1 are clamped to 0..1 (NaN becomes 0).
- After the run: oPos.w == +0 becomes 1, and xy are multiplied by `aa_sx/aa_sy`.
- oPos is already screen space (the D3D epilogue applies c[58]/c[59] and the rcc of w), with w left as clip w.

**Fixed-function path** (`ffp_vertex`, 2032):
- The composite is c[0..3], or `s_gpu.composite` if `RECOMP_FFP_CMAT_METHODS` is set.
- Skinning (`ffp_skinned_clip`, 1876): blend through model-view k at `0x0480+0x40k` with weights from attribute 1; modes 1/3/5 make the last weight implicit. Projection is `0x0440`, or c[0..3] if that matrix is all zero (VC3 relies on this). Mode 0 uses the composite unless `RECOMP_FFP_MVP` is set.
- Screen position = clip/w + c[59], with xy × aa.
- A vertex with |w| ≤ 1e-6 or a failed position fetch aborts the whole batch (3336).
- Colour: `lit_color` (1787), with scene ambient, 8 lights (infinite or local), attenuation, COLOR_MATERIAL and the flip-normals option; otherwise the colour stream picked by `color_attr()` (1645).
- Texture coordinates: `texcoord_attr()` (1676) for T0, attributes 10–12 for T1–3, then `ffp_texgen` (1991), then the texture matrix (2117).
- No fog coordinate (fog input is 0), and specular is 0.

**Clipping** (`raster_xf_clipped`, 3222):
- Each vertex becomes (x·w, y·w, z·w, w).
- Sutherland-Hodgman against w = 1e-3, then the near plane z·w = c59.z·w (on unless `RECOMP_CLIP_NEAR_Z=0`), divide back, fan.
- Non-finite triangles are dropped.
- `raster_xf_triangle` (3044) also drops zero-area triangles.

**Primitives** (3436): triangle lists, strips, fans/polygons, quads and quad strips are decomposed on the CPU. Strips are not winding-alternated. Points and lines are dropped.

**Batch output** (`backend_from_xf` 2288, `backend_ext_from_xf` 2259, `backend_flush` 2333). `Nv2aBatch` carries:
- a triangle list of `Nv2aVertex` {x, y, z in surface pixels; rhw = 1/w; packed d0; u, v = T0.xy × (w, h) when the stage-0 format is swizzled/DXT}
- `Nv2aVertexEx` {packed d1; `fog_factor(oFog.x)` computed per vertex (2646); raw T0–3}
- stage-0 texture, `rc`, `stage[4]`, `fog_color` and `zpass_out` when `backend_use_rc()`

**GL backend** (`gl_backend.c`):
- The vertex shader (89) rebuilds clip space as `(ndc·w, zclamp·w, w)` with z mapped by CLIP_MIN/MAX and clamped per vertex.
- The fragment shader (123) does texture×diffuse, the combiners, the lightmap special case and the alpha test.
- Per draw there are two `glBufferData` orphan uploads (1235, 1427) and several `glGetUniformLocation` calls (1203, 1397, 1415).

## 2. Staged design

### Stage 0: GL backend groundwork (needed by both later stages)
1. **Uniforms into UBOs.** Move the fragment uniforms into `FragState` (binding 1). Add `XfConst` (binding 0: `vec4 c[192]`) and `XfPost` (binding 2: `surf`, `depth_min/max`, `aa`, `c59`, `near_z`, fog enable/mode/params, stage-0 uv scale, `ts[4]`). Cache every uniform location. This lets one fragment shader link with many vertex shaders without resetting uniforms per program.
2. **Streaming ring buffer.** One 16 MB `GL_ARRAY_BUFFER` and one 4 MB `GL_ELEMENT_ARRAY_BUFFER`. Write with `glMapBufferRange(WRITE|UNSYNCHRONIZED|INVALIDATE_RANGE)`, or `glBufferSubData`, and orphan on wrap. GL 4.1 on macOS has no `ARB_buffer_storage`, so no persistent mapping. Use this for the existing CPU path as well.
3. **New vertex shaders feed the existing fragment shader.** They write the same varyings (`v_col`, `v_uv`, `v_spec`, `v_fog`, `v_tc0..3`), so the fragment shader does not change.

### Interface changes (`nv2a_backend.h`, additive)
```c
typedef struct {            /* one NV2A vertex attribute as the title set it */
    const void *data;       /* host ptr to element min_index (guest mem or inline_buf); NULL: constant */
    uint32_t guest_va;      /* 0 for inline/immediate payloads (future static-VBO cache key) */
    uint32_t type, size, stride;   /* NV097 encodings; stride bytes */
    uint32_t bytes;         /* readable span from data: (max-min)*stride + elem bytes */
    float    value[4];      /* used when data == NULL: imm_attr, or 0,0,0,1 */
} Nv2aAttrib;

typedef struct {            /* fixed function, policy already resolved by the executor */
    float composite[16], mv[4][16], proj[16], texmat[4][16], c59[4];
    uint32_t skin_mode;           /* 0..6, already forced to "use composite" when RECOMP_FFP_SKIN=0 / !MVP */
    uint32_t use_composite;
    uint32_t lighting, light_kind[8], colmat_amb, colmat_dif, flip_normals;
    float    scene_amb[3], mat_alpha, light[8][20]; /* amb3 dif3 dir3 pos3 range att3 ... */
    uint32_t texgen[4][4], texmat_on[4];
    uint32_t color_slot, tex0_slot, has_normal, has_color, no_tex;   /* color_attr()/texcoord_attr() picks */
} Nv2aFixedState;

typedef struct {
    uint32_t prim;                  /* NV_PRIM_* */
    const uint32_t *indices; uint32_t index_count, min_index, max_index;
    Nv2aAttrib attr[16];
    uint32_t kind;                  /* NV2A_RAW_PROGRAM | NV2A_RAW_FIXED */
    const uint32_t (*program)[4]; uint32_t program_len; uint64_t program_hash;
    const float (*constants)[4]; uint32_t const_gen, const_lo, const_hi;  /* dirty range */
    const Nv2aFixedState *ff;
    float aa_sx, aa_sy, vp_off_z; uint32_t near_z;
    uint32_t fog_enable, fog_mode; float fog_param[2];
    float uv_scale0[2];             /* tex_size_from_format(stage0) ? (w,h) : (1,1) */
} Nv2aRawGeometry;
```
- Add `const Nv2aRawGeometry *raw;` to `Nv2aBatch`. With `raw` set, `vertices`/`ext` are NULL and `count` is 0. The `rc`, `stage[]`, `texture`, `state` and `zpass_out` fields keep their current meaning.
- Add a fourth, optional member to `Nv2aBackend`: `int (*draw_raw)(const Nv2aSurface*, const Nv2aBatch*);`. It returns 0 to decline the batch. Other titles' `{clear, draw, flip}` initialisers zero-fill it, so they are unaffected.
- `nv2a_vsh_interp.c` gains:
  - `nv2a_vsh_program(uint32_t *start, uint32_t *len, uint64_t *hash)`: the hash is recomputed lazily, only after `program_word` or `set_start_slot`.
  - `nv2a_vsh_const_gen()` plus a dirty range, bumped in `constant_word`, `constant_component` and `set_constant`.
  - `nv2a_vsh_constants()`, returning the `s_const` base.

### Executor hook (`nv2a_pb_exec.c`)
At the top of `raster_batch_program()` (3329), add `if (raw_xf_enabled(kind) && try_draw_raw()) return;`. `try_draw_raw()` does the following:
1. Compute min and max index in one pass. For quads and quad strips, convert to a triangle-list index array using the same vertex order as lines 3450–3460. Points and lines are dropped, as the CPU path drops them. Indices are rebased to `min_index`.
2. Fill `Nv2aAttrib[16]` with the `transform_vertex` rules:
   - `size==0`: use `imm_attr` as a constant.
   - `offset==0`, `stride==0`, an unsupported type (3, 7+), or `!guest_span_ok(span)`: use the constant (0,0,0,1), or decline the batch if it is position.
   - Inline batches: `data` points into `inline_buf`, bounds-checked against `inline_count`.
3. Fixed function: `ffp_pack_state()` resolves every quirk once per batch: the composite source, the projection fallback to c[0..3], the env switches, the skin/composite choice, and the colour and texcoord attribute heuristics. The backend shader is then pure math and the hacks stay in one place.
4. Build `Nv2aBatch` as `backend_flush` does (`textured = !s_xf_no_tex`, `use_rc`) and call `draw_raw`. On success, update `batches_program/ffp` and `tris_drawn`, call `note_texture_use`, and call `note_drawn()`. `note_drawn()` is required: `presented_va` depends on it.
5. The backend declines (and the executor falls back to the CPU path) on:
   - `cxt_write` set while the program has any output to c[]
   - no FINAL within the 136 slots
   - an attribute type it cannot express
   - a shader compile failure

Env switches:
- `RECOMP_GPU_XF`: `0` (default at first), `ff`, `vp`, or `1` for both.
- VC3's `main` sets it once the stage is validated.
- `RECOMP_GPU_XF_CHECK=<n>`: see section 4.

### Stage B: fixed function on the GPU (do first)
Use one uber vertex shader that branches on uniforms (the branches are uniform-coherent, so this is cheap). Only attribute format varies by key: a `cmp_mask` and a `snorm_mask`, at most a handful of variants. It is a line-by-line port:
- Skin weights: same mode table as 1905–1914, weights from `in vec4 a1`.
- `eye = Σ ww_k · mv_k · pos`; `clip = proj · eye`, or `composite · pos` when `use_composite`.
- One eye position and normal, shared by lighting and texgen. Note that `lit_color` uses mv[0] only, without skin; mirror that exactly. `ffp_eye` uses the skinned blend.
- Lighting as at 1827–1863, clamped.
- Texgen as at 2010–2028, then the texture matrix.
- Screen position = clip/w + c59, with xy × aa.
- Fog input 0, so `v_fog = fog_factor(0)` computed in the shader. `v_spec = 0`.

Then the shared epilogue described under "Screen to GL clip space" below.

### Stage A: vertex-program microcode to GLSL
`vsh_translate(const uint32_t (*prog)[4], start, len, key) → GLSL string`. The cache is a hash table keyed by `(program_hash, cmp_mask, snorm_mask)` and holds the linked GL program, which reuses the shared fragment shader. Translation rules:
- Prologue:
  - `vec4 R[12]` set to 0, and `oPos` set to 0.
  - `vec4 O[13]` set to 0, except `O[3].w = O[4].w = 1` and `O[9..12].w = 1`.
  - `int a0 = 0`.
  - Inputs `in vec4 v0..v15`, at locations 0–15. That uses all 16 vertex attributes on Apple GL; there are none spare.
- Each instruction becomes one `{ }` block:
  - Read A, B and C first. Each read is `src(mux, reg, swz, neg)`:
    - mux 1: `R[reg]`; reg 12 is `oPos`, and reg > 12 reads `R[0]` (the interpreter does this).
    - mux 2: `v[idx]`.
    - mux 0 or 3: `c[k]`. With the relative bit, `k = idx + a0`, and if k is outside 0..191 it becomes 0.
  - Then apply the swizzle as a GLSL swizzle and the negation as a minus sign.
  - Compute `m` from the MAC op:

    | MAC op | GLSL |
    |---|---|
    | mov | `a` |
    | mul | `a*b` |
    | add | `a+c` |
    | mad | `a*b+c` |
    | dp3 | `vec4(dot(a.xyz,b.xyz))` |
    | dph | `dot3 + b.w` |
    | dp4 | `vec4(dot(a,b))` |
    | dst | `vec4(1, a.y*b.y, a.z, b.w)` |
    | min | `mix(b, a, lessThan(a,b))` (mirrors the C select, so NaN behaves the same) |
    | max | `mix(b, a, greaterThanEqual(a,b))` |
    | slt / sge | `vec4(lessThan(...))` / `vec4(greaterThanEqual(...))` |
    | arl | `a0 = int(floor(a.x + 0.001)); m = a;` |
    | ops 14, 15 | `a` |
    | no MAC op | `vec4(0)` |

  - Compute `i` from the ILU op, which works on `c.x`:

    | ILU op | GLSL |
    |---|---|
    | rcp | `x==0 ? +INF : 1/x` (build INF with `uintBitsToFloat`) |
    | rcc | 1/x with \|f\| clamped to [5.42101e-20, 1.884467e19], keeping the sign |
    | rsq | `inversesqrt(abs(x))` |
    | exp, log, lit | as in the C code (log of 0 gives -INF) |

  - Writes, in this order:
    1. MAC to temp: only when the op is not 0 or arl and the mask is nonzero; target `tdst` (12 is oPos, above 12 is dropped).
    2. ILU to temp: the target is `R1` whenever a MAC op is present, `tdst` otherwise.
    3. Output: `oaddr==0` is oPos, `oaddr<13` is `O[oaddr]`. The source is `i` when the "out from ILU" bit is set, `m` otherwise.

    Each write uses a masked component assignment generated per component (x=8, y=4, z=2, w=1).
- Epilogue:
  - D0/D1 clamped as `c>1?1:(c>=0?c:0)`.
  - If `oPos.w` is +0 (`floatBitsToUint(w)==0u`), set w to 1.
  - xy × aa.
  - Then the shared clip-space epilogue below.
- Constants: one UBO, re-uploaded only for the dirty range when `const_gen` changes.
- `#define` a `NV2A_VSH_DUMP`-style comment holding the disassembly (reuse `dis_src`) so a shader can be traced back to its microcode.

### Shared vertex epilogue: screen space to GL clip space
- `ndc = s.xy / surf * 2 - 1`; `zn = (s.z - dmin)/(dmax - dmin) * 2 - 1`, without clamping.
- `gl_Position = vec4(ndc*w, zn*w, w)`. This is the same affine map the current shader uses (`gl_backend.c:109–112`), so GL's homogeneous clipping matches the CPU's choice to clip in (x·w, y·w, z·w, w).
- `gl_ClipDistance[0] = w - 1e-3`; `gl_ClipDistance[1] = near_z ? (s.z - c59z)*w : 1`. Enable `GL_CLIP_DISTANCE0/1` and `GL_DEPTH_CLAMP` (core since 3.2) for raw draws. GL's own near/far z clipping then does not apply, and depth is clamped per fragment.
- Outputs:
  - `v_col = d0` (float, not 8-bit packed); `v_spec = d1`
  - `v_fog = fog_factor(oFog.x)` (port of line 2646)
  - `v_tc0..3 = oT0..3` (raw, so the fragment shader's `stage_uv` with `u_ts` still handles normalised vs texel units)
  - `v_uv = oT0.xy * uv_scale0 / u_texsize` (exactly what `backend_from_xf` and the current shader compute together)

### Attribute formats in GL
| NV2A type | GL setup |
|---|---|
| 0 D3DCOLOR | `glVertexAttribPointer(i, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, …)`: hardware BGRA→RGBA, always 4 bytes, as in `fetch_attr` |
| 2 float, size 1–4 | `GL_FLOAT`; GL's default fill (0,0,0,1) matches |
| 4 ubyte | `GL_UNSIGNED_BYTE`, normalised |
| 1 short normalised | `GL_SHORT` unnormalised, × (1/32767) in the shader (key bit `snorm_mask`), to match exactly |
| 5 short | `GL_SHORT` unnormalised |
| 6 packed 11:11:10 | `glVertexAttribIPointer(i, 1, GL_INT)` plus a decode in the shader with `bitfieldExtract` (key bit `cmp_mask`; VC3 uses this for v2 and v14) |
| constant | `glDisableVertexAttribArray(i)` + `glVertexAttrib4fv(i, value)` |

### Uploads
- Per batch, copy each attribute region (attributes whose offsets fall within one stride of each other share one copy) for `[min, max]` into the ring buffer.
- Draw with `glDrawElements` (uint16 indices, since `max-min < 65536` always holds) or with native `GL_TRIANGLE_STRIP`/`FAN`.
- This uploads each unique vertex once at its stride, about 32–40 B. Today the CPU path uploads 112 B per vertex per triangle corner: roughly 10× less upload traffic.
- Later stage (C), optional: a static-VBO cache keyed by `(guest_va, bytes)` with the same sampled-hash/full-hash-per-frame validation as textures (`gl_backend.c:862–869`). At about 90 draws per frame it is not needed at first.

## 3. Risks, and edge cases the GLSL must reproduce
1. **Whole-batch abort.** On the CPU, a single fixed-function vertex with |w| ≤ 1e-6, or an unreadable position, drops the entire batch (3336). On the GPU, set that vertex's clip distances to −1, which only kills the triangles that touch it. This divergence is deliberate; the check mode will report it.
2. **Non-finite positions.** The CPU drops the triangle (3242). In GL the result is undefined. Mitigation: force −1 clip distances when `any(isnan||isinf)`. Partially clipped slivers can remain; only a geometry shader would fix that, and it isn't worth it.
3. **Depth.** The current shader clamps z per vertex before interpolation; the new path clamps per fragment via `DEPTH_CLAMP`. This is more correct, but triangles straddling CLIP_MIN/MAX change slightly. The near-plane position (c59.z vs CLIP_MIN) must use `vp_off_z`, not `dmin`.
4. **x/y and guard band.** GL clips x and y, the CPU does not. The picture is the same; this only adds precision differences at huge coordinates. The fbo viewport is the full surface, not the `SET_SURFACE_CLIP` rect, which is unchanged from today.
5. **Culling and winding.** The CPU emits strip triangles without alternating winding. Native GL strips do alternate, which is correct, so culling will differ only when `VC3_GL_CULL` is on (it is off by default). Provoking vertex and flat shade are not modelled anywhere.
6. **Points, lines, point sprites, oPts, back-face colours oB0/oB1:** dropped by the CPU and dropped here too.
7. **Programs that write c[]** (`cxt_write`): stateful across vertices, so these stay on the CPU.
8. **Colour quantisation.** The CPU packs d0/d1 to 8 bits; the GPU keeps floats. Expect ±1/255 differences.
9. **Apple GL limits.** 16 vertex attributes in total. Shader compiles cause first-use hitches (VC3 has few programs). Fast-math may turn INF/NaN into finite values; `rcc` clamping covers the D3D epilogue.
10. **`note_drawn`, `zpass` and statistics** must still run on the raw path, or the presented surface and visibility tests break.
11. **The heuristics:** `color_attr` and `texcoord_attr` choices and the `s_xf_no_tex` rule must be resolved in the executor (`ffp_pack_state`), not re-guessed in the backend.

## 4. Verification
- **Per-vertex check** (`RECOMP_GPU_XF_CHECK=n`): for the first n batches of each `(kind, program_hash)` and then every 1000th batch, do both of these:
  - the CPU computes `s_xf[i]` for every index;
  - the backend runs the same vertex shader with transform feedback and `GL_RASTERIZER_DISCARD`, capturing an extra `o_screen = (sx, sy, sz, w)` varying plus d0, d1, fog and T0–3.

  Compare per index: position to relative 1e-4 (absolute 1e-2 px), colours to 1/255, texture coordinates to relative 1e-4. On a mismatch, log the batch, the index, both values, and `dump_program()` for a program batch or the packed `Nv2aFixedState` for a fixed-function batch. This isolates translation bugs from rasterisation differences.
- **Counters per run:** batches on the GPU per kind, decline reasons (attribute type, cxt write, span out of bounds, compile failure, no FINAL), and triangles.
- **Frames:** run attract mode with identical input under `RECOMP_GPU_XF=0` and `=1`, dump the same flip numbers (the existing flip dump/readback), and diff by PSNR and max delta per tile. Use `VC3_GL_FTRACE` on the frame that differs to find the batch.
- **Performance:** record `[PROF]` executor-thread time per kind and `[FPS]` before and after each stage.

## 5. Sequencing
1. Add the `[PROF]` split and confirm fixed-function transform dominates.
2. Stage 0: UBOs, cached uniform locations, ring buffer. This also cuts per-draw cost on the existing CPU path.
3. Interface additions, `try_draw_raw`, `ffp_pack_state`, the CPU fallback, and the check mode.
4. Stage B, the fixed-function uber-shader. This is the main fps gain.
5. Stage A, the vertex-program translator and cache.
6. Optional: the static-VBO cache, and possibly moving texture hashing off the per-bind path if `[PROF]` shows it becomes the next bottleneck.

### Critical Files for Implementation
- /Users/lialan/Downloads/VR/vc3-recomp/.claude/worktrees/vc3-bringup/external/xboxrecomp/src/kernel/nv2a_pb_exec.c
- /Users/lialan/Downloads/VR/vc3-recomp/.claude/worktrees/vc3-bringup/external/xboxrecomp/src/kernel/nv2a_backend.h
- /Users/lialan/Downloads/VR/vc3-recomp/.claude/worktrees/vc3-bringup/external/xboxrecomp/src/kernel/nv2a_vsh_interp.c
- /Users/lialan/Downloads/VR/vc3-recomp/.claude/worktrees/vc3-bringup/external/xboxrecomp/src/kernel/nv2a_vsh_interp.h
- /Users/lialan/Downloads/VR/vc3-recomp/.claude/worktrees/vc3-bringup/src/gpu/gl_backend.c