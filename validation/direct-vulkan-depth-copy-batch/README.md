# Batch source draws with depth copies

Warm DXT-offloaded control completed2100frames with the populated cache. Prior detailed slow-window trace counted675 source batch boundaries caused by depth copies. Each copy previously completed the source, completed the destination, then submitted/waited for a separate copy.

When source has a pending batch and destination is idle, append the depth/stencil copy after source rendering and complete them with the source fence. Shared recording helper restores both depth layouts. Destination has no unsubmitted accesses, all earlier operations are synchronous, and both images stay alive until return. Busy destinations, self copies and empty sources retain the old ordering. Color layout handling and sample/resource retirement remain in batch completion. New combined_depth_copies counter proves path selection in the fixture. No asynchronous lifetime or guest visibility change.

Native build passed. Focused KosmicKrisp synchronization validation passed both idle and busy destination cases with exact depths/stencils/colors, signaled fences, subsequent equality queries, and invalid bounds/extents. Existing depth-copy fixture moved to its own reusable standalone target while remaining in the offscreen suite. Corrected two stale fixture assumptions: explicitly choose float depth for exact float assertions, and reject short packed input instead of expecting packed uploads to be unsupported (GPU packing now supports them). Texture/cache/guest coherence suite passed. No VUID/SYNC diagnostics. No broad cross-driver matrix.

| Window | Control FPS | Candidate FPS | Control p95 ms | Candidate p95 ms |
|---|---:|---:|---:|---:|
| 1321..1441 | 60.04 | 60.13 | 22.12 | 23.66 |
| 1441..1561 | 59.99 | 60.03 | 23.15 | 22.44 |
| 1561..1681 | 56.39 | 59.89 | 22.33 | 20.6 |
| 1681..1801 | 60.17 | 60.03 | 22.2 | 21.36 |
| 1801..1921 | 59.86 | 59.89 | 20.64 | 21.6 |

Both2100frame runs reachlimit/exit0, same populated shader cache68774076bytes saved successfully. Frame profiling only, detailed costs/GPU timestamps disabled; dirty depth pooling explicitly enabled (still opt-in). Timing differences are not isolated from external load and are not proof of repeatable speedup or steady60. Fixture proves removal of a separate copy submission for eligible sources; frame-only run does not count actual-game combined copies. Binary/tmp/vc3-depth-copy-vc3, control/tmp/vc3-dxt-gpu-warm, candidate/tmp/vc3-depth-copy-warm. All processes terminal.
