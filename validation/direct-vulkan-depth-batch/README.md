# Combined batch depth readback

Depth/stencil reads with pending draws now append the image-to-buffer copy
and host barrier after the render pass in the same command buffer. The
existing real fence still completes before CPU mapping/invalidation and
publication. The separate transfer submission is retained when no batch is
pending, and uploads retain their existing ordering and conversion rules.
The common transfer recorder preserves both image aspects and restores the
depth/stencil attachment layout. Batch GPU timing includes the appended copy.

A focused GPU regression compares exact D32 float/stencil output against the
synchronous path after a draw writes both depth and stencil. It asserts one
combined readback and one batch submission. Specialized-combiner and renderer
snapshot checks pass on Apple M4 Pro/KosmicKrisp. Mac and Android offscreen
builds pass. Packed native D24 runtime verification remains unperformed here.

The earlier same-binary DMA-walker control /tmp/vc3-dma-walk-profile reached
frame 1200; its capture visibly shows the mission intro (PRESS START TO SKIP),
not combat. Window 1081..1201 was 1.32 FPS, p99 2889.42 ms. That run predates
this change and remains a baseline, not evidence of an optimization speedup.
No headset access or Quest performance acceptance occurred.

## Next structural experiment: exact GPU quantization

The strict D32 fallback quantizes written depth through guest Z24 after every
draw. Residency cannot simply skip that conversion. A proposed compute pass
can round-trip D32 -> Z24 -> D32 on GPU between draws, retaining semantics
while publishing guest RAM only at the existing coherence boundaries.

CPU exhaustive oracle tools/vulkan-depth-quant-exact-test.c passes every
nonnegative float bit pattern through 1.0 and all 2^24 depth words. Quantize
with scaled=z*2^24, q=uint(scaled), then decrement if z>0.5, otherwise increment
only if scaled-q>0.5. Normalize q with bits(float(q)*2^-24)+(q!=0). This matches
the existing double-precision CPU formulas bit-for-bit in that oracle.
GPU implementation, driver arithmetic validation, transfers/barriers, guest
coherence and performance are NOT implemented or established yet.

Local GPU arithmetic check now passes 1,048,576 sampled/boundary Z24 packing
and unpacking cases bit-for-bit against the double-precision CPU oracle.
Build target vulkan_depth_quant_check; set CHIHIRO_DEPTH_QUANT_SHADER to
<build>/depth_quant_test.spv and select the local ICD. The shader uses the
shared depth_quant.glsl helper; no renderer integration is enabled yet.
Unlike the CPU test, this GPU fixture is sampled, not exhaustive.

The DMA baseline ended at its 480-second timeout after at least 2041 frames,
without the prior invalid-state abort. Later windows were 2.56-2.92 FPS.
The combined-readback comparison is /tmp/vc3-combined-depth-profile using
immutable /tmp/vc3-combined-depth-vc3; inspect its report when terminal.

## Image round-trip recorder

vk_depth_quant now records D32 depth image -> storage buffer -> compute ->
depth image, restoring the combined depth/stencil attachment layout without
copying stencil. ch_vk_target_quantize_depth is a synchronous validation API,
with queue/dispatch/storage limits checked before allocation. It does not
enable resident depth or alter normal draws. Scratch buffer/resources live
until target teardown after completion.

The specialized fixture performs 128 repeated 4x4 uploads, quantizations and
readbacks, with random/boundary depths and varying stencil bytes. Depth is
exact against the CPU oracle; stencil is unchanged. The fixture passes with
VK_LAYER_KHRONOS_validation and synchronization validation requested, with no
VUID/SYNC diagnostics in /tmp/vc3-depth-roundtrip-validation.log. Android
offscreen build passes. Renderer residency integration remains to be done.

## Opt-in D32 residency

CHIHIRO_VULKAN_QUANTIZED_DEPTH=1 (requires resident depth) enables exact Z24
round-trips after each D32 draw, inside the command buffer. Native D24 stays
on its native path. Float staging is used for the resident manager so CPU
edits, target switches, sampling/fence publication and resize retain the
existing ownership rules. D32 Z16 continues through the strict fallback.
The default is off pending actual-game measurements.

