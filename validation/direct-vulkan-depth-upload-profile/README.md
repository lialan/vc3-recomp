# Retained backend depth upload timing

Two 2100-frame local headless diagnostic runs completed: /tmp/vc3-retained-boundaries and /tmp/vc3-depth-upload-profile. GPU timestamps enabled, so these are diagnostic timings rather than acceptance FPS. No headset use.

Added shared target profiling distinguishes depth upload calls, inclusive host duration, and time waiting on the upload submission's own fence. Counters are emitted/reset with existing per-target batch reports. Inclusive upload duration can contain completion of earlier draws and is not additive to batch timing. Fence wait is host elapsed time, not GPU execution time. Profiling is inactive without GPU timing enabled.

Reports emitted between completed frames 1560/1680 contained 309 uploads, 108.35 ms inclusive and 43.00 ms own-fence wait. Between 1680/1920: 598 uploads, 230.50 ms inclusive and 100.10 ms own-fence wait. Per-target wall-clock reporting intervals straddle these frame markers; these are report-emission aggregates, not exact frame slices. Partial intervals after the last report are not included.

This makes deferred imports a modest opportunity, insufficient alone to explain the slow-motion gap. A correct future implementation would need to preserve staging memory until upload completion, handle imports before draws/clears/readbacks/copies, expose no premature completion fence, and flush pending uploads before reusing shared staging. Current upload semantics remain synchronous. Before introducing that lifetime complexity, compare cache capacity on the current backend: the sampled CPU stacks include decoder/upload/make-room calls and the retained 64 MiB cache continues to evict textures.
