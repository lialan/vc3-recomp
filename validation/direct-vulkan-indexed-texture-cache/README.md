# Indexed texture-cache fallback lookup

Replace full-table fallback scans after hint misses with 1024 bucket heads and one linked index per entry. Keep validated hints, complete identity comparisons, write tracking and serial checks. Insert in ascending slot order to preserve prior hit/reuse selection. Unlink before destruction and insert only after successful allocation; in-place refresh preserves identity. Collisions never imply equality. No game-specific paths, guest clocks, shaders or pipeline identities change.

Native VC3 and Android chihiro_vulkan builds passed. Texture GPU integration passed synchronization validation with no Validation Error or SYNC-HAZARD. Added collision coverage: distinct sampler identities at the same address, repeated pinned hits, eviction and slot reuse while another colliding image remains pinned. Existing alias/write-watch/snapshot and size-aware eviction cases also pass.

One 2100-frame local gameplay run passed, using a copied cache from the prior single-pass write-watch run. Fallback entry visits: 106,692 versus 7,906,947 in that preceding run (98.65% lower); hint hits 577,712 versus 576,542. These are separate sequential captures with slightly different work counts, not a randomized timing comparison. The indexed data structure removes full-cache scans; counts are not FPS or memory-bandwidth proof.

Combat 59.99/60.00 FPS; slow-motion activation 59.84; held 60.06/60.00. Target-window p95 21.02-22.76 ms, max 32.41 ms, zero intervals over 33.333 ms. These are simulator guest completion intervals, not Quest/XR presentation. Pacing is not steady 16.67 ms.

Non-compilation combat/activation/held means: draw CPU 1.762/3.090/2.942 ms, draw wall 3.509/6.688/6.011 ms, completion wall 4.930/4.881/4.714 ms. CPU results are mixed against the preceding capture; no measured CPU-time or FPS improvement is claimed. Nested scopes must not be summed.

The local executable includes unrelated preexisting host_input.c edits, excluded from commit. No headset access, APK installation or push. Overall performance goal remains active.
