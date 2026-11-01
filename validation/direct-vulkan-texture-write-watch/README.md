# Shared texture write tracking

Shared texture-cache entries observe guest write generations for encoded storage and P8 palettes. Unchanged generations reuse the preceding exact comparison result, including a mismatch against an old pinned snapshot. Missing trackers, active write leases and invalidated pages retain exact comparisons. Hosts supply the existing shared tracker and own its alias/fault/syscall integration and lifetime. No game-specific logic is involved.

Initial same-binary A/B, Apple M4 Pro / KosmicKrisp, warm identical pipeline cache copies, mono headless, sound off, 2100 frames, completed successfully. Control disables only texture tracking with CHIHIRO_VULKAN_TEXTURE_WRITE_WATCH=0; candidate uses 1. Existing color/depth tracking stays enabled.

| Window | Control FPS | Candidate FPS |
|---|---:|---:|
| Activation | 40.66 | 41.66 |
| Early held slow motion | 50.20 | 51.29 |
| Later held slow motion | 52.75 | 53.88 |

Reverse-order repeat also completed successfully: control activation 40.65 FPS and held windows 50.21/52.92 FPS; candidate 41.64 and 50.90/53.24 FPS. Retained as a modest throughput gain, not a claim of consistently improved tail latency or 60 FPS slow motion. See repeat-measurements.json. Initial data is in initial-measurements.json. Raw reports: /tmp/vc3-texture-watch-{0,1}/report.json. Logical comparison-byte counters are not measured memory traffic.

The real Vulkan render-texture fixture passed with synchronization validation and no VUID/SYNC errors. New cases cover aliased writes, palette edits, pinned immutable old images, repeated mismatches, restoring original bytes, active write leases and invalidation fallback. Log: /tmp/vc3-texture-watch-validation.log. Main macOS executable builds successfully. Android offscreen target build and the lavapipe fixture also passed. Neither driver reported VUID or synchronization errors. No headset used; these are not Quest performance claims.

Final first-pair cumulative comparison counters: control 618139 exact checks / 72602619168 logical bytes; tracked candidate 23120 exact checks / 27240234752 logical bytes, with 593071 tracked decisions. Frames and guest scheduling are not guaranteed byte-for-byte identical across runs.

## Follow-up held slow-motion CPU profile

A separate 2100-frame diagnostic run of the retained implementation completed successfully. Sampling started after completed frame 1680 for three seconds; raw report /tmp/vc3-texture-watch-cpu-profile/cpu-held.txt and runner report in the same directory. This sampled run is not an acceptance throughput measurement. Summed call-tree occurrences: nv2a_vsh_run 98, ch_vk_guest_depth_store 61, swap_color_rows 46, ch_guest_write_range_changed 12. These are sampled stacks, not independent timers or GPU timing. This motivates testing the existing exact binary32 depth-pack helper in CPU guest stores, while preserving the finite/range validation and Z16 conversion.
