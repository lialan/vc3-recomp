# Cached, persistently mapped shared Vulkan staging

Render-target readback allocations now prefer HOST_VISIBLE | HOST_CACHED memory, falling back to any host-visible type. Previously the first host-visible type was selected regardless of CPU caching. Both primary color/depth staging and paired-color staging retain a lazy mapping until target destruction. Texture upload/readback staging retains its mapping across replacements, with replacement allocations fully mapped before retiring old storage. GPU texture decoding, color/depth packing, fence ordering, guest publication, and noncoherent flush/invalidate calls are unchanged. No game-specific paths or shader changes.

This targets CPU readback bandwidth and driver mapping overhead. Existing Quest tutorial logs showed roughly 11–13 ms of CPU completion work in late frames, but did not identify the selected memory type or isolate mapping cost. There is no measured Quest speedup yet and no headset access in this task.

One focused local validation batch on Apple M4 Pro/KosmicKrisp with Khronos synchronization validation passed:

- Render-texture and guest-memory coherence, aliases, CPU edits, resized targets, cache ownership, pending sampling and mip contents.
- Packed/unpacked D32/D24, linear/Morton color/depth, repeated staging reuse, paired pending color/depth, multiple dirty owners and guest component edits.
- 108 DXT1/3/5 and 240 supported 32-bit texture decode cases, including exact mip contents, queued replacements and bounds.

No validation errors or synchronization hazards. Native game build and Android shared Vulkan library cross-build passed. An unused-label compiler error during the edit was corrected before validation. Tests do not establish noncoherent Adreno behavior or VR acceptance. Installed Quest checkpoint and preserved APK were not changed.

One warm-cache local game run completed 2,100 frames, exit 0, and saved the 68,774,076-byte cache successfully. Current build also embeds the same Mac seed; startup reported both packaged and disk cache bytes. No captures requested in this frame-only run. Compared with the preceding historical run (not a fresh isolated A/B), tails are mixed; no measured speedup or steady-60 claim.

| Window | FPS | p95 ms |
|---|---:|---:|
| 1321..1441 | 59.96 | 21.51 |
| 1441..1561 | 60.01 | 22.13 |
| 1561..1681 | 59.84 | 21.44 |
| 1681..1801 | 60.1 | 20.78 |
| 1801..1921 | 59.99 | 22.03 |
| 1921..2041 | 60.06 | 21.36 |

Local executable /tmp/vc3-mapped-staging-vc3; complete run /tmp/vc3-mapped-staging-warm. These are guest-completion intervals, not Quest/XR display measurements.
