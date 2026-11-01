# Disjoint dirty depth owners: bundled prototype

Local simulator/headless Vulkan only,2026-11-01. Basec783cd5. No headset/device operations. User requested fewer repeated tests and more fixes bundled together; after the first prototype, combined full-clear elision and a hot draw fast path, then ran one focused correctness batch and one matched benchmark pair.

## Shared changes

Opt-in CHIHIRO_VULKAN_MULTIPLE_DEPTH=1 retains up to four disjoint dirty depth owners. Switching between unchanged views does not publish/import. Actual completion flushes all owners; overlapping texture consumers publish matching inactive owners; alias reinterpretation publishes and releases overlapping owners. Resize/release flushes and releases all borrowed targets. CPU edits merge through the existing per-component logic. Cold-owner eviction publishes first. Bound retained guest spans to16MiB (at most64MiB scratch); an oversized individual view falls back to one owner with no additional retained images. Clean reuse and dirty pooling are mutually exclusive. Default remains single-owner.

Initial pooling exposed extra readbacks (14865 versus13320control). Fixed full-clear semantics: an exact full depth+stencil clear on another target supersedes dirty pixels, so the old image is discarded instead of published. Partial/different views still publish. Added a fast path for the unchanged active depth view so ordinary batches do not scan the owner pool. No title-specific conditions.

## Results

| final bundle | control | enabled |
| --- | ---: | ---: |
| depth imports | 5315 | 4267 |
| depth writebacks | 13322 | 12792 |
| normal FPS | 59.98/60.10 | 59.99/60.00 |
| activation FPS | 51.53 | 55.52 |
| early held FPS | 56.22 | 56.42 |
| later held FPS | 55.83 | 59.62 |
| activation p95 ms | 25.03 | 22.38 |
| early held p95 ms | 20.93 | 21.64 |
| later held p95 ms | 22.49 | 19.91 |

This eliminates1048imports and530writebacks in the measured run. One matched pair shows throughput improvement but mixed tails; no repeatability, isolated-GPU, or steady60 claim. Another project has run GPU work concurrently in this session. Default remains off pending further integrated progress/coverage, not a proven all-game default. CPU bulk color/depth/geometry fallbacks remain zero in these logs. Frame1740 was visually inspected as active E.S. combat.

## Validation scope

Initial owner prototype: exactD32/D24 linear/Morton tests and existing rendertexturefixtures pass on KosmicKrisp and lavapipe with synchronizationvalidation; native/Android builds pass (buildonly). Final bundle: one native correctness batch on KosmicKrisp passes exact owner tests including full-clear supersession, dormant CPU stencil edits, all-owner completion, inactive texture consumers and aliases. Registered render-texture comparisons now explicitly enable pooling for resident-depth cases and pass exact guest-memory/coherence checks. No VUID/SYNC-HAZARD errors. Final native build passes. Android and lavapipe were not rerun after the clear/fast-path fixes, per the requested bundled cadence.

All four local2100frame runs exit0. Final binary/tmp/vc3-multiple-depth-bundle-vc3; runs/tmp/vc3-multiple-depth-{bundle-control,bundle-candidate}. Reports record binary hash, env and capture hashes; raw logs remain there. The opt-in pool conservatively rejects color/depth aliases across retained owners, and cross-target shared-zeta aliases currently use publication/import rather than the single-owner GPU-copy shortcut. Slot-pressure and broader-game performance coverage remain open. Further fixes should be bundled before the next focused validation and benchmark, avoiding another automatic reverse pair for this individual change.
