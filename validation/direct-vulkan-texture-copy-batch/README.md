# Batch render-target texture refreshes

Shared refresh path appends image copy and mip generation to a pending source batch, then waits once on the source fence. Previously source rendering completed before a separate texture copy submission/wait. New recording API accepts a same-queue command buffer and documents exclusion of sampling/reuse/destruction until completion. Destination upload is retired before recording; device/queue/extents/layers/alias checks precede modifying the source batch. Fresh snapshots and empty-source refreshes keep their existing path. Copy and mip recording helpers are shared by both paths. No new shader or asynchronous guest completion behavior.

Native build and focused KosmicKrisp synchronization validation pass. Fixture performs two full-source color changes, asserts combined_texture_copies increments, reads every mip exactly, rejects null destination without consuming pending draws, then exercises existing texture cache pins, source destruction, CPU edits, aliases and strict-vs-GPU guest-memory parity. Existing combined depth-copy cases also pass. No VUID/SYNC diagnostics. One compile correction fixed the fixture vertex member name before execution. No cross-driver matrix.

| Window | Prior FPS | Candidate FPS | Prior p95 ms | Candidate p95 ms |
|---|---:|---:|---:|---:|
| 1321..1441 | 60.13 | 59.99 | 23.66 | 22.87 |
| 1441..1561 | 60.03 | 60.01 | 22.44 | 21.8 |
| 1561..1681 | 59.89 | 59.89 | 20.6 | 22.19 |
| 1681..1801 | 60.03 | 59.95 | 21.36 | 19.74 |
| 1801..1921 | 59.89 | 59.95 | 21.6 | 20.57 |

Candidate2100framesexit0/reachlimit. Cache68774076bytes loads/saves successfully. Comparison reuses preceding turn control, not a fresh isolated A/B; no repeatable speedup claim. Frame-only profiling, dirty pooling explicitly enabled (runtime still opt-in). Tails remain above16.67ms; steady60 not established. Actual-game combined-copy count not captured; fixture proves eligible path use. Binary/tmp/vc3-texture-copy-vc3, cache/tmp/vc3-dxt-gpu.cache, run/tmp/vc3-texture-copy-warm. All processes terminal.
