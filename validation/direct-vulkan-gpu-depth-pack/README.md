# Shared GPU D32 guest-depth conversion

D32 depth/stencil transfers can now import/export native guest Z24/S8 words through compute. The shader uses the existing exact pack/unpack arithmetic, processes four pixels per invocation to own complete stencil words, and handles odd pixel counts without races. Packing runs after depth copies in the existing submission; paired color/depth reads still complete in one submission. Uploads finish previous batch users before overwriting staging. Barriers cover host, transfer and compute access. The helper borrows the target pipeline cache, so compilation participates in persistence.

The shared runtime defaults this on for compute-capable graphics queues. CHIHIRO_VULKAN_GPU_DEPTH_PACK=0 retains the float-transfer CPU control. Standalone backend callers opt in through gpu_depth_pack. Native D24 retains its existing integer path. Guest Morton/pitched storage copies and CPU edit merging retain their semantics; bulk D32 numeric conversion is removed from the CPU. The shared staging allocation grows from five to roughly nine bytes per pixel to hold both transfer and guest representations.

## Validation

KosmicKrisp and lavapipe pass exact packed roundtrips, float-to-Z24 conversion against a double-precision oracle, all stencil bytes, 1x1/3x5/17x9/512x512 dimensions, repeated staging reuse, and paired pending depth/color readback with a one-submission assertion. The render-texture backend fixture enables GPU packing for its quantized-depth cases and preserves exact guest storage, including CPU edits and alias/coherence paths. Existing fragment depth comparisons pass on both drivers. No VUID or synchronization-validation diagnostics. Both native macOS and Android offscreen builds pass; no headset use. Frame1740 of the repeat candidate was visually inspected as E.S. combat.

## Warm performance

Same immutable executable and populated cache copies; all runs2100frames, Apple M4 Pro/KosmicKrisp, mono640x480, sound disabled, 60FPS cap, GPU vertex programs enabled. Initial pair control then GPU, repeat GPU then control. Cold population is separate.

| Pair | Mode | Activation FPS | Early held FPS | Later held FPS | Held p95 ms |
|---|---|---:|---:|---:|---|
| First | CPU depth |51.12|55.35|53.65|22.21 / 23.27|
| First | GPU depth |57.49|60.30|60.18|21.06 / 21.09|
| Reverse | CPU depth |51.31|57.46|56.88|20.67 / 22.30|
| Reverse | GPU depth |58.24|60.33|59.65|20.37 / 21.41|

Retained: activation and both held windows improve in both comparisons. Sustained throughput is approximately60FPS, but activation still misses60 and frame intervals remain nonuniform. This does not close the user's steady warm-cache60FPS requirement. Numbers describe guest completion, not headset display pacing. Full provenance in measurements.json; raw logs /tmp/vc3-gpu-depth-{populate,control,candidate,repeat-candidate,repeat-control}/.
