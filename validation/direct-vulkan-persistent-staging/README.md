# Rejected persistent color/depth staging experiment

2026-11-01. Base37a2fcb. Local Vulkan simulator/headless only; no headset/device operations.

The saved candidate patch makes shared targets map color/depth staging once at allocation, retain the mapping through transfers and unmap during destruction. Paired color storage follows the same lifetime. Host-coherent memory skips redundant flush/invalidate API calls; noncoherent memory retains whole-allocation cache maintenance. Existing GPU barriers, fence waits, guest publication, format conversion and staging allocation sizes are unchanged. This removes repeated host API work, not the serialized waits themselves. No game-specific code. The candidate was rejected; production source is restored to base37a2fcb.

One native build and focused KosmicKrisp synchronization-validation batch passed: exact D32/D24 linear/Morton depth, RGBA/BGRA color, repeated staging reuse, paired readbacks, pooled owners, CPU component edits, render-texture snapshots and registered backend coherence. Saved logs have no VUID/synchronization hazards. The local driver run does not establish coverage of every physical noncoherent memory type; that branch retains the previous explicit cache operations. No repeated cross-driver/Android matrix.

One sequential2100frame comparison, identical populated cache copies, frame-only diagnostics, dirty pooling explicitly enabled, QoS unchanged, pacing unset. Both runs exit0 and reach frame limit. Reports preserve full environment/binary hashes. Rawlogs /tmp/vc3-staging-{control,candidate}/game.log.

| Scene | Control FPS | Candidate FPS | Control p95 ms | Candidate p95 ms |
| --- | ---: | ---: | ---: | ---: |
| Combat early | 59.99 | 59.77 | 21.18 | 23.87 |
| Combat late | 59.92 | 60.13 | 21.13 | 23.12 |
| Slow-motion activation | 58.84 | 46.75 | 20.81 | 27.02 |
| Held early | 60.50 | 55.82 | 20.36 | 26.03 |
| Held late | 60.14 | 55.99 | 21.30 | 24.31 |

The candidate substantially regressed activation (58.84 -> 46.75 FPS) and held slow motion (60.50/60.14 -> 55.82/55.99). A single justified follow-up using the unchanged control recovered activation to59.00 and later held to60.24; early held57.63 had two >33ms outliers. This does not isolate host contention from persistent mapping versus skipped coherent cache operations, but it does not justify enabling the candidate. Rejected and restored the production staging path; candidate.patch preserves the experiment for a future driver-specific diagnosis. Do not claim a measured win or silently repeat this candidate.

Follow-up control combat59.99/59.99, activation59.00, held57.63/60.24FPS; p9521.83/22.06/20.99/23.00/21.33ms. Warm steady60 remains unproven. Guest completion timing is not display cadence; no exclusive-machine claim. CPU-emulation audit is disabled in frame-only runs, so this is not fresh zero-counter proof. Next substantial target remains submission/readback serialization while preserving guest visibility and independent owner ordering. Keep bundled fixes and focused validation rather than another automatic benchmark matrix.
