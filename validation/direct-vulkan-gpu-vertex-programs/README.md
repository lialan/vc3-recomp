# Shared Vulkan vertex programs

Native Vulkan now accepts NV2A_RAW_PROGRAM geometry. The canonical 136-slot NV2A instruction encoding is specialized into explicit shader steps, with all per-draw constants snapshotted into leased geometry storage. Fixed-function geometry keeps its original vertex stride and constant upload size. Program attributes are unpacked only for registers named by the program. Triangle-list/strip/fan topology, stage coordinate scales, color clamps, fog, +0 w handling and clip distances feed the existing shared fragment pipeline. No game addresses or scene classifications are used.

The default local Vulkan host offers RECOMP_GPU_XF=all. RECOMP_GPU_XF=ff remains the CPU-program diagnostic control. This moves bulk vertex execution and clipping to the GPU; it does not eliminate all CPU graphics processing. The executor still evaluates one vertex for legacy batch metadata, and deliberately retains context-write programs and unsupported raw offers on its reference path. Texture decoding and guest color/depth conversion remain further work.

## Validation

- 144 programs x 256 vertices compared against nv2a_vsh_run on KosmicKrisp and lavapipe, with 2e-5 relative/absolute tolerance for finite results and matching NaN/infinity classifications. Covers all 128 paired MAC/ILU opcode combinations, relative constants, masks, swizzles, invalid temporary/constant indices and ignored constant writes with context-write disabled. An initially found LIT zero-base/NaN-exponent mismatch was fixed before the passing runs.
- Registered-backend rendering checks indexed strips/fans, exact solid colors, precise visibility queries, per-draw constant snapshots, changed instruction words with identical caller hashes, +0 w semantics, negative w and near clipping, and unsupported bounds. No VUID or synchronization validation errors on either driver.
- Native macOS guest and Android offscreen builds pass. Android build only: no headset use.
- The first loop-based specialization experiment was slower and replaced by explicit specialized steps. Cold pipeline compilation is intentionally reported separately from warm-cache acceptance.

## First matched warm-cache result

Apple M4 Pro / KosmicKrisp, mono 640x480 headless, sound disabled, guest cap60, 2100 frames. Same immutable executable, identical populated pipeline-cache copies. GPU-program run followed by CPU-program control; all completed. Runtime shader constants and sampled guest content still vary by game execution, so repeat-order and broader scene checks remain required.

| Window | CPU programs | GPU programs | CPU/GPU p95 ms |
|---|---:|---:|---:|
| Normal combat 1321..1441 |60.25|60.00|22.54 / 22.67|
| Normal combat 1441..1561 |60.09|59.97|22.19 / 24.61|
| E.S. activation 1561..1681 |40.77|55.77|29.48 / 22.54|
| Early held 1681..1801 |50.78|57.88|25.47 / 20.74|
| Later held 1801..1921 |52.91|60.03|24.05 / 20.32|

This is a throughput improvement, not steady60FPS acceptance. Activation and early held still miss the target; later held meets the average but has nonuniform frame intervals. User accepts cold compilation stalls and requires steady warm-cache60FPS. Cold population, full provenance and raw window metrics are in measurements.json. Local /tmp/vc3-gpu-vp-{populate,warm,control}/ contains logs/captures. No Quest/VR performance claim.

A separate 2100-frame diagnostic run sampled four seconds after frame1560. Collapsed CPU self-stack counts: guest depth store97, color swap84, texture decode53, texture replace48, depth transfer38, guest depth load25; vertex interpretation is below the five-sample reporting threshold. These are sampling counts, not percentages of frame time. The next GPU port should address depth packing/unpacking. Raw /tmp/vc3-gpu-vp-profile/cpu-slow.txt; retained counts in cpu-profile.json. Warm frame1740 was visually inspected and shows E.S. combat, not Game Over.
