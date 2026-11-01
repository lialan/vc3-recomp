# Rejected deferred depth-import experiment

Opt-in shared target code copied depth/stencil upload input into owned staging, then recorded the upload before the next batched draw. Other transfers, reads, clears, queries, explicit finish and destruction completed pending imports synchronously. Completion fence access returned NULL while an import was deferred. A transfer-read/write barrier protected shared staging when the batch also appended readback; new depth or color imports completed prior staging users before mapping. No game-specific logic or default change.

Correctness checks passed on KosmicKrisp and lavapipe with synchronization validation, with no VUID or SYNC diagnostics. Expanded fixture verified exact depth/stencil bytes for combined uploads/readbacks, repeated uploads, color staging reuse, explicit completion and destruction. Backend integration cases enabled the optimization for GPU paths, including quantized depth modes, and retained exact guest-memory parity. macOS and Android offscreen builds passed. Logs /tmp/vc3-deferred-depth-integration.log and /tmp/vc3-deferred-depth-lvp.log. No headset used.

## Performance decision

Same immutable binary and identical warm starting pipeline cache copies. Apple M4 Pro / KosmicKrisp, mono headless, sound disabled, 60 FPS cap, GPU timestamps off. All four runs completed 2100 frames. First pair disabled then enabled; repeat reverses order.

| Pair | Mode | Activation FPS | Early held FPS | Later held FPS |
|---|---|---:|---:|---:|
| First | Disabled | 43.20 | 54.52 | 55.97 |
| First | Enabled | 43.49 | 55.21 | 56.82 |
| Repeat | Disabled | 43.52 | 52.93 | 56.34 |
| Repeat | Enabled | 42.10 | 50.59 | 55.15 |

First enabled run combined all 5314 reported uploads with draw batches. Nevertheless the throughput improvement did not repeat; repeated held p95 also worsened from 23.92/20.70 to 26.66/21.93 ms. No claim that combining uploads is intrinsically slower, but current evidence does not justify retaining the additional pending-state complexity. All runtime/API/test/runner edits reverted. Patch preserved at /tmp/vc3-deferred-depth-rejected.patch, containing the correctness tests and experimental flag CHIHIRO_VULKAN_DEFER_DEPTH_UPLOAD. That flag is NOT in the retained runtime.

Raw reports: /tmp/vc3-deferred-depth-{0,1}/report.json and /tmp/vc3-deferred-depth-repeat-{0,1}/report.json. Provenance in measurements.json. The preceding depth-upload timing remains useful: saved host waits were a small fraction of the overall frame budget.