Thirty-two queued draws around rounding boundaries, covering all eight depth
comparison functions, match strict per-draw CPU round-trips in exact final
color/depth/stencil. Backend scenarios compare every guest byte after depth
writes, CPU component edits, target switching, resize/full clears and flush.
Both fixtures pass with validation and synchronization validation requested;
logs /tmp/vc3-quantized-{draw,backend}-validation.log have no VUID/SYNC errors.
Mac main and Android offscreen builds pass. No guest performance claim yet.

## First guest evidence (still running at checkpoint)

/tmp/vc3-quantized-depth-profile uses immutable /tmp/vc3-quantized-depth-vc3,
quantized residency enabled, DMA walker, write-watch disabled. Loading window
121..241 reached 56.73 FPS. Mission-intro window 1201..1321 reached 15.57 FPS
vs about 2.9 FPS in strict-depth controls. Frame-1200 PPM pixel payloads match
/tmp/vc3-dma-walk-profile exactly: zero differing pixels, maximum channel
error zero. This single capture is not full-game correctness or combat proof.
Later 1561..1681 was 16.37 FPS and 1681..1801 11.94 FPS; pacing remains poor.
CPU sample cpu-intro.txt shows substantial remaining memcmp plus command
recording work. No matched repeat/Quest measurement is claimed.

1be418d skips quantization when draws cannot write depth, and prints the
selected residency mode at startup. Focused tests/validation and both builds
pass; it is NOT in the immutable first quantized run. Combined-readback control
was stopped by SIGTERM after multiple mission windows (report exit -15).

The first quantized guest run completed all 2160 frames successfully (exit 0),
with frame-window data in its report.json. Final measured windows were 18.05
and 17.79 FPS. The follow-up /tmp/vc3-quantized-writeonly-off uses immutable
/tmp/vc3-quantized-writeonly-vc3, changing only the non-depth-writing skip;
it remains running. No clear intro speedup from that skip is established.
The CPU sample attributes the largest memcmp callchains to depth observation
and resident color observation. Evaluate write-watch with this same binary.

Frame 1200 is the attract/mission intro and displays zero credits. The existing
2160-frame captures do not establish playable combat. Extend future runs with
late coin/start inputs and periodic captures while retaining the original
input prefix for comparable early windows. Do not call intro FPS combat FPS.

## Write tracking comparison and longer local run

Both /tmp/vc3-quantized-writeonly-off (2160 frames) and
/tmp/vc3-quantized-writeonly-watch (4800 frames) completed with exit 0 and
passed_execution=true, using identical immutable /tmp/vc3-quantized-writeonly-vc3.
Write-watch enabled raised matched 1801..1921 / 1921..2041 windows from
18.67 / 18.58 FPS to 26.87 / 26.17 FPS. Frame 1200 pixel payloads match exactly.
The watch run adds sparse later screenshots and late coin/start/fire inputs;
those inputs occur after the compared first 2160 frames. This is one local
comparison, not a repeated benchmark or whole-game parity proof.

At frame 4080 the capture is mission selection with zero credits; frame 4560
is the bank attract intro with INSERT COIN(S). Automated inputs were logged,
but playable combat is still unverified. Later windows fall as low as 11.27
FPS. cpu-late.txt profiles command recording/render-pass setup and GPU waits;
this backend is not yet consistently performant. No headset was accessed.

## Larger bounded command batches

The draw-cap is 256 (previously 64). Descriptor sets and pinned sample lifetimes
remain bounded; all query, guest publication, clear/import, capacity and eviction
boundaries still finish real GPU work. Larger descriptor pools increase per-target
resource reservations; sets remain lazily allocated and geometry remains bounded
by the existing arena policy.

The standalone Vulkan-only command-batch fixture now exercises 514 draws across
three batches, exact additive pixels, changing texture contents, pinned-budget
fallback, queries, color/depth edits, growth and LRU retirement. Its LRU pressure
uses static blend factors because blend constants are now dynamic. It and the
specialized/backend fixtures pass with validation and synchronization validation
requested, with no VUID/SYNC diagnostics. Mac main and Android offscreen builds
pass; no device run was performed.

