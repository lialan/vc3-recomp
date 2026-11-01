# Shared Chihiro Vulkan backend

This library implements NV2A rendering for hosts using the shared Chihiro runtime.
Performance changes belong here (or in the shared xboxrecomp executor), not in
per-game lifted code. VC3 is a validation workload, not an input to backend policy.
No title/address/scene checks select the optimizations below.

## Host contract

Link `chihiro_vulkan` and supply `ChVkBackendConfig` from `vk_backend.h`: the Vulkan
device/queue, exact guest memory mapping, texture decoder and optional presentation
hooks. The host owns these resources and must stop/drain the executor before
backend destruction. Only one backend can be attached to the current global
`Nv2aBackend` callback interface. Queue locking must cover complete callbacks;
shared pipeline-cache access is serialized by that same ownership rule.

The common `vk_runtime.c` host adapter selects capabilities and reads
`CHIHIRO_VULKAN_*` options. Direct library callers use config fields and target
APIs. Existing `VC3_*` environment aliases are compatibility inputs; new games
should use the shared names. Game metadata, save/config locations, input scripts,
XR scene routing and lifted-code hooks remain host/profile responsibilities.

## Reusable performance mechanisms

| Mechanism | Configuration and requirements |
| --- | --- |
| Bounded command batches | `batch_draws`, `batch_draw_limit`: zero uses 256; explicit limits 1..256. Common host option `CHIHIRO_VULKAN_BATCH_LIMIT`. Configure targets before their first pipeline. Queries, guest fences, reads, clears, transfers and resource pressure retain real completion boundaries. |
| Resident color/depth | `resident_color`, `resident_depth`. Eligibility follows guest format/layout and device support. Unsupported paths retain strict conversion; unsupported aliases fail explicitly. |
| Exact D32 fallback for guest Z24 | Opt-in `quantized_depth` requires resident depth. GPU round-trips preserve the shared CPU pack/unpack semantics; stencil is retained. Native D24 remains native, Z16 stays strict. Optional `depth_quant_draw` / `CHIHIRO_VULKAN_DEPTH_QUANT_DRAW=1` uses a depth-only draw instead of compute; it requires `quantized_depth`. It regressed the measured VC3/KosmicKrisp run and remains experimental; compute is the default. |
| Guest write tracking | Optional `guest_write_watch` supplied by host after installing mapping/fault hooks. Unsupported pages use exact byte comparison. No guessed game addresses or skipped CPU edits. |
| Render-target texture snapshots | Opt-in `sample_render_targets` requires resident color and texture cache. Immutable versioned images, sampler keys, pins and byte budgets preserve pending draw lifetimes. |
| Pipeline reuse | Device/backend-owned pipeline cache survives target resize. Pipeline objects remain target-owned; blend constants are dynamic. Opt-in `CHIHIRO_VULKAN_SPECIALIZE_RC` specializes instruction words, retaining dynamic factors/fog/textures. |

Batch size is a resource/throughput choice, not a rendering approximation. Smaller
limits reserve fewer descriptors and retain fewer samples. Geometry arena capacity
can end batches earlier. Changing the configured limit after pipeline creation is
rejected, so descriptor-pool capacity cannot become inconsistent.

## Validation and evidence

Configure this directory with `CHIHIRO_VULKAN_SHADER_TESTS=ON` for standalone
Vulkan-only tests, including `vulkan_command_batch_check`,
`vulkan_specialized_combiner_check`, `vulkan_render_texture_check` and
`vulkan_depth_quant_check`. These use synthetic NV2A states and guest storage,
without a game executable, ROM or headset. CPU storage/quantization tests are
available with `CHIHIRO_VULKAN_CPU_TESTS=ON`.

`tools/run-vulkan-local.py` is currently a VC3-specific host benchmark driver.
Its input timing, captures and FPS observations are workload evidence, not shared
runtime defaults. See `validation/direct-vulkan-depth-batch/README.md` for local
results and limitations. Other games must validate their own formats, scenes,
correctness and pacing before enabling experimental options. Simulator evidence
does not establish headset performance or cross-game acceptance.

Standalone target quantization mode 3 uses a direct depth input attachment. It is a validation experiment, not selected by the backend/runtime configuration: isolated KosmicKrisp measurements were slower than compute. Its shader, ownership, and exactness fixtures are game-independent; see `validation/direct-vulkan-depth-batch/README.md`.

Experimental `depth_quant_inline` / `CHIHIRO_VULKAN_DEPTH_QUANT_INLINE=1`
selects same-subpass depth input rounding. It requires `quantized_depth`, is
mutually exclusive with `depth_quant_draw`, and leaves native D24 untouched.
The backend prepares a compatible renderpass before the first pipeline, even
if the first draw only writes color. Target mode 4 rejects selection after an
incompatible pipeline has been created. Turning quantization off keeps the
compatible pass until target destruction. Framebuffer-local barriers surround
the helper draw; guest query counts exclude it. CPU clears use the separate
input pass. No guest addresses, game identifiers, or scene heuristics enter
this policy. It remains opt-in pending a demonstrated real-workload benefit.

Input-attachment image usage is opt-in through `ch_vk_target_create_input_depth`; ordinary constructors preserve their original usage flags. Modes 3/4 reject ordinary targets. The backend selects this constructor only for `depth_quant_inline` on a D32 fallback.

`vulkan_depth24_check` is a standalone native-depth reference fixture. The current
D32 post-draw conversion deliberately retains the CPU-roundtrip contract, which
can differ from native D24 visibility at rounding boundaries: repeated z=0.5
with EQUAL passes D24 but fails that D32 path. The fixture records this distinction;
passing the D32 oracle alone does not establish native-format comparison parity.

Experimental `depth_quant_fragment` / `CHIHIRO_VULKAN_DEPTH_QUANT_FRAGMENT=1`
selects target mode5 on guest Z24 D32 fallbacks. It requires `quantized_depth`
and resident depth and is mutually exclusive with `depth_quant_draw` and
`depth_quant_inline`. Incoming depth uses binary32 scaling and nearest-even
rounding, matching the local native-D24 comparison fixture; this intentionally
differs from the legacy CPU/post-draw contract. Native D24 is unaffected. Mode5
canonicalizes existing storage once when enabled and uploads/clears at their
existing boundaries; shader output then supplies canonical depth directly.
Ordinary pipelines use separate unchanged fragment modules. The mode is opt-in;
see the depth-batch validation notes for correctness coverage and local performance.
