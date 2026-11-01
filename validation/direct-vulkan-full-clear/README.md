# Full-surface and channel-only clear routing

2026-11-01. Base69fc357. Shared Vulkan runtime; local simulator/headless only.

Explicit full-surface clear rectangles, including clamped oversized extents, use the existing aspect-clear path when color is unmasked or absent. This removes rectangle geometry and clear-specific graphics pipelines and permits resident clear batching. Partial channel+depth rectangles keep their single masked draw. Channel-only implicit clears skip the prior no-aspect readback/submission before the masked draw. No game addresses or scene heuristics; partial extents retain existing rendering semantics.

Native build and one focused KosmicKrisp synchronization-validation batch passed. Added 128 mask combinations (16 color masks x4 depth/stencil combinations x2 depth modes), with exact color/depth/stencil checks for full/clamped rectangles and fragment-quantized depth. Two channel-only clears retain one pending batch with zero intermediate submissions. Existing command-limit, fence ownership, exact GPU transfer, pooled ownership and registered texture/coherence fixtures pass. No VUID/SYNC hazards; no cross-driver/Android matrix repeated.

One sequential2100frame comparison with equal populated cache copies and frame-only diagnostics; dirty pooling enabled, QoS unchanged/pacing unset. Both exit0/reach limit. Full provenance in reports; rawlogs /tmp/vc3-full-clear-{control,candidate}/game.log.

| Scene | Control FPS | Candidate FPS | Control p95 ms | Candidate p95 ms |
| --- | ---: | ---: | ---: | ---: |
| Combat early | 59.97 | 60.18 | 21.61 | 20.22 |
| Combat late | 59.99 | 60.07 | 22.78 | 20.17 |
| Slow-motion activation | 59.90 | 59.56 | 20.89 | 21.14 |
| Held early | 60.01 | 60.21 | 21.13 | 20.65 |
| Held late | 59.92 | 60.02 | 20.60 | 19.71 |

The fixture proves eliminated intermediate submissions and exact results. This pair does not establish a repeatable whole-game speedup or steady60. Guest completion intervals are not display cadence; no exclusive-machine claim. Detailed graphics audit is disabled in frame-only runs. Next diagnose remaining completion tails using existing shared thread accounting and guest/executor timing; multiple accepted rendering overhead reductions have left p95 completion jitter. Avoid another blind rendering micro-optimization or repeated benchmark matrix without causal evidence.
