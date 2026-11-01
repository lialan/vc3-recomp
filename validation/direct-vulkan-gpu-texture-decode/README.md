# Shared GPU texture decoding

Local simulator only; no headset or device operations. Warm-cache steady 60 FPS remains the acceptance target. Cold compilation stalls are allowed.

The Vulkan texture cache can upload encoded 32-bit guest texels and dispatch compute to unswizzle rectangular Morton textures, convert ARGB bytes to RGBA, and force XRGB alpha. Formats 0x06, 0x07, 0x12 and 0x1e are supported for 2D textures. Other formats and cubes retain the reference path. This is shared backend code without game addresses or scene-specific decisions.

Exact CPU snapshots and write tracking still establish coherence. Pending upload fences precede staging reuse; pinned sampled images remain immutable. GPU staging's encoded bytes count against the same cache budget. GPU decoding and mip generation share the upload submission. Resource replacement reuses its compute helper. The helper borrows the pipeline cache and preserves its lifetime contract.

96 pixel/mipmap cases pass on KosmicKrisp and lavapipe under synchronization validation: rectangular Morton, pitched/odd dimensions, ARGB/XRGB, all mips, repeated queued replacements, CPU/GPU alternation and short-input rejection. Render-texture and write-watch coherence fixtures pass on both drivers. Logs contain no VUID or synchronization hazards. Native and Android offscreen builds pass.

The preceding GPU-depth CPU profile is included as a collapsed self-sample diagnostic, not a wall-time or GPU percentage. Texture decode and ARGB conversion remain visible after removing depth conversion, motivating this change.

## Warm-cache comparison

Same immutable binary, 2100 flips, 640x480 mono headless, KosmicKrisp on Apple M4 Pro. GPU vertex programs and GPU depth packing enabled in both arms. Each run starts from an identical copy of the populated pipeline cache. CPU/GPU and GPU/CPU run orders:

| Run | Slow activation FPS | Early held FPS | Late held FPS | Held p95 ms |
| --- | ---: | ---: | ---: | --- |
| CPU control | 56.77 | 60.13 | 60.20 | 20.81 / 20.93 |
| GPU decode | 58.85 | 59.91 | 60.13 | 23.25 / 21.38 |
| GPU repeat | 58.94 | 59.99 | 59.46 | 21.23 / 21.86 |
| CPU reverse control | 54.69 | 55.00 | 59.96 | 23.40 / 20.81 |

Normal combat windows remain near 60 FPS. Activation improves in both comparisons, but held throughput and p95 are mixed. This removes CPU texture graphics work and improves activation; it does not establish steady 60 FPS. Frame1740 was visually inspected as E.S. combat, not GAME OVER. Complete reports include binary hash, environment and capture hashes.

Shared runtime enables this for compute-capable graphics queues. Set CHIHIRO_VULKAN_GPU_TEXTURE_DECODE=0 for the CPU control. Direct backend callers select gpu_texture_decode. Resource bounds are checked against maxStorageBufferRange and compute dispatch limits before staging mutation. Other formats, CPU snapshots, guest color conversion and memory coherence still consume CPU work.