/tmp/vc3-batch256-profile completed 2160 frames (exit 0), using immutable
/tmp/vc3-batch256-vc3, with the same write-watch/quantized-depth configuration.
Frame 1200 matches the preceding 64-draw watch run exactly. Windows 1801..1921
and 1921..2041 improve from 26.87/26.17 to 31.45/31.30 FPS; frames over 33 ms
fall from 102/110 to 32/35 per 120-frame window. This is one local comparison;
loading overlapped a short compilation, but measured intro windows did not.
Remaining command-recording, GPU depth round-trip costs and first-use compilation
still prevent smooth 60 FPS. Full playable-combat acceptance remains open.

### Capture interpretation correction

Reviewing the intermediate captures shows active combat at frame 1680 (player
HUD/ammo/health and enemies), then a continue-after-death screen at frame 2160.
The scripted inputs did start gameplay; zero remaining credits on later screens
was not evidence that every run remained in attract mode. Controlled sustained
combat/pacing is still open. Frames 1200, 1680, 2640, 3120 and 3600 match exactly
between the 64-draw watch run and /tmp/vc3-batch256-extended. Frame 2160 differs
in 271086 pixels (mean absolute channel difference 5.924); frame 4080 differs in
12700 pixels (mean 0.104). The images depict the same respective scenes, but the
cause of these differences has not been isolated. Do not claim whole-run parity.

## Shared configuration

`ChVkBackendConfig.batch_draw_limit` and `ch_vk_target_batch_limit` expose the
bounded draw budget to all games (zero/default 256, explicit 1..256). The common
host accepts `CHIHIRO_VULKAN_BATCH_LIMIT`; the local runner records it. Invalid
limits and changes after pipeline creation are rejected. Library integration and
ownership requirements are documented in runtime/src/gpu/vulkan/README.md.
The latest 7200-frame run uses the earlier immutable batch256 binary, not these
new configuration APIs.

Shared batch API validation passes: standalone 514-draw test at limits 1, 64,
256 (514, 9, 3 submissions respectively), rejected invalid/late configuration,
exact pixels and resource lifetimes. Backend scenarios use limits 1/64/default
and compare exact guest memory. Validation + sync validation requested, no
VUID/SYNC diagnostics. Mac main and Android offscreen builds pass.

/tmp/vc3-batch256-extended completed all 7200 frames with exit 0, no failure
diagnostics. This is the earlier immutable /tmp/vc3-batch256-vc3. Later inputs
add service/coins/start/fire. Frame 5520 shows a continue screen with 4 credits;
the run includes gameplay and deaths, not sustained controlled combat. A CPU
sample is saved in cpu-late.txt. Later scene costs and pacing remain poor;
optimizations are not performance acceptance. No headset was accessed.

## Combine completion readbacks

The shared backend now flushes dirty resident depth before its final target
completion loop, allowing the depth copy to join pending draws. Color readback
also appends its transfer/host barrier to pending draws and waits on the same
real fence, rather than finishing and starting a second submission. The final
loop still drains targets without dirty resident storage. Invalid color output
buffers are rejected before consuming pending draws. Combined color/depth copies
use the shared staging buffer separately, never concurrently.

Exact-pixel command/lifetime tests at limits 1/64/256 and exact-guest-memory
backend scenarios pass with validation and sync validation requested; no VUID or
SYNC diagnostics in /tmp/vc3-combined-{color,flush}-validation.log. Mac main and
Android offscreen builds pass. Real-game performance remains to be measured.

The unchanged immutable batch256 binary repeat (/tmp/vc3-batch256-repeat)
completed 2400 frames. Frame 1200 matches the previous extended run exactly.
Frames 1680/2160 differ even without a code change: respectively 148603/271431
pixels, mean absolute channel differences 0.603/6.003. This establishes run-to-run
variation, not its cause or whole-game correctness. Single-frame comparisons
later in gameplay cannot alone attribute differences to an optimization.

Combined-readback guest measurement: /tmp/vc3-combined-flush-profile completed
2400 frames (exit 0, no failure diagnostics), using immutable
/tmp/vc3-combined-flush-vc3. Frame 1200 differs from the preceding repeat;
this comparison does not establish exact guest capture parity.
Windows 2041..2161 and 2161..2281 reached 32.77/32.59 FPS versus 26.27/25.29
in /tmp/vc3-batch256-repeat; >33ms counts fell from 119/119 to 15/10. Earlier
batch256 controls varied, so these are individual measurements, not a confidence
interval. Final live-target counters: 12888 combined depth reads and 10443
combined color reads; control depth reads were 8309, with no combined color path.
A differing draw count and known later capture variation prevent whole-run exact
parity claims. The backend remains below the performance goal; continue profiling
shared guest publication/conversion and GPU round-trip costs. All runs are terminal.

