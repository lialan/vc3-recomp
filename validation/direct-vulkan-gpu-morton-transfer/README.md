# GPU Morton color and depth transfers

Local Vulkan only: Apple M4 Pro / KosmicKrisp for game timing, KosmicKrisp and lavapipe for exactness and synchronization. 640x480 mono headless, sound disabled, DMA pushbuffer walking. No headset or display-rate acceptance.

## Shared implementation

Full rectangular power-of-two ARGB color transfers now swizzle/unswizzle and permute channels in GPU compute. Linear and Morton planes are disjoint to avoid cross-invocation overwrite races. Paired color staging grows from 4 to 8 bytes per pixel; the main staging allocation already had sufficient space for depth conversion. Cropped/sparse Morton allocations retain reference handling.

Packed guest Z24S8 transfers now execute on compute for both D32 and native D24. Full Morton depth transfers fuse layout conversion with depth/stencil packing; resident guest publication/import becomes a byte copy. CPU component edits retain the GPU-written component, including Morton storage, target copies, and layout changes. Float diagnostic APIs retain their linear contract.

Color, depth and texture shaders share `morton.glsl`, using masked bit spreading instead of a per-bit loop. All changes are generic runtime/backend operations, with no title addresses or scene assumptions. The runtime enables GPU paths on a compute-capable graphics queue. Diagnostic controls: `CHIHIRO_VULKAN_GPU_MORTON_COLOR=0`, `CHIHIRO_VULKAN_GPU_MORTON_DEPTH=0`; existing GPU depth/color controls remain available.

## Validation

Native and Android builds pass; Android is build-only. Exact arbitrary-byte color roundtrips and random packed depth/stencil roundtrips cover 1x1, 1x8, 8x1, 4x16, 16x4, 128x128 and 256x256, repeated staging reuse, paired color/depth publication, CPU stencil edits, GPU image copies and layout transitions. Native D24 cases run only where the driver supports that format. Existing odd-size depth and rendering cases are retained.

Both local drivers pass the depth/color fixture, render-target/texture coherence fixture and 96 exact texture pixel/mip cases with synchronization validation enabled. Saved logs contain no validation errors or synchronization hazards. Game captures show active E.S. combat; captures and binary hashes are recorded in runner reports.

## Audit correction

The previous graphics audit's approximately 0.22 ms/frame estimate omitted CPU depth layout conversion. New explicit depth counters identified 65,536 Morton depth pixels per frame in normal combat, activation and held slow motion. `depth-before-audit.txt` records that discovery. It was not valid to infer absence of CPU depth swizzling merely from absence of numeric depth conversion in the sampled profile.

After the depth port, cumulative CPU color conversion, resident depth layout/float conversion, strict depth conversion, bulk geometry batches/vertices and GPU raw declines are zero in the tested 2,100-frame run. Warm rare texture decoding and the legacy metadata vertex remain; compatibility paths still exist for other formats, cropped Morton views and context-writing programs. Startup texture fallback is larger and is not included in the warm-scene claim. Guest instruction execution, command handling, index assembly, copies/comparisons, guest write tracking and Vulkan submission/waits remain CPU work.

## Timing interpretation

The initial color-only comparison encountered additional pipeline cache misses (up to about 200 ms), so it is retained as evidence, not clean warm-cache acceptance. Expanded-cache color-only runs show about 60 FPS combat/held averages with activation around 59 FPS and p95 about 21-22 ms. Removing CPU graphics emulation does not establish steady 60 FPS. The final same-binary comparison and counter deltas below are the authoritative results for this patch.

The final same-binary depth-control pair uses identical copies of an expanded pipeline cache, with GPU Morton color enabled in both. Depth control keeps CPU Morton layout; candidate enables GPU Morton depth. Results (FPS):

| Run | Combat 1 / 2 | Activation | Held 1 / 2 | Held p95 ms |
|---|---|---|---|---|
| Populate |60.05 / 60.01|59.37|60.01 / 60.02|20.27 / 20.45|
| CPU depth layout |59.97 / 59.92|56.44|55.76 / 60.77|21.37 / 20.58|
| GPU depth layout |60.01 / 59.53|52.25|57.37 / 60.22|22.76 / 20.82|

These are **not isolated throughput acceptance**: another local game process was observed running concurrently, starting 14:45:18 local time, along with substantial WindowServer/loginwindow CPU load. See `concurrent-load.txt`. Individual pipeline creations in combat/slow-motion were under 2 ms in this pair, so the degraded averages cannot simply be dismissed as long shader compilation. No sustained-performance improvement is claimed from the noisy pair. Earlier runs also show frame-time tails above 16.67 ms. A quiet-host repeat is still required; do not claim steady 60 FPS.

## Final source audit

`commit-report.json` identifies the final binary (including the fast return for unchanged depth layout) and complete environment. `commit-audit.json` contains cumulative counter deltas at exact frame boundaries 1320/1560/1680/1920. `commit-audit.txt` retains counters and executor timings. This run also overlaps the other game and is not isolated FPS acceptance.

| Scene | CPU texture decode ms/frame | CPU color/depth conversion or layout pixels/frame | CPU geometry vertices/frame |
|---|---:|---:|---:|
| combat | 0.005479 | 0 | 0 |
| activation | 0.007475 | 0 | 0 |
| held | 0.000833 | 0 | 0 |

The held metadata reports are saved separately; this is the remaining single CPU vertex used for legacy batch metadata, not bulk vertex execution. CPU graphics emulation is insignificant in these tested warm scenes, not literally absent across all supported formats/games. The 60 FPS pacing objective remains open.
