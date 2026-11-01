# Explicit completion and raw preparation costs

2026-11-01, baseb0b1589. Local Vulkan simulator/headless only.

Refreshed diagnostics first showed depth upload waits only~0.15ms/frame in held slow motion, while explicit flush callbacks were hidden inside other_ms. Shared optional FRAME_COSTS now measures completion_ms/completion_cpu_ms/completion_calls around locked_flush and prepare_ms/prepare_cpu_ms around encoded raw snapshot preparation. Flip's internal flush stays in flip_flush_ms, so it is not double-counted. Raw snapshot timing ends before draw timing begins. Both new wall phases are subtracted from other_ms and reset at each frame. Normal profiling-off behavior is unchanged.

Native build passes; one instrumented2100frame run exits0/reacheslimit. All2099frame phase sums match interval within0.006ms and new CPU measurements are nonnegative/no greater than wall beyond0.5ms tolerance. Existing generic summarizer automatically includes new millisecond fields. No rendering fixture matrix for diagnostic-only change.

Zero pipeline-creation subsets (diagnostic filtering, not warm-cache acceptance):

| Scene | Frames | Draw wall/CPU ms | Completion wall/CPU ms | Raw prepare wall/CPU ms | Batch wait/GPU ms | Depth-upload wait ms |
| --- | ---: | --- | --- | --- | --- | ---: |
| Combat | 234 | 4.784 / 2.239 | 4.554 / 1.542 | .501 / .500 | 4.888 / 2.804 | .192 |
| Activation | 115 | 8.259 / 4.001 | 5.048 / 1.959 | .952 / .946 | 6.301 / 3.421 | .154 |
| Held | 239 | 7.711 / 3.869 | 5.206 / 1.998 | .989 / .979 | 6.074 / 3.264 | .152 |

Batch wait/GPU timers overlap callback phases and must not be added to them. Completion includes pending draws, publication transfers and host copies; its full5.2ms is not all removable readback overhead. Raw preparation is encoded byte snapshot/repacking plus topology, not numeric shader emulation. Detailed timing overhead/host variability means this is not a speedup comparison with the earlier uninstrumented run.

Current sampled audit confirms CPU geometry batches/vertices, strict depth pixels, resident depth layout/float pixels and color conversion pixels remain zero. Texture fallback counters remain cumulative and include startup; do not infer zero texture work per frame. Raw preparation nevertheless costs nearly1msCPU during held slow motion: per-vertex padded slots are still assembled on CPU despite GPU numeric unpacking. Next substantive shared optimization: compact encoded attribute-stream snapshots and GPU gathering/default-lane reconstruction, preserving byte-exact inputs, constants, rebased indices, odd strides and lifetime rules. Avoid retrying deferred depth uploads for a0.15ms wait without new evidence.

Full log /tmp/vc3-completion-costs/game.log; binary/tmp/vc3-completion-costs-vc3. Initial refresh/tmp/vc3-current-transfer-costs/game.log. No headset/device operations. Reports preserve flags/hashes. Steady60 remains unproven. All task processes terminal.
