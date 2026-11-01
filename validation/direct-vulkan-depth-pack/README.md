# Rejected binary32 CPU depth packing experiment

The retained backend still spends sampled CPU time in ch_vk_guest_depth_store. This experiment replaced its Z24 double-precision conversion with the existing exact ch_vk_depth_pack helper, retaining full input validation and unchanged Z16 semantics. No game-specific behavior was introduced.

Correctness: the exhaustive CPU oracle passed all 16777216 Z24 normalization codes and every nonnegative binary32 input through 1 against the double guest-store and distinct attachment oracles. The KosmicKrisp render-texture integration fixture also passed with synchronization validation. No Android build or headset run was performed for this rejected change.

Two immutable binaries, identical warm starting caches and 2100-frame local headless scenario; both completed successfully. Candidate was run first. Mono, sound disabled, 60 FPS cap, Apple M4 Pro / KosmicKrisp.

| Window | Retained double FPS | Binary32 FPS |
|---|---:|---:|
| Activation | 41.32 | 41.92 |
| Early sustained slow motion | 50.94 | 51.25 |
| Later sustained slow motion | 54.89 | 53.37 |

Mixed throughput and worse later-window p95 (20.66 -> 21.63 ms) do not support retaining this change. Runtime changes reverted; no claim that binary32 is inherently slower. Saved patch: /tmp/vc3-depth-pack-rejected.patch. Raw runs: /tmp/vc3-depth-pack-{binary32,double}/report.json. No acceptance claim from the separate sampled run. Further work should revisit shared vertex execution and transfer costs using fresh evidence.