## Shared exact CPU Z24 normalization

`vk_depth_quant_math.h` supplies exact binary32 equivalents of the Z24 pack and
unpack arithmetic. Guest depth loads and CPU depth-component merges now use its
unpack helper. The existing double-precision store arithmetic is retained: a
float-only packing experiment was slower and was rejected. The exhaustive oracle
is now a CMake CPU-test target (`vulkan_depth_quant_exact_test`) and directly tests
the shared helpers against independent double-precision expressions.

All 16777216 codes and all nonnegative binary32 values through 1 match exactly.
Guest layout tests and exact backend guest-memory scenarios pass with Vulkan
validation/sync validation requested. Mac main and Android offscreen builds pass.
No VUID/SYNC diagnostics in /tmp/vc3-depth-cpu-final-validation.log.

Microbenchmark sources/results are in /tmp/vc3-depth-cpu-bench: 512 calls on a
640x480 pitched Z24S8 surface, clang -O3. Reference load 0.126 ms vs retained
load-only candidate 0.107 ms; store remains 0.221 ms (reference 0.210-0.222 ms).
This is CPU conversion evidence, not a measured whole-game FPS gain.

## Consecutive capture comparison

Both /tmp/vc3-sequence-control and /tmp/vc3-sequence-combined completed 1210
frames and captured frames 1194..1209. They use the older immutable batch256 and
combined-flush binaries, respectively, without the new CPU normalization helper.
No exact cross-sequence frame match was found. Optimized frame 1200 is closest
to control frame 1201: mean absolute channel difference 0.0111, versus 0.8057
against control frame 1200. This supports the visible animation timing shift,
but residual differences remain and whole-game parity is not established.
Comparison details are in /tmp/vc3-sequence-combined/comparison.json. All runs
are terminal. Continue shared backend profiling; smooth 60 FPS remains open.

## Depth-only draw round-trip prototype

The shared depth helper now has a graphics alternative: depth image -> existing
storage buffer -> fullscreen depth-only draw. The fragment shader applies the
same exact pack/unpack arithmetic and writes gl_FragDepth; stencil and color are
preserved. This removes the compute dispatch and buffer-to-image transfer. It
uses separate buffer/image resources, not attachment feedback or interlock.
The synchronous `ch_vk_target_quantize_depth_draw` API is validation-only; normal
resident rendering still uses compute. Its initial validation currently also
initializes/checks the compute path; runtime selection is not integrated yet.

128 small-image roundtrips alternate compute/draw, including rounding boundaries.
The full-size `vulkan_depth_roundtrip_bench` validates 640x480 depth/stencil and
untouched color after 128 repeats. Both pass with validation and sync validation
requested, without VUID/SYNC diagnostics in /tmp/vc3-depth-draw-{,full-}validation.log.
Android offscreen build passes (build only). Initial warm synchronous timings:
compute 0.280 ms/call, draw 0.257; validation run 0.266 vs 0.244. These isolated
measurements do not prove a game benefit. Next integrate as a shared opt-in and
compare actual batches, including graphics binding restoration and guest parity.

The local capability dump /tmp/vc3-kosmickrisp-capabilities.txt advertises feedback
extensions but not fragment interlock. The Vulkan render-pass synchronization
rules were consulted: https://docs.vulkan.org/spec/latest/chapters/renderpass.html
The prototype avoids those optional feedback features entirely.

/tmp/vc3-no-timestamps-profile completed 2400 frames using immutable
/tmp/vc3-depth-load-vc3 (CPU load improvement included), GPU timestamps disabled.
Final windows: 34.24, 34.47, 35.33, 35.16 FPS. CPU sample cpu-late.txt still shows
substantial depth completion/wait cost. This is not a same-binary on/off timestamp
comparison, so do not attribute the entire difference to instrumentation. No
headset used. All runs are terminal; performance goal remains open.

## Opt-in depth-draw integration

