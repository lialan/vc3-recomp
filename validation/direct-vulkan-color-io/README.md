# Profile-guided linear color transfers

A scoped Quest simpleperf recording of the actual native guest collected2071
samples with none lost. Of sampled user CPU time,33.12% was memcmp,22.16% color
publication and9.71% color loading. Callchains attribute19.41% to color
observation,11.15% to depth observation and2.51% to texture-cache comparisons.
This measures CPU samples, not GPU wait time or overall frame time.

Linear ARGB8888 transfers now swap red/blue with packed integer words. Bounds
checks run first and only disjoint planes enter the restricted loop; aliases,
RGB565, Morton and other endianness retain the existing path. Unaligned rows use
constant-size builtin copies. NDK optimization remarks showed that Android's
fortified memcpy wrapper blocked vectorization; bypassing that wrapper only
inside this already bounds-checked loop produces NEON loads/masks/stores in the
actual O2/A78 binary. No hand-written SIMD or relaxed pixel arithmetic is used.
`CHIHIRO_VULKAN_COLOR_FAST_IO=0` supplies the generic-byte-path control.

The Quest renderer suite passes. New CPU cases compare the complete guest buffer
against the scalar path, using17-pixel rows (vector chunks plus tail), an odd
base/stride, alpha, padding, bounds/atomic rejection and in-place alias loads.
Both same-binary O2/A78 startup runs reach240 frames: first120 intervals measure
19.64FPS / p99 199.08ms for generic transfers and22.28FPS / p99 188.30ms for the
fast path. This is one startup pair, not a combat benchmark or synchronized image
comparison. Maxima remain305.07/304.13ms. No pacing acceptance is claimed.

A follow-up1672-sample profile (none lost) reports9.03% in the color store plus
2.33% in its new shared swap loop; color loading is8.49% and memcmp43.36%.
These are separate diagnostic profiles, with changed CPU composition, not a
same-event-count speedup estimate. Raw perf.data files remain in the ignored
build-quest-vulkan-perf/profiles tree. The canonical runner now supports
--simpleperf for this own-workload recording, without changing system profiling
permissions. Binary hashes and build settings are in each report.

Periodic captures (--capture-every) retain intermediate screens if a long run
hits its watchdog. RGB data is buffered and written in one operation, then
renamed atomically so only complete PPMs are exposed. Mac and actual Quest
four-frame runs both produce exactly frames0/2 at interval2, with correct pixel
payload sizes. Capturing adds I/O to frame times; use separate runs for pacing.

All Quest sessions held the whole-session shared lock, cleaned UUID files and
verified the original GL APK hash. The refreshed no-GL APK audit is build-only.
Full gameplay, immersive attachment and sustained Quest combat/frame-time
acceptance remain open.
