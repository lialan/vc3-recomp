# Remaining CPU graphics audit

Follow-up correction: the residual estimate below omitted CPU Morton depth layout transfers. The [Morton transfer audit](../direct-vulkan-gpu-morton-transfer/README.md) adds depth counters and moves that work, plus Morton color transfer, to shared GPU compute. Treat the newer audit as authoritative.

Simulator/headless Vulkan only. No headset operations. All changes are in the shared runtime/backend, without guest addresses or scene-based rendering shortcuts.

## Changes

- Resident linear ARGB color transfers now permute RGBA/BGRA on GPU compute within existing upload/readback submissions, including paired depth/color publication. CPU code copies pitched rows and preserves guest CPU-edit merging. RGBA host callbacks retain their contract; a companion BGRA callback avoids conversion back and forth for the framebuffer presenter. Diagnostic screenshots still pack RGB bytes on the CPU only when requested.
- Vertex shaders now unpack NV2A byte, signed-short, normalized-short and packed-normal attributes. Host preparation validates bounds and snapshots encoded bytes; it no longer normalizes/unpacks vertex formats. Existing triangle strip/fan expansion remains on the CPU.
- Added cumulative CPU geometry, texture-fallback pixel/time and color-fallback pixel/time reports. Added separate executor timing for the one legacy metadata vertex per raw program batch.

## Correctness

Both KosmicKrisp and lavapipe pass the 144 vertex-program arithmetic cases plus 24 attribute format/size cases across256 inputs, including signed zeros, infinities and NaNs. The printed arithmetic-test summary in the captured logs predates its wording correction; source loop executes168 cases. Registered fixed/program render tests pass with packed-short positions, packed colors, strip/fan indices, precise visibility queries, constants snapshots, clipping and rejected bounds.

Color/depth fixture passes exact arbitrary-byte RGBA/BGRA roundtrips, encoding toggles, odd sizes, packed/float depth staging reuse and paired pending draw readback. Backend coherence fixture compares strict and GPU paths with CPU edits, layout changes, pitched/Morton surfaces, resize and pinned texture snapshots. No VUID or synchronization hazards in the saved logs.

## Color-only warm comparison

Same immutable binary and identical populated cache copies;2100flips,640x480 mono headless,Apple M4 Pro KosmicKrisp. GPU vertex programs, depth and texture decode are enabled in both arms. This pair predates the encoded vertex-input and BGRA presentation changes.

| Run | Activation FPS | Early held FPS | Late held FPS | Held p95 ms |
| --- | ---: | ---: | ---: | --- |
| CPU color |57.11|58.96|59.50|23.12 /22.28|
| GPU color |59.40|59.98|60.03|23.37 /22.36|
| GPU repeat |59.38|59.93|60.04|22.08 /22.25|
| CPU reverse |59.38|60.20|60.02|21.84 /22.60|

First pair improves activation; reverse pair is tied. No claim of a consistent large throughput gain or steady60FPS from this change. The CPU graphics conversion is moved to GPU regardless of scheduling noise.

## Combined warm results

Two unsampled2100-flip runs, identical populated caches, include GPU color conversion, native BGRA handoff and vertex attribute decoding. Native and Android offscreen builds pass; no device operations. Frame1740 visually checked as active E.S. combat. Frame1500 is byte-identical to the earlier GPU-texture baseline; later captures are not byte-identical and are not claimed as full-game visual parity.

| Run | Normal combat FPS | Activation FPS | Early held FPS | Late held FPS | Held p95 ms |
| --- | --- | ---: | ---: | ---: | --- |
| Warm |59.98 /60.01|59.18|60.18|60.08|22.24 /22.22|
| Repeat |60.03 /60.03|59.32|60.06|59.89|20.95 /21.61|

Not steady60FPS: activation and frame-time tails remain. Removing bulk CPU graphics emulation does not eliminate Vulkan synchronization, memory coherence, scheduling and guest execution costs.

## Audit

CPU geometry counters are zero, including startup, for these2100-flip runs. In held slow motion the executor reports zero fixed transforms, program transforms, clipping/expansion and CPU draw batches; no requested CPU vertex extents. The one legacy metadata vertex costs0.862–1.061ms per roughly1-second report (about0.02ms/frame). Reports in sampled-audit-held.txt are emitted after the4-second sampling interval; sampling perturbs throughput, so its frame numbers are not performance acceptance.

Warm counter deltas: normal combat flips1320–1560 spends49.166ms in color fallback and1.211ms in texture fallback across240frames. Activation1560–1680 spends24.462ms color and0.886ms texture across120frames. Held1680–1920 spends49.798ms color and0.193ms texture across240frames. Thus residual measured bulk graphics emulation is around0.22–0.24ms/frame including the metadata vertex, roughly1.5% of a16.67ms budget, not literal zero. The final unsampled audit below confirms these orders of magnitude.

CPU sample self-table no longer contains swap_color_rows, vertex format fetch arithmetic, texture decoding, ARGB upload conversion or depth numeric conversion among >=5 occurrences. Copies, comparisons, command dispatch, memory tracking and vertex/index snapshot preparation remain. Self counts are not wall-time percentages.

Unsupported formats, cube textures, Morton color layouts, RGB565 and context-write vertex programs retain compatibility fallbacks; this audit establishes observed VC3 combat/slow-motion behavior, not a guarantee for every game/scene. Shared counters and fixtures let other games verify their own fallbacks.


## Final unsampled audit and scope

Normal combat, activation and held slow motion respectively: CPU texture decode0.00501/0.00760/0.00085ms per frame; color fallback0.20026/0.20151/0.20290ms per frame. Fallback color layouts are128x128 and256x256 Morton ARGB targets. Held metadata vertices cost0.804–0.894ms per roughly1-second report, about0.014ms/frame. Fixed/program CPU transforms, clipping and draw batches are zero; GPU raw declines are zero. Counters are collected at60-flip boundaries.

The measured residual emulation is approximately0.22ms/frame (about1.3% of16.67ms). This is the evidence for concluding that no significant CPU shader/raster/format emulation remains in these tested warm combat/slow-motion scenes. It is not a claim that all CPU graphics code or all fallback paths are removed. Other games, unusual formats and untested scenes must use the same counters to establish their own profile. CPU command interpretation, index assembly, validation, snapshots, memcpy/memcmp, guest write tracking and Vulkan driver submission remain. These are still performance opportunities, but not CPU execution of the original bulk GPU shaders.

The final retained change only limits the diagnostic fallback log to its first occurrence per surface after this unsampled audit. ColorGPU conversion can be disabled with CHIHIRO_VULKAN_GPU_COLOR_PACK=0 for diagnosis. Native and Android offscreen builds pass; nothing was installed or run on a headset.
