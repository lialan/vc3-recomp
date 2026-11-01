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
