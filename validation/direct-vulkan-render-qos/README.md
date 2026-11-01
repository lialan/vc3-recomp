# Executor profiling disable and optional render-thread QoS

2026-11-01, native local Vulkan only. No headset/device operation. One native build and one combined benchmark pair; no graphics fixtures repeated because this bundle changes scheduling/profiling, not rendering semantics.

Shared executor now honors explicit RECOMP_PROF=0. Previously any present value enabled per-batch clock reads and reports. Presence-style other values remain supported. The2100frame control emits PROF reports with1; candidate has no PROF reports with0. This is a retained configuration fix, not a claim that profiling caused the entire pacing issue.

Shared Vulkan macOS callbacks optionally configure the actual rendering thread once per thread. CHIHIRO_VULKAN_RENDER_QOS unset leaves scheduling unchanged;0 reports only;1 requests user-initiated;2 requests user-interactive. Uses SDK pthread QoS APIs, without realtime policy, affinity or spinning. Successful requested QoS applies for the thread lifetime; no restoration on backend destruction or change on other platforms. Default remains unchanged. In this run getter/setter verification succeeded: control class21->21, candidate21->33. Mode1 avoids lowering an already interactive thread.

Same binary/tmp/vc3-render-qos-vc3, same starting pipeline-cache copies,2100frames each, both exit0. Both use dirty pooling. Control QoS0 + executorPROF1 reproduces previous scheduling/profiling; candidate QoS2 + executorPROF0 combines the two changes. This pair cannot isolate either contribution.

| scene | control FPS | candidate FPS | p95 control/candidate ms |
| --- | ---: | ---: | --- |
| activation | 57.76 | 59.80 | 20.88 / 20.60 |
| held early | 60.14 | 60.16 | 20.69 / 21.40 |
| held late | 59.99 | 59.12 | 19.91 / 21.83 |

Mixed tails do not justify a QoS default change; keep opt-in. No isolated-GPU or steady60 claim. Normal stays~60. No pixel/visual acceptance claim added for this scheduling experiment. Reports preserve binary/env/capture hashes; rawlogs/tmp/vc3-render-qos-{control,candidate}/game.log.

Remaining measurement issue: CHIHIRO_VULKAN_PROFILE=1 is still required for frame interval reports and enables per-draw backend/NV2A timers. Next bundle should separate frame-only reporting from detailed profiling so acceptance can run with all per-draw diagnostics disabled, then inspect paced-vblank/flip timing rather than infer display stability from average FPS. Keep bundled validation cadence.
