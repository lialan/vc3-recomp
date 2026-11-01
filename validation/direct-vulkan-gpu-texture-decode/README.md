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


## Follow-up diagnostics

Retained CPU profile (four seconds after flip1560): memmove147, swap_color_rows79, command dispatch52, memcmp43 self samples. Texture decoding and ch_vk_texture_replace_argb no longer appear in the collapsed >=5 table. These are sampling counts, not frame percentages; sampling perturbs timing heavily, so its FPS is not used for acceptance.

RECOMP_FRAME_TRACE initially produced invalid huge timing values under DMA walk because nv2a_pb_scan did not initialize the baseline. Shared executor fix f629e13 initializes it once per DMA walk, including resumed partial methods. The pushbuffer regression checks both cases; missing optional bulk-offer stubs in that standalone fixture were also restored. Native and Android offscreen builds pass.

The corrected, unsampled trace reports last-segment execution averages around11-13ms on time and13-20ms for late flips during slow motion. This includes CPU execution and synchronous Vulkan waits; it is not a GPU measurement or a full-frame duration. Captured trace is included. It supports further investigation of rendering/transfer work but does not isolate guest simulation as the cause.

Vulkan timestamp follow-up completed without CPU sampling. Reports emitted between completed1680 and1920 contain 3785 command-batch submissions, 836.54ms measured batch GPU work, 118.87ms host recording and 1516.64ms host fence waits. These reporting periods straddle the scene markers and exclude standalone texture/depth transfers; GPU execution overlaps fence waits, so these must not be added or treated as exact frame percentages. The evidence shows both substantial GPU work and additional submission/wait overhead. It does not support calling the remaining slowdown pure guest instruction simulation.