`ChVkBackendConfig.depth_quant_draw` / `CHIHIRO_VULKAN_DEPTH_QUANT_DRAW=1`
selects the graphics roundtrip when quantized D32 residency is enabled. Native
D24 remains native. The target mode API accepts 0/off, 1/compute, 2/draw; changes
finish pending work. Compute queue/dispatch limits apply only to compute mode.
Normal draw recording restores graphics pipeline/descriptors/push constants and
vertex/index buffers after the depth-only pass.

The queued 32-draw exact comparison against per-draw CPU roundtrips now runs both
modes across all eight depth comparisons. Backend exact guest-memory tests use
graphics mode, covering CPU depth/stencil edits, sampling, target switching,
resize and clears. Both pass with validation and sync validation requested, no
VUID/SYNC diagnostics in /tmp/vc3-depth-draw-{batch,backend}-validation.log. Mac
main and Android offscreen builds pass. No new game performance claim yet.

/tmp/vc3-depth-draw-on is running 2400 frames with immutable
/tmp/vc3-depth-draw-integrated-vc3, session 95115. GPU timestamps are disabled,
matching the prior 2400-frame profile. Run the compute control with the SAME
binary after this run completes; do not overlap benchmarks or restart a live run.

## Same-binary graphics/compute comparison

/tmp/vc3-depth-draw-on and /tmp/vc3-depth-draw-off both completed 2400 frames
(exit 0), with identical executable SHA256 and timestamps disabled. Graphics
roundtrip regresses actual guest throughput despite the isolated benchmark:
1921..2041 / 2041..2161 / 2161..2281 = 29.59/30.77/29.87 FPS graphics versus
34.93/34.85/34.89 FPS compute. Frame 1200 matches exactly; 1680/2160 do not,
consistent with the previously observed run-to-run variation but not proving
whole-game parity. Compute remains the default; do not recommend graphics mode
for this local driver based on the microbenchmark. All runs are terminal.

Next investigation: a depth input attachment plus a fullscreen quantization
shader might avoid the image-to-buffer copy and eventually keep quantization in
the same render pass. This is NOT implemented or validated. The core input
attachment/self-dependency rules must be checked carefully, especially depth/
stencil aspects, per-pixel ordering and BY_REGION barriers. Do not substitute
sampled-image feedback without the required features/layouts/pipeline flags.
Reference: https://docs.vulkan.org/spec/latest/chapters/renderpass.html

## Direct input-attachment experiment (2026-11-01)

Shared target mode 3 and ch_vk_target_quantize_depth_input implement exact D32
rounding through a depth-only input attachment. The depth-only descriptor view
is owned by the helper; D32 images include INPUT_ATTACHMENT usage. The pass uses
GENERAL for the depth/input reference and explicit barriers before/after. One
fullscreen triangle reads each pixel before its late depth write; stencil writes
are disabled. It needs no sampled-image feedback extension. Normal rendering
continues to use compute; no runtime/backend configuration selects this experiment.

/tmp/vc3-input-validation.log passes random and boundary depth/stencil checks and
32 queued draws across all eight depth comparisons against CPU roundtrips for
all three modes, with validation and synchronization validation requested and no
VUID/SYNC diagnostics. /tmp/vc3-input-bench-validation.log passes full 640x480
exact depth/stencil and untouched-color checks. Its 128 synchronous calls report
compute 0.227 ms, buffer draw 0.196 ms, direct input draw 0.280 ms. These are
isolated timings, not guest FPS; the input path is not a measured speedup. Mac
main and Android offscreen builds pass (build only; no headset access).

This supersedes the prior "not implemented" note for the separate-pass input
prototype only. Same-renderpass quantization is still unimplemented. That next
experiment needs a compatible color/depth/input subpass with BY_REGION self
dependencies and must preserve pipeline restoration and guest fence semantics.
Core attachment reference rules:
https://docs.vulkan.org/refpages/latest/refpages/source/VkSubpassDescription.html

## Same-subpass depth quantization checkpoint

