# Reuse unpinned CPU texture allocations

The shared Vulkan cache can reuse a matching CPU texture allocation once all sampling pins have been released. The new ch_vk_texture_replace_argb API retains image/view/sampler/staging/command resources, waits for any previous upload before writing staging, then replaces base pixels and regenerates existing mipmaps. It supports queued or synchronous uploads and six-face cubes. Callers must complete every prior sampling lease; this API does not wait for arbitrary external sampling.

Cache reuse is opt-in via ChVkTextureCacheConfig.reuse_uploads, enabled by the shared backend. The key, dimensions, palette span and budget cost remain identical. Pinned snapshots are immutable. Exact encoded-byte validation precedes replacement; cached comparison state is invalidated afterward so the old generation result cannot hide a subsequent edit. No game-specific addresses or scene heuristics; default 64 MiB budget unchanged.

## Local performance

Apple M4 Pro / KosmicKrisp, mono headless, sound disabled, 60 FPS cap. Identical warm starting pipeline cache copies, two immutable binaries. All four runs completed 2100 frames successfully. First pair candidate then control; repeat reverses order. No profiling tools or competing builds during runs.

| Pair | Variant | Activation FPS | Early held FPS | Later held FPS |
|---|---|---:|---:|---:|
| First | Control | 42.41 | 51.66 | 55.00 |
| First | Reuse | 43.22 | 54.22 | 56.25 |
| Repeat | Control | 40.73 | 50.81 | 54.13 |
| Repeat | Reuse | 41.90 | 51.24 | 55.29 |

Held p95 first: 24.85/20.80 -> 23.44/20.13 ms; repeat: 24.55/21.89 -> 24.08/20.98 ms. Normal combat remains about 60 FPS. Throughput gain varies; this does not establish stable 60 FPS slow motion or uniform tail-latency improvement.

First candidate reused 2480 CPU uploads. Final evictions fell from3267 to270; live cache payload stayed within64MiB. GPU snapshot reuse increased from997 to1405 because fewer obsolete CPU snapshots competed for space. Upload counts remain similar (2975 versus2949); avoiding allocation and stale snapshot retention is the mechanism, not skipping changed texture contents. Counter totals reflect whole runs and can vary with guest scheduling.

## Validation

Expanded render-texture fixture passes on KosmicKrisp and lavapipe with synchronization validation, with no VUID/SYNC errors. Cases include repeated queued replacements before readback, preserved view/sampler handles, invalid short input, exact mip/cube pixels, alias and palette writes, unpinned allocation identity, repeated mutation after reuse, pinned old-image immutability, active leases and invalidation fallback. Existing strict-vs-GPU exact guest-memory cases also pass. Logs: /tmp/vc3-cpu-texture-reuse-validation.log and /tmp/vc3-cpu-texture-reuse-lvp.log.

macOS main executable and Android offscreen target build successfully. Candidate frame1740 visually shows the expected E.S. combat scene/effects; this inspection is not comprehensive scene acceptance. No headset used and no Quest performance claim. Raw reports: /tmp/vc3-cpu-texture-reuse-{control,candidate}/report.json and /tmp/vc3-cpu-texture-reuse-repeat-{control,candidate}/report.json. Full provenance and timings are in measurements.json.
