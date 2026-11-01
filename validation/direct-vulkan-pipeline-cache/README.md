# Shared persistent Vulkan pipeline cache

Opt in with `CHIHIRO_VULKAN_PIPELINE_CACHE=/existing/directory/cache.bin`
(the current game's prefix, e.g. `VC3_VULKAN_PIPELINE_CACHE`, also works).
The common Vulkan runtime loads at attachment and saves on normal shutdown
or the diagnostic frame limit. No default path, scene identifier, guest address,
or game-specific warmup is embedded. Parent directories must already exist.
A failed save is nonfatal. Interrupted runs before shutdown do not update it.

`ChVkBackendConfig.pipeline_cache` lets any host provide a same-device cache.
The host retains ownership, keeps it alive through backend destruction, and
serializes external cache access with backend callbacks. Targets share compiler
data while keeping their pipeline objects and mutable descriptors independent.
If a host supplies its own cache, the runtime does not override it or persist it.

`ch_vk_pipeline_cache_file_load/save` provide optional persistence. The file
envelope stores a versioned magic, byte length and 64-bit FNV-1a checksum.
It rejects truncated/trailing data, corrupt bytes, payloads above 64 MiB, and
Vulkan headers with incompatible version/size/vendor/device/cache UUID. The
checksum detects accidental corruption, not hostile-file authenticity. Cache
data is local driver output. Invalid/missing files start an empty cache; Vulkan
creation failure with supplied data retries empty. Saving uses a unique sibling
temporary file and atomic rename. Windows saving is currently unsupported.

## Measurements

Apple M4 Pro, KosmicKrisp, 640x480 local headless VC3, 60 FPS cap, sound off;
resident depth and native-style fragment rounding enabled. Both runs use the
same immutable binary, SHA256
`86fae16fed53deaa82f8bb6d8f79b50c5253fa05fed2cc77066ba408c6fc7d83`.
Each completed 2100 guest frames, exit 0, no runner failure diagnostics.
The warm run loaded 14,797,575 driver bytes from the preceding cold run.
The envelope adds 24 bytes. Complete window measurements and environment are
in [measurements.json](measurements.json).

| Window | Cold FPS | Warm FPS | Cold maximum ms | Warm maximum ms |
| --- | ---: | ---: | ---: | ---: |
| Combat 1321..1441 | 60.01 | 59.98 | 22.12 | 24.43 |
| Combat 1441..1561 | 59.44 | 59.98 | 53.91 | 21.95 |
| E.S. activation 1561..1681 | 34.35 | 36.62 | 131.66 | 33.95 |
| E.S. held 1681..1801 | 42.43 | 43.90 | 135.49 | 30.33 |
| E.S. held 1801..1921 | 46.57 | 47.49 | 24.94 | 28.95 |

The 11 pipeline creations between completed frames 1560 and 1680 total
199.238 ms cold versus 3.651 ms warm. The preliminary probe independently
reduced activation maximum to 32.78 ms, but the table uses the final API/file
implementation. These results establish repeat-run compilation/pacing benefit,
not a universal steady-state speedup. Cold-first-run compilation and held E.S.
throughput remain unresolved. Driver/device changes can invalidate the cache.
No headset, device installation, XR acceptance or hardware performance claimed.

Evidence: `/tmp/vc3-persistent-cache-{cold,warm}/report.json` and game.log,
immutable `/tmp/vc3-persistent-cache-vc3`, cache copies
`/tmp/vc3-persistent-cache.bin.{cold,warm}`. Frame 1740 of the warm run was
visually inspected and shows the E.S. HUD, active enemies and bullet effects.
This is a scene sanity check, not whole-game pixel equivalence.

## Validation

The render-texture fixture compares exact guest memory through CPU edits and
depth/stencil publication before and after cache-file reload. It proves borrowed
cache survival after backend destruction and tests bad magic, checksum,
truncation, trailing bytes, oversized length, and wrong header UUID/vendor/
device/size/version. Failed saves must not create a cache or overwrite a valid
file. Existing render-texture, paired readback and queued-upload fixtures pass.

KosmicKrisp validation + synchronization validation: passed, no VUID/SYNC errors,
`/tmp/vc3-pipeline-file-kosmickrisp-expanded.log`.
Lavapipe validation + synchronization validation: passed existing fixtures and
unsupported-persistence fallback, `/tmp/vc3-pipeline-file-lvp-expanded.log`.
This installed lavapipe reports a 32-byte size but exports only 20 bytes; the
helper refuses to persist that incomplete Vulkan header. It does not exercise
the successful disk roundtrip covered by KosmicKrisp.
Mac main and Android offscreen builds pass; Android was build-only.

## Earlier in-memory cache baseline (historical)

The renderer now retains a VkPipelineCache across its targets and target
resizes. Pipeline objects, descriptor pools and the bounded LRU remain local
to each target. The backend owns and destroys the driver cache after all
surfaces; targets borrow it only before their first pipeline creation.
Standalone targets retain their existing private-cache behavior. Access is
serialized through the backend queue lock / single executor contract.

The specialized-combiner GPU fixture shares one cache across interpreter and
specialized targets, destroys/recreates targets, rejects rebinding, and still
passes exact/toleranced rendering and 140-program LRU checks. The renderer
snapshot fixture passes including resize and teardown. Mac executable and
Android offscreen builds pass. These are local correctness/build results;
no actual-game speedup or Quest runtime acceptance is claimed yet.

The separate immutable empty-clip run /tmp/vc3-empty-clip-profile was still
active near frame 1080 when this change was completed. It predates shared
caching and must not be used as evidence for this optimization.
