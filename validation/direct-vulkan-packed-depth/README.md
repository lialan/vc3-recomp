# Exact packed resident D24 transfers

Resident D24 depth/stencil transfers now use integer words (depth31..8,
stencil7..0) between guest RAM and the Vulkan transfer buffer. This removes the
GPU packed -> normalized float -> guest packed conversion sequence. Native GPU
buffer depth and stencil aspects remain separate, with the same layouts, waits
and completion behavior. Linear guest rows copy only their pixel bytes; Morton
transfers retain guest offset mapping. CPU edits merge with masks per component.
Z16/D32 and existing float API clients retain the original path.
`CHIHIRO_VULKAN_DEPTH_PACKED=0` selects the diagnostic float staging control.

Actual Adreno tests pass65536 exact GPU words, every depth bit position, every
stencil byte, extrema, and a float-control roundtrip. D32 rejects packed writes
before flushing its pending work. CPU tests cover rectangular Morton layout,
pitched padding, bounds/atomic rejection and Z16 rejection. The entire resident
ownership suite now runs in both float and packed modes: component CPU edits,
shared and overlapping zeta, GPU copies, full clears, texture/palette sampling,
resize and real executor semaphore publication all pass. All previous GPU,
GLES comparison, deferred completion and injected-XR tests pass.

The same O2/A78 binaries reach240 startup frames in each mode. First120 intervals
measure15.37FPS / mean65.04ms / p99 205.68ms for float staging and19.77FPS /
mean50.58ms / p99 195.01ms for packed staging. This single startup pair is useful
transfer evidence, not a sustained combat benchmark or synchronized game image
comparison. Frame times remain far above the target. Both sessions clean their
UUID trees and verify the original GL APK is unchanged under the shared lock.

Mac and Android builds pass. The refreshed APK audit has no OpenGL imports and
is build-only, with no installation/headed acceptance. Full gameplay, immersive
attachment and Quest combat/frame-time acceptance remain open.
