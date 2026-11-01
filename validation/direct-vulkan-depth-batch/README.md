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
