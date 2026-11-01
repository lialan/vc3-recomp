# Pair all retained depth owners and preserve shared-zeta GPU copies

Local headless Vulkan,2026-11-01. Baseb2eeab2. Two shared runtime fixes bundled before one focused correctness batch and one benchmark pair, per user request. No headset/device operation.

Completion now pairs color/depth readbacks for every dirty resident target, not just the currently bound owner. Per-target color serials preserve CPU edit invalidation after a paired read. A final depth flush covers remaining depth-only owners, and target finish still precedes guest completion publication. New explicit target-selective depth/color flush does not flush unrelated owners; completion callers must finish with the all-owner flush.

Exact shared-zeta migration under dirty pooling now moves the owner using the existing GPU depth/stencil copy path. If the destination previously owned disjoint dirty storage, publish/release that storage first. Partial aliases retain publication/import. This restores an existing generic optimization for other games; VC3 reports no GPU-copy use in these scenes.

Focused KosmicKrisp synchronization-validation batch passes exactD32/D24 linear/Morton owner checks plus registered pooled-backend guest-memory/coherence comparisons. Extended tests check non-current paired flush, preservation of another dirty owner until completion, and GPU copy with unchanged import count and unmodified guest RAM. Native build passes. No repeated Android/lavapipe/reverse-pair run for this bundle. Frame1740 visually confirms active E.S. scene.

Matched starting pipeline-cache copies and environment, previous pooled binary vs updated binary,2100frames each; both exit0. Pooling explicitly enabled in both. Dirty pooling remains opt-in; this commit does not change defaults.

| scene | control FPS | candidate FPS | control/candidate p95 ms |
| --- | ---: | ---: | --- |
| normal early | 59.99 | 59.99 | 20.98 / 20.52 |
| normal late | 59.99 | 59.99 | 20.80 / 20.23 |
| activation | 54.86 | 59.90 | 20.78 / 20.09 |
| held early | 57.81 | 59.87 | 20.11 / 19.84 |
| held late | 60.74 | 59.93 | 18.61 / 19.28 |

Near60 means do not establish steady frame pacing: p95 still19-20ms and some intervals exceed20ms. Another ghostsquadevolution workload was running (PID5790 when captured) along with WindowServer load; no isolated-GPU or repeatable-gain claim. Submission totals23049->23035 are essentially flat; do not claim a large submission-count reduction from pairing. Combined read counters reflect changed readback routing. Final CPU bulk geometry/color/depth counters remain zero.

Binary/tmp/vc3-paired-pool-vc3; logs/cache/tmp/vc3-paired-pool-{control,candidate}. Reports preserve hashes/environment/capture hashes. Next bundle: cache validated color-layout spans to remove repeated overlap/alias calculations per draw, and address retained-owner compatibility before changing defaults. Keep the user-requested bundled validation cadence. Steady60 goal remains active.
