# GPU attribute-stream gathering and cache persistence

2026-11-01, base7cd9eca. Shared Vulkan backend/runtime. Local simulator/headless only; no headset operations.

Raw reuse preparation now validates attribute spans, merges overlapping/adjacent ranges (including interleaved streams), and snapshots them in bulk. It records byte offsets/strides instead of expanding each vertex into16-byte attribute slots. Constants and zero strides remain immutable snapshots. Sparse layouts that would copy more than the padded representation retain the old preparation path. Scratch retention remains bounded; the final four-byte guard permits unaligned GPU word loads.

Fixed/program shaders gather encoded bytes from an additional read-only storage-buffer descriptor, reconstruct default lanes, and use existing GPU numeric decoding. They also support the legacy expanded snapshots, preserving the owned preparation APIs and sparse fallback. No native vertex-format pipeline variants or game addresses. All data lives in existing leased geometry arenas; descriptors, byte ranges, rebased indices and queued lifetimes remain validated. Header layout is generated from C with ABI assertions. New cumulative snapshot counters distinguish actual bytes from the prior expanded size.

Focused KosmicKrisp sync-validation batch passes:120 CPU snapshot cases,48 GPU compact/expanded exact-pixel comparisons covering every encoded type/size, unaligned33-byte interleaving, zero stride and source overwrite after snapshot; registered fixed/program rendering, constants/clipping/query/bounds, command batching and texture/coherence fixtures. Native build passes. No VUID/SYNC hazards. No cross-driver/Android matrix.

For frames1560..1920 the diagnostic run records133751 compact batches,462791652 snapshot bytes versus3289952768 expanded bytes:14.07% of prior bytes (85.93% reduction). Raw preparation CPU in held zero-creation frames is.501ms versus prior.979ms, but load differed; do not claim a controlled49% speedup. Other-project ghostsquadevolution GPU work and OrbStack activity were observed during diagnosis, left running as instructed. Exact byte reduction is independent of that contention.

The first attempted warm comparison was not properly warmed: both warm-up and candidate said save=skipped. Cache serialization had a64MiB cap. Raising the bounded cap to128MiB and reporting size/rejection reasons confirmed the updated blob is68728308bytes, above67108864. It now saves successfully, and the next run loads and saves exactly68728308bytes. Cache corruption/truncation/oversize/device-compatibility fixture passes. This fixes a real repeated-compilation failure exposed by shader changes; it is not a GPU texture-budget increase. Candidate uses a populated new-shader cache; the control used its own populated old-shader cache.

| Scene | Earlier control FPS | Saved-cache candidate FPS | Candidate p95 ms |
| --- | ---: | ---: | ---: |
| Combat early | 59.99 | 59.46 | 22.53 |
| Combat late | 59.99 | 60.53 | 21.11 |
| Activation | 59.34 | 59.91 | 20.42 |
| Held early | 60.54 | 60.01 | 23.06 |
| Held late | 59.95 | 60.02 | 21.41 |

All relevant runs exit0/reach2100frames. The cache-failure candidate had activation41.71FPS with443ms max; saved-cache candidate activation max27.51ms and no>33ms samples in these combat/slow windows. Do not treat the intervening cache fix/load changes as an isolated performance A/B or claim steady60: tails remain20–23ms. These are guest completions, not display cadence. Reports include binary hashes/flags. Final binary /tmp/vc3-stream-cache-vc3; final cache /tmp/vc3-stream-cache-populate.cache. Rawlogs /tmp/vc3-stream-{costs,warm-control,warm-candidate}/game.log and /tmp/vc3-stream-cache-{populate,warm}/game.log.

Next: verify current defaults and remaining completion waits using the now-valid cache. Dirty depth pooling has remained explicitly enabled in benchmarks and opt-in in runtime configuration. Do not confuse configured benchmark performance with default deployment acceptance. No significant CPU shader/transform/color/depth emulation is added; bounded encoded-byte copying/topology and rare fallback formats remain. Steady60 goal remains active.
