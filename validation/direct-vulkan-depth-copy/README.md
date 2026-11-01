# GPU transfer of exact shared depth ownership

When two color targets use identical zeta storage, extent, pitch and depth
interpretation, the resident depth owner now transfers directly between their
native depth/stencil images. Pending source and destination draws complete before
the transfer. Both images return to attachment layout, their color images remain
unchanged, and the real destination fence completes before ownership moves.
CPU component edits merge first. Layout/extent/pitch/zeta changes and partial
aliases retain the existing publication/import path. Guest completion and sampled
depth still publish the latest owner. No precision or fence semantics are relaxed.
`CHIHIRO_VULKAN_DEPTH_GPU_COPY=0` retains the CPU-roundtrip diagnostic control.

The complete Quest GPU suite passes. New coverage checks pending D32 source and
destination draws, nonuniform depth/stencil, preserved colors, exact subsequent
depth/stencil visibility (8 samples), extent rejection and self-copy. Resident
D24 tests assert102 GPU ownership copies with one initial import/one explicit
writeback, stencil-only CPU edits retaining dirty GPU depth, the CPU control,
and the existing overlapping-zeta/texture/palette/semaphore cases.

Both O2/A78 runs use the same binaries and reach240 startup frames. In their
first120 intervals, strict roundtrips report12.89FPS / p99 218.19ms; GPU copies
report14.66FPS / p99 206.21ms. At frame240 the copy run records476 GPU copies,
1061 imports and1340 writebacks; the strict run records1539 imports and1577
writebacks. These single startup samples show useful reduced transfer work,
but remain far below the60FPS goal and are not combat or VR frame-time acceptance.

Frame180 captures differ (max channel delta35, 11044 pixels). Visual inspection
shows different AM2-logo/INSERT-COINS fade phases. Wall-time-dependent phase is
the likely explanation, not an established synchronized pixel comparison; the
frame clock aligns inputs but does not freeze guest timers. Both captures are
retained. Exact GPU-copy correctness is established by the controlled fixtures,
not by claiming these game captures match. Both sessions used one whole-session
Quest lock and cleaned up; the original GL APK remained unchanged.