Target mode 4 now keeps exact input-attachment rounding in the original render
pass. It uses framebuffer-local self dependencies, color writes disabled,
stencil disabled, and restores normal draw bindings on the following draw.
Guest visibility queries end before the helper triangle. Renderpass depth is
GENERAL within the subpass and returns to DEPTH_STENCIL_ATTACHMENT_OPTIMAL at
pass boundaries. Backend `depth_quant_inline` (shared environment
CHIHIRO_VULKAN_DEPTH_QUANT_INLINE) prepares the compatible pass before any
pipeline exists, including targets first used for color-only draws. It is
opt-in and mutually exclusive with the buffer-draw experiment. No game policy.

Validation: /tmp/vc3-inline-queued-validation.log covers 32 queued draws/all
eight comparisons against CPU roundtrips, and an exact 16-sample guest query
that excludes the quantization triangle. /tmp/vc3-inline-backend-validation.log
covers exact full guest memory with CPU edits, target switches, resize, clears,
and sampling. Validation/sync validation requested, no VUID/SYNC diagnostics.
Mac main and Android offscreen builds pass; no device activity.

The initial /tmp/vc3-inline-bench.log incorrectly called its draw loop queued:
it did not enable deferred color readback. Do not use those timings as batching
evidence. The corrected /tmp/vc3-inline-queued-bench.log asserts 128 draws and
one submission. It passes exact depth/stencil checks but reports 0.377 ms/draw
compute vs 0.496 ms inline. Isolated timings vary substantially between runs;
no performance claim follows from this fixture.

/tmp/vc3-inline-on completed 2400 frames, exit 0, late windows
1921..2041 / 2041..2161 / 2161..2281 = 34.04/33.59/33.32 FPS.
The same immutable /tmp/vc3-inline-integrated-vc3 control is running in
/tmp/vc3-inline-off (session 72525). Only INLINE differs; DRAW=0 and timestamps=0.
Do not overlap another GPU workload. Compute remains default pending comparison.

## Input image usage isolation and repeat

Same-binary /tmp/vc3-inline-off completed 2400 frames (exit 0), late windows
26.97/26.64/26.35 FPS. /tmp/vc3-inline-repeat also completed 2400 (exit 0),
27.72/28.20/27.72 FPS. Thus the initial 33-34 FPS inline run is not reliably
repeatable; do not claim a 25% improvement. Off/repeat frames 1200 and 1680 match
exactly, 2160 does not. On/off frame1200 exact, 1680 mean channel error .603,
2160 .169; visually both 1680 frames show the same combat scene. Not whole-game
parity. All three old-binary runs are terminal.

Review found the prototype set INPUT_ATTACHMENT usage on all D32 images, even
compute controls. This is now isolated in ch_vk_target_create_input_depth;
ordinary target constructors keep their previous allocation usage. Modes 3/4
reject ordinary targets, and only depth_quant_inline opts in at backend creation.
Native D24 is unchanged. /tmp/vc3-input-optin-validation.log passes exact queued
roundtrips, guest query count, full guest-memory backend coverage, and capability
rejection, with validation/sync validation requested and no diagnostics. Mac main
and Android offscreen builds pass. Corrected microbenchmark remains slower inline
(0.506 vs .380 ms/draw, 128 draws/one submission); no GPU overlap during fixtures.

A fresh same-binary pair with the isolated constructor is now running sequentially:
/tmp/vc3-input-optin-off then /tmp/vc3-input-optin-on, immutable
/tmp/vc3-input-optin-vc3, session 2058. Do not overlap GPU work. This pair changes
both quantization mode and its required allocation usage, as real configuration
selection does. Compute remains default. No headset/device activity.

## Isolated-allocation comparison and CPU validation improvement

/tmp/vc3-input-optin-off and -on both completed 2400 frames with identical binary
SHA256 and exit 0. Late 1921..2041/2041..2161/2161..2281 windows: see reports;
inline is 29.90/29.52/30.01 FPS, compute is roughly 33 FPS. The same-pass path is
not promoted. Both GPU alternatives remain explicit experiments; ordinary
constructors and compute behavior preserve the previous allocation policy.
All game runs are terminal; no headset/device use.

The shared CPU depth-store validation now reduces an invalid flag across the
input, allowing vectorization while rejecting the entire store before touching
guest memory. Actual Z24/Z16 rounding and output loops are unchanged. Clang -O3
reports width4/interleave4 vectorization for this validation loop. A/B/B/A,
512 calls at640x480 each, no concurrent game/GPU work:
reference .223/.215 ms per store, candidate .179/.176; load .110-.112 ms unchanged,
checksums equal. Sources, binaries, compiler remarks and results are under
/tmp/vc3-depth-validation-bench. This is a CPU microbenchmark, not a measured
whole-game FPS improvement. No platform intrinsics or game-specific policy.

