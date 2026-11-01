# Bounded cold texture eviction and per-frame diagnosis

Local simulator/headless Vulkan only, 2026-11-01, Apple M4 Pro / KosmicKrisp. Base commit abebf5e. No headset or device operations. Other projects kept running at user request. These traces diagnose individual operations; they are not isolated throughput comparisons or steady-60 acceptance.

## Finding and shared change

In the baseline `frame-costs-v4-trace.txt`, activation texture misses spend 11.681, 8.367 and 9.729 ms in cache make_room (and 2.991 ms for a smaller snapshot). Existing byte-pressure LRU destroys many tiny old images to allocate one large image. This is GPU resource management on the host, not CPU emulation of shader or raster work.

The shared cache now chooses a fitting cold unpinned victim under byte pressure, or the largest cold victim if none fits. Entries touched in the current/previous frame are excluded from this preference. LRU remains the fallback and handles entry-count pressure. A completed-frame hook trims at most one cold unpinned image above 75% of the existing byte budget; hard capacity remains 64 MiB. No title addresses, game-specific scene logic, larger budget, or background destruction thread. Standalone clients opt in and supply frame boundaries; the shared runtime enables it by default. CHIHIRO_VULKAN_SIZE_AWARE_EVICTION=0 restores the control.

The intermediate size-aware-only candidate reduced the first eviction burst but left later bursts. The final cold-trim trace has effectively zero make_room time in the observed initial activation allocations. This does not eliminate all destruction latency: warm trim maxima were 5.478 ms (frame 1416) and 2.226 ms (1813), while the observed initial activation cleanup steps were about 0.2-0.3 ms. One-image-per-frame is an operation bound, not a wall-time guarantee. Retaining headroom can increase misses for other working sets; an isolated same-binary A/B is still needed before claiming a throughput win.

## Correctness and scope

Both KosmicKrisp and lavapipe pass the exact-pixel render-texture/coherence fixture with synchronization validation and no VUID/SYNC-HAZARD errors. The added fixture checks fitting cold victim selection, preservation of tiny cached entries, recent-frame grace, byte budget, pinned image safety, and LRU control. Native VC3 and Android offscreen builds passed (Android build-only). slow-motion-1740.png was visually inspected and shows active E.S. combat.

Warm CPU color pixels, resident depth layout/float pixels, strict depth pixels, and declined raw GPU batches remain zero. CPU texture decoding averages in the final contended run: normal 0.00615 ms/frame, activation 0.024725, held 0.00085. These are counter deltas, not sampled percentages. Compatibility texture formats and legacy metadata vertices still exist; no literal-zero/all-games claim. Prior Morton audit contains the full bulk geometry accounting.

## Measurement contract

Set CHIHIRO_VULKAN_PROFILE=1 and CHIHIRO_VULKAN_FRAME_COSTS=1 for per-frame phases and slow draw/texture-miss traces. CHIHIRO_VULKAN_GPU_TIMESTAMPS=1 enables GPU timing. Disabled tracing avoids these detailed logs and counter scans. Frame phases are draw, clear, flip flush, cache trim and residual other. Pipeline creation, batch GPU/wait and depth-upload wait overlap those phases and must not be added to them. Other includes guest execution, pacing, raw snapshots and callbacks; it is not pure guest instruction emulation. GPU batch timings exclude standalone uploads. Cumulative retired target costs survive target resize. Depth-upload wait uses a cumulative counter, not the resettable report accumulator.

Detailed tracing perturbs execution. Reports record exact binaries, environments, pipeline caches, scene input and capture hashes. Original logs remain /tmp/vc3-<run>/game.log. Saved trace slices cover completed frames 1320 through 1920; measurements.json records counter deltas and accounting checks. The earlier /tmp/vc3-frame-costs v1 had an invalid rolling depth-wait delta and is deliberately excluded.

Both baseline and final runs still create a few pipelines inside the nominal warm windows: 20.123 and 34.566 ms total across frames 1320-1920. Persistent-cache runs are not proof that every pipeline is warm. Even frames with zero pipeline creation show substantial GPU/host wait and residual timing. Final contended FPS is normal 31.16/31.51, activation 27.19, held 26.95/23.80; do not compare it as a regression or gain against less-contended runs. External OrbStack and concurrent build workers were consuming several cores. No steady-60 claim.

Next: repeat untraced same-binary policy control/candidate runs with identical populated pipeline-cache copies when contention permits, verify pipeline-creation counters, and investigate remaining waits/pacing independently from graphics emulation. User chose to keep other work running and continue diagnosis.
