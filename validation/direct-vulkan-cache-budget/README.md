# Bounded texture-cache budget diagnostic

`CHIHIRO_VULKAN_TEXTURE_CACHE_MIB` (or the VC3-prefixed form) now sets the
runtime cache's existing byte limit, from1 to1024MiB. The default remains64MiB.
It accounts for encoded snapshots/palettes, estimated image payload and retained
staging payload, not total Vulkan allocation overhead. Entry count, immutable
leases, pin-aware eviction and exact per-acquire content checks are unchanged.
The Quest runner exposes `--texture-cache-mib` and records it in provenance;
the local runner forwards either environment spelling.

The actual Quest256MiB run uses the O2/A78 native build with the Morton helper.
It reaches the bomb battle (capture at1920) but misses2500 frames before its
300-second watchdog; the last reported completion is2100. At2040 it has2053
uploads/1790 evictions and255.6MiB live in263 entries. The preceding64MiB
fast-color diagnostic had7358 uploads/7175 evictions at2040. Those runs differ
in the address helper, capture interval and guest timer phases, so this is not
a controlled same-binary performance comparison.

The256MiB run still measures only6.21FPS at1561..1681,5.55FPS at1681..1801,
5.45FPS at1801..1921 and3.98FPS at1921..2041. The last window has p99408.12ms
and max416.46ms. Cache churn continues, and CPU coherence/import/recording costs
remain large even in windows without new uploads. No budget increase is enabled
by default and no smooth-combat acceptance is claimed.

Mac's four-frame actual guest run confirms256MiB is accepted, remains free of
OpenGL imports and reaches its frame limit. Zero is rejected during Vulkan
initialization before guest execution; the CLI rejects1025 before device access.
Android and Mac builds pass. The separate renderer suite passed after the
Morton change. The refreshed APK is build-only, with its no-GL audit recorded in
`apk-report.json`; it was not installed.

The Quest test used the whole-session shared lock, cleaned its UUID and verified
the original OpenGL APK hash unchanged. Logs retain the boot header and relevant
Vulkan frame/cache/draw/capture rows. Full logs and all periodic PPMs remain at
the report's local output path. Quest immersive correctness and sustained combat
FPS/frame-time requirements remain open.

## Current local Vulkan follow-up

After shared vertex reuse, persistent pipeline cache and texture write tracking, a same-binary 64/128 MiB A/B completed 2100 frames each. Apple M4 Pro / KosmicKrisp; identical warm starting pipeline cache copies, mono headless, sound disabled, GPU timestamps off. No headset used.

| Budget | Activation FPS | Early held FPS | Later held FPS | Final uploads | Final evictions |
|---|---:|---:|---:|---:|---:|
| 64 MiB | 42.14 | 52.04 | 54.78 | 2977 | 3268 |
| 128 MiB | 41.50 | 52.16 | 54.65 | 2975 | 2905 |

CPU texture uploads did not materially decrease. GPU snapshot reuse increased from 994 to 1340, but the pair showed no sustained throughput benefit. Retained 64 MiB default; no claim from this single pair about other scenes or devices. Provenance: current-measurements.json; raw /tmp/vc3-current-cache-{64,128}/report.json and game.log. Next candidate is reusing unpinned CPU texture allocations, while preserving immutable pinned images and queued-upload lifetime, rather than increasing the memory budget.