vulkan_guest_layout_test passes with NaN, +/-infinity, negative subnormal and
just-above-one values at every position in the32-element input, each preserving
all guest bytes; negative zero remains accepted with identical output. Shared
backend full-memory tests pass with validation/sync validation requested
(/tmp/vc3-depth-validation-backend.log), plus Mac main and Android offscreen
builds. Remaining performance work still includes frame pacing and larger GPU
submission/roundtrip cost; current local evidence does not justify a smooth60FPS
claim or Quest acceptance.

Final pair details: compute late windows are32.98/33.05/32.80 FPS. All three
captured frames(1200/1680/2160) differ in this pair; frame1200 visually shows the
same scene with a different hand/animation pose. This does not prove whole-game
parity or establish the cause of every pixel difference. Exact shared fixtures
remain the correctness evidence. Current HEAD includes the CPU validation win;
no game-FPS claim is made for that CPU-only change yet. All sessions terminal.

## Depth comparison profile and native D24 reference (2026-11-01)

A batch-local redundant-roundtrip experiment passed exact shared fixtures
(40 draws,15 skips across EQUAL/NEVER/alphaNEVER/stencilNEVER, all4 modes), but
/tmp/vc3-skip-unchanged-on completed2400 frames with403131 conversions and ZERO
skips. Removed that runtime optimization and diagnostic override; its patch is
/tmp/vc3-skip-unchanged.patch, immutable tested binary /tmp/vc3-skip-unchanged-vc3.
Do not claim a game speedup. Retained only a useful shared profile histogram:
ChVkTargetResourceStats.depth_roundtrips_by_compare counts batched full-surface
conversions in VkCompareOp order; backend reports live-target totals.

/tmp/vc3-depth-histogram completed1800 frames, exit0, on KosmicKrisp. Counts:
LEQUAL256792, ALWAYS8993, others0. Thus96.6% are LEQUAL, and an ALWAYS-only
fragment rounding optimization would address little of the main cost. Combat
windows1321/1441/1561:21.13/24.66/24.72 FPS. This includes the previously committed
CPU validation improvement; it is not an isolated measurement of that change.

Added standalone vulkan_depth24_check using the existing depth24 fixture without
EGL/OpenGL. KosmicKrisp skips (D24 unsupported); local lavapipe/llvmpipe passes
65536 exact packed words plus native resident-vs-roundtrip behavior. Crucially,
a new explicit counterexample verifies DIFFERENT semantics: writez=.5 with
ALWAYS, then drawz=.5 withEQUAL. Native D24 passes (green); current D32 post-draw
rounding fails (red). /tmp/vc3-depth-contract-lvp.log passes with validation and
sync validation requested, no VUID/SYNC diagnostics. This is Vulkan native-format
reference evidence, not console hardware proof. Do not treat the old D32 CPU
roundtrip oracle as automatically interchangeable with native D24 comparisons.

/tmp/vc3-native-d24-lvp completed1800 frames, exit0, using the same immutable
/tmp/vc3-depth-histogram-vc3 with software Vulkan ICD
/opt/homebrew/share/vulkan/icd.d/lvp_icd.aarch64.json. All conversion histogram
counts are0 (native D24 path). Combat windows1321/1441/1561 are
40.37/42.46/43.27 FPS, p95=26.81/25.17/25.14ms. Frame1680 visually shows the
expected active combat scene; no cross-driver exact-frame parity claim. This is
a different driver/device type, not an isolated code A/B or headset result.

Mac main and Android offscreen builds pass. No headset/ADB/device activity.
All sessions are terminal. Next: investigate an opt-in D32 fragment-depth path
against native D24 as the reference (all8 comparisons, ties, stencil, queries,
interpolated depths). Rounding incoming depth before the hardware comparison may
match native fixed-point behavior and remove per-draw full-target copies, but it
is NOT implemented and must not silently replace the legacy D32 contract. The
semantic distinction is now executable test evidence. Goal remains open: no
smooth60FPS/Quest acceptance claim.
