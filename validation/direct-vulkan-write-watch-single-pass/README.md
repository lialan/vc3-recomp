# Single-pass stable guest write tracking

Shared guest write tracking previously scanned stable page state twice per check: first for re-arming, then for generations. Combine the stable scan with generation comparison. If a writable page is found, preserve the existing ordered locks, protection re-arm and full rescan. Active leases and disabled tracking remain conservative. Independent observer stamps, GPU work, guest clocks and pipeline identities are unchanged.

Native VC3, Android chihiro_vulkan/chihiro_rt and the three fixture targets built successfully. guest_write_watch_test passed alias/observer/fault decoding, 1000 concurrent writable/rearm cycles, leases, protection invalidation and teardown. Vulkan texture-render and depth-pack fixtures passed with synchronization validation; no Validation Error or SYNC-HAZARD in archived logs.

One local 2100-frame capture exited successfully, using a copy of the previous queued-depth candidate cache. The final periodic counter records 1,503,163 single-pass checks covering 69,031,775 pages: each avoids the old second page-state scan. This is operation-count evidence, not a measured bandwidth or FPS improvement.

Combat windows: 59.93/60.10 FPS; slow-motion activation: 59.94 FPS; held: 60.02/59.95 FPS. Target-window p95 is 21.71-23.34 ms, maximum 31.59 ms, zero intervals above 33.333 ms. This is local guest completion pacing, not XR presentation or Quest acceptance, and does not prove steady 16.67 ms frames.

Non-compilation frame means for combat/activation/held: draw CPU 1.763/3.066/2.889 ms; draw wall 3.434/6.330/5.833 ms; completion wall 4.905/4.932/4.762 ms. There is no fresh matched control, so do not attribute timing differences from prior runs to this change. Nested scopes must not be added.

CPU geometry batches/vertices, resident depth layout/float pixels, strict depth pixels and color conversion pixels remain zero. Residual CPU texture decoding totals 6.723 ms over the full run (1,030,144 pixels); not all CPU graphics work is eliminated.

Binary /tmp/vc3-write-watch-single-pass-vc3 includes unrelated preexisting host_input.c edits, excluded from this commit. No headset access, APK installation or push. Prior Quest shader/cache refresh and on-device pacing validation remain pending.
