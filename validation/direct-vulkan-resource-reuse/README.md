# Retained Vulkan draw resources

Default: 32 pipeline objects per target with LRU eviction, one render pass,
image view and framebuffer per target, and growable persistently mapped
geometry storage. A new write/replacement/descriptor update occurs only after
the prior synchronous GPU fence completed. Noncoherent memory uses an aligned
flush; coherent host-visible memory is preferred. This is not command batching.

CHIHIRO_VULKAN_REUSE_RESOURCES=0 selects one pipeline entry and per-draw
geometry allocation as a same-binary diagnostic control. The target render
pass/framebuffer remains shared in this control, so it does not recreate every
aspect of the original implementation.

The final Quest GPU fixture compares 128 alternating-state draws, preserving
every RGBA byte: the control creates 128 pipelines/128 geometry allocations;
reuse creates 2/1. Buffer growth beyond 64 KiB and a return to small draws retain
correct pixels. Forty additional states exercise bounded eviction and rebuilding
an earlier state. The observed single-run loop times are 70.09 ms and 45.54 ms;
these are offscreen microbenchmark timings including readback and compilation,
not combat FPS, repeated statistical estimates or a performance acceptance gate.
Other native/GLES pixel parity, guest coherence, queries, completion, layout and
injected-XR ownership tests remain passing. Original GL APK hash unchanged.

Both Mac native startup runs finish 120 frames with identical frame 30-32 captures
and no GL imports. This earlier Mac diagnostic control used 64 KiB minimum geometry
allocations; the final control uses exactly the requested size, as covered in
the final Quest fixture above. The retained/default behavior is unchanged by
that control adjustment. Reports identify each actual binary hash.

The APK report is a build/import audit, not an installation or Quest gameplay
claim. Full gameplay, immersive attachment, raw GPU vertex processing and
sustained Quest combat performance remain open.
