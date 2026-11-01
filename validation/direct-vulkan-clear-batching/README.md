# Shared resident clear batching

2026-11-01, base385d8cf (runtime37a2fcb). Local Vulkan simulator/headless only.

Eligible resident aspect clears now use vkCmdClearAttachments inside the bounded draw batch. Extracted common batch initialization so clear-only batches can start without a draw. Partial color/depth/stencil clears preserve untouched aspects; fragment-quantized depth retains the existing attachment packing rule. First-use/uninitialized targets, synchronous readback, and modes requiring an extra quantization pass keep the old path. ch_vk_target_clear_read remains synchronous; ch_vk_target_clear_aspects_read documents deferred completion when batching/deferred readback are enabled. Explicit finish/read/query/resource boundaries remain unchanged; pending clears own the fence just like draws. Batch limits include clears, while draw counters exclude them and batch_clears counts them separately. No game addresses or scene heuristics.

Validation: native build plus one focused KosmicKrisp synchronization-validation batch for command batching, exact packed depth/color, pooled ownership and registered texture coherence. Added real-GPU sequence: three partial clears then a draw, exact color/depth/stencil readback, unchanged output before completion, correct pending fence ownership, limit1 versus64/256. At larger limits the sequence needs one submission; limit1 correctly needs four. Existing draw/submission/timestamp counts remain exact. No VUID/SYNC diagnostics. No cross-driver/Android matrix repeated.

One sequential2100frame warm-cache comparison, frame-only diagnostics, identical cache copies, dirty pooling enabled, QoS unchanged/pacing unset. Both runs exit0 and reach limit. Full environment/binary hashes in reports; raw logs /tmp/vc3-clear-batch-{control,candidate}/game.log.

| Scene | Control FPS | Candidate FPS | Control p95 ms | Candidate p95 ms |
| --- | ---: | ---: | ---: | ---: |
| Combat early | 60.02 | 60.07 | 21.28 | 22.57 |
| Combat late | 60.05 | 60.03 | 21.75 | 22.32 |
| Slow-motion activation | 59.85 | 59.88 | 22.30 | 22.28 |
| Held early | 59.97 | 59.97 | 20.90 | 21.58 |
| Held late | 60.00 | 59.98 | 20.52 | 21.73 |

This establishes correctness and fixture submission reduction, not a whole-game steady60 guarantee. Frame times are guest completions, not display cadence. Per-draw counters were disabled for the low-overhead comparison, so no actual-game batched-clear count or new CPU-emulation zero-counter claim. No isolated-machine claim. Explicit full-surface clear rectangles currently still take the separate rectangle-draw path in vk_nv2a.c; next bundle should route those through the shared clear path with exact rectangle/channel/depth parity coverage. Preserve the requested fewer-tests/bundled-fixes cadence.
