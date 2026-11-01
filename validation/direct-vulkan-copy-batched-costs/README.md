# Current completion-tail diagnosis

One2100frame detailed run of37d1839, /tmp/vc3-texture-copy-vc3, populated /tmp/vc3-dxt-gpu.cache, exits0/reacheslimit and saves68774076bytes. Local simulator only. Profile/cost/GPU timestamps enabled, dirty pooling explicitlyenabled. This is diagnosis, not acceptance or isolated A/B. Another ghostsquadevolution process in the separate gse-recomp-hotd4-steam-frame worktree was observed active (PID56107,35.7%CPU snapshot); left running as instructed. GPU timestamp durations may include contention and do not isolate intrinsic shader cost.

Shared summarizer now partitions zero-pipeline-creation frames at a configurable --tail-ms threshold (20default), retaining all-frame aggregates. Current partitions verified to cover234combat/115activation/239held frames exactly. Timers overlap and must not be added; no claim zero-creation means all first-use work absent.

| Activation mean ms | <=20ms frames (105) | >20ms frames (10) |
|---|---:|---:|
| Interval |16.245|21.225|
| Draw wall |8.209|11.120|
| Draw CPU |3.606|3.835|
| Completion wall |5.174|6.735|
| Completion CPU |1.872|2.016|
| Batch wait |7.148|10.498|
| Batch GPU |4.584|7.492|

Held239frames: completion mean5.721wall/1.966CPU; long44frames completion7.602 versus5.297ms inshort195frames. Other/unattributed mean5.182 vs2.736ms, includes guest execution/idle/scheduling and cannot be treated entirely as rendering. BatchGPU5.509 vs3.703ms. Combat tails also have substantial other time7.553 vs4.429ms.

WarmCPUtexture counter remains1030144pixels/6.562ms, with zero geometry/color/depthconversion counters in1320..1920 samples. Remaining activation tails are not explained by a0.22ms CPUtexturefallback. Data points to GPU completion/waiting plus outside-backend scheduling; no isolated rootcause claim under external GPU work.

Next substantive opportunity: publication currently completes/readbacks each retained owner synchronously. Investigate submitting independent readbacks before waiting, preserving overlap/alias ordering, guest-write observation, staging lifetime and semaphore visibility. Need explicit begin/retire API and exclusion of overlapping guest ranges, not simply removing fences. Avoid repeating blind QoS/idle/staging sweeps already rejected. Steady60 remains unproven.
