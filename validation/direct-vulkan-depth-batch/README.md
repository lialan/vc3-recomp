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
