# Post-build matched-cache comparison

User reported the heavy build finished. Four local 2100-frame Vulkan runs, control/candidate then candidate/control. Same executable SHA256 and scene inputs, each starting from a fresh copy of /tmp/vc3-thread-costs.cache. Detailed FRAME_COSTS and GPU_TIMESTAMPS disabled; periodic profiling remains enabled (RECOMP_PROF=0 is presence-based in the executor, so it does not disable that report). No headset or device operation. All runs exit0 and reach2100frames.

| pair / policy | normal FPS | activation FPS | activation p99 / max ms | early / late held FPS |
| --- | --- | ---: | --- | --- |
| first control | 60.00 / 59.98 | 58.87 | 31.69 / 36.10 | 59.97 / 60.55 |
| first candidate | 60.00 / 59.96 | 56.96 | 23.87 / 32.37 | 58.44 / 50.82 |
| reverse candidate | 59.97 / 59.98 | 58.79 | 25.86 / 26.05 | 59.01 / 59.96 |
| reverse control | 60.02 / 59.93 | 58.94 | 37.27 / 43.17 | 59.98 / 60.03 |

Candidate reduces activation p99 and maximum in both pairs and eliminates >33ms activation frames (control1/2; candidate0/0), consistent with eliminating eviction bursts. It does not establish a mean-throughput improvement; activation averages slightly lower, held results vary, and late-held first candidate is substantially lower. Keep this distinction explicit. Cache total evictions increase293->826 in the first pair while uploads remain2945->2948: proactive destruction moves work and can increase churn. Current default is retained for the repeatable activation-tail improvement, not as a verified overall performance win. Other-game working-set coverage and isolated verification remain open.

Heavy build load is gone but another project was running ghostsquadevolution on the GPU during these tests (PID88395 initially,90800 later). It was left untouched. These are lower-contention matched-policy comparisons, not exclusively isolated GPU measurements. The process listing includes WindowServer/browser activity. Do not claim remaining differences are caused solely by this policy or solely by contention.

Steady60 remains unproven: reverse candidate activation58.79FPS and held59.01/59.96FPS, heldp9520.36/19.99ms. Guest completion timing is not XR display timing. Warm compatibility fallback counters remain recorded; CPU color/depth/bulkgeometry are zero. No runtime policy changes made during these measurements. Next work remains reducing depth readback/import synchronization while preserving guest-memory coherence. Full raw logs/caches remain /tmp/vc3-quiet-<name>{/game.log,.cache}; reports include binary hash, environment and capture hashes.
