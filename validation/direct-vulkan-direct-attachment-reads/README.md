# Direct attachment packing and paired GPU readback

Shared Vulkan targets can sample D32/S8 and RGBA8 attachments directly into the existing packed guest staging planes. This eliminates intermediate depth/stencil and RGBA transfer-buffer writes and rereads. A paired depth/BGRA read uses one compute dispatch, with independent linear/Morton addressing for each output. Final host copies and guest completion ordering are unchanged; this does not defer or omit guest-memory publication.

The common `vk_image_read` helper owns immutable views, descriptors, sampler and compute pipeline. Target ownership/fences cover their lifetime. The paths check sampled image usage and extent/dispatch support. Native D24, unpacked depth, unconverted RGBA and unsupported configurations retain existing transfer paths. Both defaults are enabled where supported; `CHIHIRO_VULKAN_DIRECT_DEPTH_READ=0` and `CHIHIRO_VULKAN_DIRECT_COLOR_READ=0` select controls. No guest clocks, game-specific logic, CPU graphics conversion or draw visibility changes are introduced.

Per pixel, direct color packing avoids a 4-byte intermediate write plus reread. Direct depth packing avoids an intermediate 4-byte depth and 1-byte stencil write plus reread (stencil plane rounded to four bytes). These are removed logical staging operations, not measured physical DRAM bandwidth. Paired dispatches additionally avoid a second compute dispatch and its buffer barriers. At the final profiling sample the live targets recorded 11,488 direct depth reads, 20,205 direct color reads and 8,568 paired dispatches.

## Validation

Native VC3 and Android `chihiro_vulkan` builds pass. SPIR-V for all three readers is compiled and embedded by CMake. Android is build-only; no headset was accessed. The new shaders are not present in the previously captured Quest driver cache and need device training when access is permitted.

KosmicKrisp Apple M4 Pro depth-pack and render-texture fixtures pass with `VK_LAYER_KHRONOS_validation` and synchronization validation. Logs contain no Validation Error or SYNC-HAZARD. Existing checks cover random packed words, non-grid float packing, exact stencil, odd sizes, repeated upload/download reuse, GPU copies, CPU component edits, resident aliases and byte-exact RGBA/BGRA conversion. New coverage checks all four independent linear/Morton depth/color combinations, delayed caller writes until read-end, and exactly four fused dispatches on the supported test device.

## Controlled operation benchmark

`read-bench.c` uses the existing Vulkan test driver and the same shared library. It alternates disabled/enabled targets three times, excludes 16 warmup reads per target, and times 240 synchronous paired 640x480 reads. All resulting color and depth words are verified. Pipeline creation is outside timing. No validation layers are enabled for timing.

| Run | Transfer/pack mean ms | Direct/fused mean ms |
| --- | ---: | ---: |
| 1 | 0.326896 | 0.261792 |
| 2 | 0.324483 | 0.252571 |
| 3 | 0.343842 | 0.258629 |

Mean of three means: 0.331740 -> 0.257664 ms, 22.3% lower. This is an isolated operation improvement, not a 22% frame-rate claim.

Example build: `clang -O2 -DCH_VK_DEPTH_FRAGMENT_TEST=1 -Iruntime/src/gpu/vulkan -Iexternal/xboxrecomp/src/kernel -I/opt/homebrew/include tools/vulkan-combiner-driver.c validation/direct-vulkan-direct-attachment-reads/read-bench.c /tmp/vc3-mac-shader-checks/libchihiro_vulkan.a /opt/homebrew/lib/libvulkan.dylib -lm -lpthread -o /tmp/vc3-read-bench`. Run with the recorded KosmicKrisp ICD and no concurrent task GPU test.

## Final gameplay pair: pacing remains unresolved

Same executable and same copied cache seed; fresh save/config; headless native 640x480 mono 4:3, sound off, 60 Hz guest clocks, identical flip-clock input. Training runs with profiling; the final off/on pair disables profiling/timestamps and retains per-frame interval plus pipeline creation logs. The two direct-read flags are the only behavior-setting difference. Reports record exact environments, executable hash and seed hash. Both final runs exit 0 at 2100 frames. Other host activity was not globally stopped; one sequential pair is not randomized proof.

| Window | Control FPS | New FPS | Control p95 ms | New p95 ms | New max ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Combat, 1322-1560 | 57.07 | 59.97 | 20.478 | 20.990 | 23.295 |
| Slow-motion activation, 1562-1680 | 58.88 | 59.96 | 20.399 | 20.580 | 24.867 |
| Held slow motion, 1682-1920 | 59.98 | 59.93 | 20.411 | 20.044 | 25.197 |

Pipeline creation in these windows totals control/new 255.486/4.122, 72.213/11.840 and 1.741/1.651 ms. Different scene timing and driver cache state mean the control stalls cannot be credited as an optimization win. A loaded cache is not proof of compilation-free gameplay. After excluding frames with any logged graphics-pipeline creation, combat p95 is 20.020/20.955 ms; activation 20.188/20.398; held 20.411/20.044. Thus this pair does NOT establish a combat-p95 gain. The new windows have no interval above 33.333 ms, but still do not meet steady 16.67 ms or the earlier OpenGL combat p95 of 17.043 ms.

An earlier depth-only pair measured combat 22.633 -> 21.578 ms and held 22.577 -> 21.363 ms, but the final control itself is faster, reinforcing host/workload variability. An earlier standalone asynchronous color-copy experiment was removed after it failed to improve combat p95; its patch/logs remain under `/tmp/vc3-warm-p95` only.

The final profiled combat trace (233 frames with zero pipeline creation) averages draw CPU 1.693 ms, completion CPU 1.381 ms, draw wall 3.364 ms, completion wall 4.546 ms, batch GPU 3.268 ms and batch wait 4.500 ms. These scopes overlap and MUST NOT be summed. Frames above 20 ms have higher GPU time and more time outside backend callbacks; that outside time includes guest work and waits, not pure CPU computation. Remaining work is critical-path submission/guest-coherence and scheduling attribution, not removing correctness-preserving RAM publication blindly.

OpenGL's callback table has no completion flush; its Mac flip reads back the shown surface asynchronously and normally skips other surfaces. Vulkan currently publishes resident color/depth at guest completion boundaries. This is a real semantic/work difference, not evidence that omitting Vulkan publication is safe.

The benchmark binary includes the preexisting host_input.c edit and boundary timing diagnostics, both preserved and excluded from this optimization commit. No APK installation, headset validation, push, polygon-parity claim or overall performance-completion claim.
