# Frame-only timing without per-draw diagnostics

2026-11-01, local simulator/headless Vulkan only. Base50774cb. No headset or device operations. Native build plus one comparison and one bounded existing-pacing probe; no unrelated rendering fixture matrix repeated for a reporting-only change.

CHIHIRO_VULKAN_FRAME_PROFILE=1 now enables frame-interval aggregation independently of CHIHIRO_VULKAN_PROFILE. Existing detailed-profile and frame-cost behavior still enables interval reporting. Frame-only mode records one clock sample per completed frame and reports120-frame windows; it does not enable per-draw backend/NV2A phase timers or periodic CPU-graphics audits. GPU timestamps, executor profiling, pipeline logs and frame tracing were disabled/removed. Explicit profiling flags are preserved in reports. The local runner also records RECOMP_FLIP_PACING and RECOMP_VBLANK when supplied.

Same binary/tmp/vc3-frame-only-vc3, matched populated cache copies,2100frames perrun, pool enabled, QoS unchanged. Control per-draw VulkanPROFILE1 versus candidatePROFILE0+FRAME_PROFILE1. Both exit0 and emit17interval windows. No executor/frame trace reports in either; no backend/draw/frame-cost reports in frame-only candidate. This validates reporting separation, not zero CPU emulation counters (those audits are disabled here; prior correctness/audit evidence remains).

| scene | profiled control FPS | frame-only FPS | frame-only p95 ms |
| --- | ---: | ---: | ---: |
| normal early | 60.00 | 60.03 | 20.68 |
| normal late | 60.03 | 60.00 | 21.12 |
| activation | 59.90 | 59.88 | 19.74 |
| held early | 59.97 | 60.01 | 19.77 |
| held late | 60.04 | 59.98 | 19.73 |

A separate bounded run of the existing shared RECOMP_FLIP_PACING=1 (RECOMP_VBLANK=paced) gives activation59.88FPS and held59.99/60.01, p9520.09/19.60/19.73ms. Hold diagnostics confirm it actually held flips, with no timeouts/clock stops. It does not materially resolve the completion tails; leave default off. The backend completion timestamp precedes the executor hold, so this is not display-cadence acceptance. Do not introduce frame skipping or change the target to average FPS.

Steady60 is still unproven. No isolated-GPU claim. Reports preserve environment/cache/binary provenance. Rawlogs/tmp/vc3-frame-only-{control,candidate}/game.log and/tmp/vc3-flip-paced/game.log. Future acceptance runs should use FRAME_PROFILE1, PROFILE0, FRAME_COSTS0, GPU_TIMESTAMPS0, RECOMP_PROF0 and unset RECOMP_FRAME_TRACE. Keep detailed profiling for diagnosis only. Next pursue substantive submission/coherence or CPU command-preparation costs using this lower-overhead baseline; QoS and existing flip hold did not establish a pacing fix.
