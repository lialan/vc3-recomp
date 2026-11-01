# Shared bulk ARGB/XRGB decode specialization

The shared executor bulk texture decoder now selects native ARGB/XRGB word handling outside the per-pixel loop. Pitched linear and rectangular Morton inputs retain the existing address calculation; XRGB forces alpha to 255. Other formats keep the previous decoder. No game addresses, scene heuristics or Vulkan-only decoding semantics are introduced.

## Evidence and validation

A fresh retained-build profile completed 2100 frames: /tmp/vc3-reuse-retained-profile/. Sampling began after completed frame 1560 for 2 seconds and after frame 1680 for 3 seconds. cpu-profile.json records the profiler's collapsed TOP-of-stack counts, not sums of inclusive call paths (which can double-count). decode_texel had 39 activation and 33 held samples; nv2a_vsh_run 56/92 and guest depth store 47/66. Samples are diagnostic, not GPU timings or acceptance FPS.

The existing coordinate sampler is the independent scalar oracle. All 2304 format/layout/palette cases pass in the normal CPU fixture and ASan/UBSan build, covering linear, Morton, rectangular, odd dimensions, alpha and unsupported-format status. Native 512x512 swizzled ARGB bulk decode measured 0.146 ms in the normal fixture; scalar sampler 3.595 ms. This is a CPU diagnostic, not an end-to-end speedup factor. macOS guest executable and Android offscreen build pass. No headset used.

## Controlled local runs

Apple M4 Pro/KosmicKrisp, 2100 frames per run, 60 FPS cap, mono headless, sound disabled. Warm identical starting pipeline cache copies. Two immutable binaries; first control then candidate, repeat reverses order. All four runs completed successfully.

| Pair | Variant | Activation FPS | Early held FPS | Later held FPS |
|---|---|---:|---:|---:|
| First | Control | 41.88 | 51.90 | 54.76 |
| First | Specialized | 43.19 | 53.21 | 55.96 |
| Repeat | Control | 42.29 | 53.10 | 55.07 |
| Repeat | Specialized | 42.64 | 54.05 | 54.85 |

Activation and early held throughput improve in both pairs; later held is flat/slightly lower in the repeat. First later p95 worsens from 21.00 to 22.11 ms, whereas repeat later improves from 22.47 to 22.02 ms. Retained as a modest overall throughput gain with no claim of uniform frame-pacing improvement. Normal combat remains near 60 FPS; slow motion is not consistently 60 FPS. Raw /tmp/vc3-argb-bulk-{control,candidate}/report.json and /tmp/vc3-argb-bulk-repeat-{control,candidate}/report.json; full provenance in measurements.json.
