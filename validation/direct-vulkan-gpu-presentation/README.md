# GPU presentation without CPU frame staging

The shared Vulkan backend now offers a borrowed-target presentation callback.
The Quest host uses a shared three-slot GPU mailbox and copies its immutable
snapshot directly into the XR swapchain. Normal presentation no longer loads
and converts guest RAM to an RGBA CPU frame, copies it into a CPU mailbox, or
copies/uploads it into a separate screen target. There is still a GPU snapshot
copy and a GPU copy to XR, with completion fences; this is not zero-copy rendering.
Snapshots are allocated on first use/resize, then reused only after release.

The producer never overwrites a taken frame. Unconsumed frames can be replaced;
all slots held returns VK_NOT_READY, dropping only presentation. Queue operations
remain serialized by the host queue mutex. Take/release use only the mailbox
mutex; neither performs Vulkan work. Destruction refuses outstanding consumers.
CPU-written movie/idle surfaces are observed and imported before GPU handoff.
Explicit screenshot requests retain diagnostic readback; ordinary GPU presentation
allocates no CPU frame pixels. Frame accounting and pipeline-cache saving remain.

A second change avoids GPU readback when importing CPU edits into a clean resident
color target: its GPU contents equal the published baseline, so current CPU pixels
already contain the complete merge. Dirty images retain per-pixel GPU/CPU merging.

**Remaining:** guest color/depth publication at completion boundaries still reads
back GPU data and waits. Removing that requires a sound CPU-read/alias ownership
policy. This change does not remove those waits, repair the reported missing
polygons, or establish steady 60 FPS. No new shader/pipeline identity is introduced.
No headset was accessed and no APK was installed in this checkpoint.

## Validation

Native VC3 and Android VC3 builds pass. Actual Mac Vulkan texture/coherence tests
pass synchronization validation, including exact mailbox pixels, three held slots,
pressure, replacement, reuse, source destruction, resize, and backend GPU presentation
with CPU edits in strict/resident/packed modes. CPU presentation callbacks are not
called when the GPU callback is configured. Existing guest-memory parity cases pass.
The injected XR fixture uses real Vulkan images: destination/source bytes, timeout
retry, image layout and fence-gated release pass synchronization validation. This is
not a live OpenXR runtime or Quest result. The unrelated external-FD test is explicitly
skipped on macOS. No Validation Error or SYNC-HAZARD in final archived logs.
An initial added test called the optional flush callback in strict mode; the test was
corrected to honor the existing NULL callback contract, then the final suite passed.

## Mac OpenGL vs Vulkan evidence

Both builds use this worktree, Release, generated-code -O1, same input script on
executor-flip clock, fresh save/config directories, 640x480, 4:3, mono, 60 FPS setting,
no interpolation, no sound/window. Vulkan uses Mesa KosmicKrisp on Apple M4 Pro;
OpenGL uses Apple's native GL path. These are offscreen host-completion intervals,
not physical display timing or an API-intrinsic comparison.

The isolated capture pair records frames 1320 onward. Comparison PNGs show OpenGL
on the left and Vulkan on the right. Lighting/texture differences are clear.
Actors/HUD animation also differ despite equal input/frame counters: the game is not
a deterministic replay. These captures do not yet isolate missing geometry from
animation, culling, depth or shading. Do not label the missing-polygons issue fixed.

A subsequent pair disables screenshots, detailed render profiling and GPU timing.
It reuses the warmed driver caches; this does NOT prove every pipeline is compiled.
A single sequential pair, no statistical repeatability claim:

| Window (inclusive flips) | OpenGL FPS | GL p95 ms | Vulkan FPS | VK p95 ms |
|---|---:|---:|---:|---:|
| Combat 1322–1560 | 60.00 | 17.04 | 59.31 | 24.39 |
| Activation 1562–1680 | 54.29 | 27.24 | 47.21 | 21.16 |
| Held slow motion 1682–1920 | 57.36 | 26.32 | 59.27 | 22.63 |

Vulkan maximum intervals: combat 100.15 ms, activation 423.25 ms, held 103.11 ms.
Those spikes are not attributed by the lightweight run. The cache loaded and saved
95,220,076 bytes, which does not establish a compile-free run. The profiled capture
pair is archived separately (performance.json); do not mix its numbers with this pair.
An earlier accidentally overlapping capture attempt was excluded from timing analysis.

These measurements precede the final clean-color import change and do not benchmark
the new Quest GPU presentation path (the Mac's normal framebuffer consumer still
requires CPU pixels). Thus no FPS gain is claimed for the new handoff. Combat pacing
is worse in Vulkan; slow-motion results are mixed, rather than universally slower.

Raw runs and hash-preserved executables: /tmp/vc3-mac-gl-vk-compare.
Reports contain only renderer/input environment settings. Compressed logs and
comparison PNGs are archived here. The binaries include preexisting host_input.c
and vk_target.c timing diagnostics, excluded from this commit. Local changes only.
