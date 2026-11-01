# Calling-thread CPU versus wall-time diagnosis

2026-11-01, local simulator/headless Vulkan, Apple M4 Pro/KosmicKrisp. Base82ac537. Other workloads kept running as explicitly requested. No headset/device operation. Diagnostic code only, no scheduling or rendering policy change.

Optional CHIHIRO_VULKAN_FRAME_COSTS now reports draw/clear/flip/cache-trim calling-thread CPU milliseconds alongside wall milliseconds. CLOCK_THREAD_CPUTIME_ID excludes blocking and descheduling but includes driver CPU work on the caller. It excludes worker threads and is not GPU shader time. Unsupported platforms report thread_cpu_timing=0. Pipeline creation and GPU/wait timers overlap phase timings. Additional timing calls perturb execution; use untraced verification for acceptance.

Measured zero-pipeline-creation subsets:

| scene | frames | draw wall / CPU ms | clear wall / CPU ms | batch wait / GPU ms |
| --- | ---: | ---: | ---: | ---: |
| normal | 234 | 5.329 / 2.632 | 1.611 / .706 | 4.979 / 2.914 |
| activation | 115 | 8.999 / 4.278 | 1.576 / .782 | 6.567 / 4.039 |
| held | 239 | 9.021 / 4.080 | 1.706 / .767 | 7.296 / 4.759 |

These overlapping columns must not be added. Held cache trim averages .099ms wall/.099msCPU. A substantial part of callback wall time is not calling-thread execution. No inference that every gap is avoidable, or solely host contention. Uninstrumented guest execution/raw preparation and pacing remain in other_ms. Filtering creation frames is diagnostic, not proof of a fully warmed scene or of following-frame pacing.

Detailed-run normal60.01/60.05FPS, activation56.18, held52.86/58.04; p95 still20.75-23.54ms. Earlier run without per-frame/GPU timing: normal60.01/59.97, activation42.52, held51.22/42.53. Different concurrent load makes these unsuitable for attributing a tracing or cache-policy gain/regression. A .44-second native rebuild overlapped the earlier run; neither run is isolated acceptance. Vblank logs maintain nominal16.7ms even while executor misses frames. Steady60 remains open.

Validation: native and Android offscreen targets build (no deployment). All2099 per-frame records have supported thread timing; draw phase sums agree within.01ms and CPU time is nonnegative and no larger than wall beyond .5ms timing-boundary tolerance. Actual rendering ran2100frames and exited successfully. Existing exact-render tests were not rerun for this diagnostics-only change.

Reproduce summary for this or another game with shared backend logging:

```
python3 tools/summarize-vulkan-frame-costs.py /tmp/vc3-thread-costs/game.log --window normal:1321:1561 --window activation:1561:1681 --window held:1681:1921
```

Window ends are exclusive. Tool includes all-frame and zero-pipeline-creation subsets, timer availability, and percentile/count metadata. Full report preserves binary hashes/environment; raw log remains /tmp/vc3-thread-costs/game.log. Next examine repeated depth ownership transitions and completion boundaries: held counters show frequent readback/import submissions; preserve CPU guest visibility and aliasing while evaluating reductions. No change to guest clock or unsupported CPU graphics fallback semantics.
