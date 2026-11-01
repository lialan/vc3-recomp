# Queue independent resident readbacks before waiting

Shared target readback now supports begin/end. Begin records and submits depth plus optional color transfer, borrowing CPU output buffers without filling them. End waits, retires batch resources, invalidates mapped storage and copies results. The resident-owner completion path observes all CPU depth edits first, submits the disjoint retained owners, then drains and publishes every depth result. Color snapshots retain the existing serial check and CPU-edit reconciliation before guest publication. Pending draws and query/semaphore completion remain fenced. On a later begin error, already-submitted requests are drained. No operation on a borrowed target is permitted between begin/end; destruction drains a live request before releasing target storage.

Retained depth owners are already bounded and nonoverlapping. GPU texture snapshots keep their sampling leases until retirement. The GPU work and guest simulation clock are unchanged. The finish-duration metric now includes the submission-to-retirement interval, potentially overlapping other targets; do not sum it as CPU time.

Focused KosmicKrisp synchronization validation passed depth packing/residency and render-texture/guest-memory parity. Added checks cover output visibility only at end, rejected duplicate begin/end, and two independently queued dirty owners including a paired color read. Existing coverage exercises odd dimensions, D32/D24, linear/Morton layouts, CPU component edits, alias publication, resize, sampled-image ownership and strict-vs-GPU guest bytes. Native game and Android Vulkan library builds passed. One public-function signature compiler error was corrected before execution.

One sequential warm-cache local A/B used the preceding mapped-staging binary and this candidate, identical profiling and isolated copies of the same pipeline cache. Both completed 2,100 frames and saved the cache. Candidate reported 7,700 queued readbacks, zero CPU geometry/color/depth conversion and a flat warm CPU texture fallback counter. No headset access. This is one profiled pair, not an isolated/repeated hardware benchmark.

| Zero-pipeline-creation window | Completion before ms | After ms | CPU before ms | After ms |
|---|---:|---:|---:|---:|
| Combat | 4.812 | 4.442 | 1.573 | 1.541 |
| Slow-motion activation | 4.917 | 4.465 | 1.802 | 1.812 |
| Slow-motion held | 5.032 | 4.597 | 1.820 | 1.808 |

Completion decreased about 8–9 percent while CPU time was similar and GPU timestamp totals slightly increased. This supports reduced waiting/submission gaps, not faster shaders or a proven Quest FPS gain. Mean frame intervals remain near the 60 Hz cap. Full metrics are in comparison.json.

Five captured frame hashes differ. Visually inspected frame 1840 in both runs: same combat composition and slow-motion overlay, differing animated trace/effect positions. No bit-exact game-capture or full visual-parity claim; exact deterministic fixture parity passed. Runs /tmp/vc3-queued-readbacks-{control,candidate}; binary /tmp/vc3-queued-readbacks-vc3.

Next significant finding: ordinary shared runtime startup still defaults resident depth, quantized depth, multiple depth owners and GPU render-target sampling off, whereas benchmark environments enable them. Address the default configuration and validate without performance overrides before claiming packaged builds use these paths. Installed Quest checkpoint unchanged.
