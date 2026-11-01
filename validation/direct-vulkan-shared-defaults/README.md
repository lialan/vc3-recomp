# Enable optimized shared runtime paths in ordinary startup

The benchmark runner explicitly enabled residency, GPU target sampling, exact guest-write tracking and register-combiner specialization. Ordinary runtime startup left those opt-in, so compiling optimizations into an APK did not enable all of them. Shared defaults now enable:

- Exact write tracking on supported non-Windows hosts, excluding the legacy RECOMP_WATCH diagnostic fault path.
- GPU render-target sampling when resident color and texture caching are enabled.
- Packed resident depth on compute-capable queues, bounded disjoint dirty owners, and fragment-rounded quantized D32 fallback.
- Register-combiner instruction specialization, with dynamic factors, fog and textures unchanged.

Explicit diagnostic overrides remain; requested clean-depth reuse prevents automatic dirty pooling. No simulation clock, vblank schedule or fence semantics changed. Raw GPU geometry, GPU texture decode and GPU color/depth conversion were already defaults and remain so.

Native game and Android runtime/Vulkan-library builds passed. Prior queued-readback focused validation covered these residency paths and combiner specialization already has dedicated exact fixture coverage. Two additional local 2100-frame runs removed all performance overrides, retaining only automated input, diagnostics and pipeline cache location. The first revealed generic combiner shaders were still the default; the corrected run enables specialization by default. Both exited0/reachedlimit/savedcache. This is a profiled sequential local comparison, not a repeated or isolated Quest benchmark.

| Full interval window | Generic FPS | Specialized FPS | Generic p95 ms | Specialized p95 ms |
|---|---:|---:|---:|---:|
| 1321..1441 | 59.95 | 59.88 | 21.36 | 21.59 |
| 1441..1561 | 60.10 | 54.58 | 21.45 | 21.39 |
| 1561..1681 activation | 41.17 | 57.06 | 27.06 | 22.43 |
| 1681..1801 held | 42.66 | 59.99 | 25.69 | 22.77 |
| 1801..1921 held | 46.18 | 60.08 | 25.53 | 22.14 |

Individual stalls remain, including the second combat interval. Excluding frames with pipeline creation, summed GPU batch time fell from 11.237 to 3.873 ms in activation and 11.926 to 4.141 ms while held. Completion wall time fell 7.681 to 5.082 ms and 8.381 to 5.478 ms respectively. CPU draw/completion time remained broadly similar. Timer scopes overlap and are not additive. See comparison.json.

Startup logs confirm the optimized defaults, and late sampled CPU geometry, strict depth, resident depth layout/float and color conversion counters are zero. CPU texture fallback remains flat at 1,030,144 pixels from startup, not increasing in sampled warm gameplay. No broad claim that every possible game format uses GPU conversion. Frame1840 inspected: expected combat scene and E.S. overlay, no full-game parity claim.

Final binary /tmp/vc3-shared-defaults-specialized-vc3; run /tmp/vc3-shared-defaults-specialized; cache saved70,015,500bytes. The intermediate generic run is /tmp/vc3-shared-defaults. No device access, APK installation or package-cache regeneration. The installed checkpoint predates these default fixes. Further work should investigate pacing/scheduling tails and asynchronous decode costs with these ordinary defaults rather than relying on benchmark-only flags.
