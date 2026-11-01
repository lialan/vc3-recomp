# Opt-in clean depth-image reuse

2026-11-01, local Vulkan simulator/headless only. Runtime checkpoint06c942d. No headset or device operation. Generic shared backend feature; no title addresses or scene conditions.

Switching away still observes CPU edits and publishes dirty depth. The clean GPU image and its exact published guest snapshot can be reused on return only if target identity, dimensions, format, pitch, guest address, span and all guest bytes match. This avoids a redundant upload without changing guest publication boundaries. Partial overlaps or CPU edits force import; GPU copies into a dormant target invalidate its saved key. Public release drops every borrowed target before resize/destruction/external mutation. Four slots and16MiB bound raw snapshots; no extra Vulkan image allocation. Existing float/packed coherence paths remain.

CHIHIRO_VULKAN_REUSE_CLEAN_DEPTH=1 enables it. **Disabled by default**: reduced uploads are verified, repeatable FPS improvement is not. Keeping the experiment opt-in avoids treating a successful component counter as performance acceptance.

Exact owner tests pass on KosmicKrisp and lavapipe with synchronization validation: nativeD24 and quantizedD32, linear and rectangularMorton, unchanged reuse, CPU edits while dormant, GPU-copy invalidation, overlapping guest publication, and release before external target mutation. Added final release test passes bothdrivers. Existing render-texture/coherence fixtures also pass with their default configs; they are not a claim of full cached-owner backend coverage. Native VC3 and Android offscreen build; Android build-only. Active E.S. frame1740 visually inspected.

Same-binary matched-cache control/candidate then reverse,2100frames each, all exit0. Detailed per-frame/GPU timestamps off; periodic profiling remains on. Both controlruns imports5315; both candidates4267 with1048cleanreuses (19.7% fewer imports). Warm windows remove0.5upload/frame: normal600->480, activation300->240, held600->480. Dirty writeback count is not reduced. Per-target CPU allocations and snapshot comparison remain.

| pair | activation control -> candidate FPS | held control -> candidate FPS |
| --- | --- | --- |
| first | 56.41 -> 59.70 | 60.14/60.14 -> 60.07/59.62 |
| reverse | 58.65 -> 56.87 | 60.26/59.77 -> 58.38/60.35 |

First activation p95 improves22.07->19.43ms; reverse worsens21.45->23.50ms. Held tails mixed. No steady60 or throughput-win claim. Another project has continued running a GPU workload in this session; do not infer that variability is entirely cache policy or entirely contention. No external processes stopped. All four run reports record binary SHA256, environment and capture hashes. Raw logs remain /tmp/vc3-clean-depth-<run>/game.log, binary /tmp/vc3-clean-depth-vc3.

Next: this experiment proves some uploads are redundant but leaves all dirty owner-switch readbacks. Evaluate whether disjoint dirty images can remain resident until an actual guest completion or overlapping consumer, with explicit publication/alias/CPU-edit tests. Avoid enabling this clean-cache feature by default solely because imports decreased. Warm60 goal remains active.
