# Shared Vulkan shader-fetch and cache checkpoint

The confirmed production bug is pipeline-cache persistence: the trained cache grew beyond the 128 MiB payload limit, so serialization was rejected and newly compiled entries disappeared on restart. Runtime load/save and build packaging now share a bounded 512 MiB limit in vk_pipeline_cache_file.h. A 159085530-byte cache saved successfully, later accumulated caches loaded/saved successfully, and a 192019126-byte payload packaged into 32961362 compressed bytes. Oversized envelopes remain rejected; checksum, device/UUID checks and atomic replacement remain intact. No mid-game serialization was added. The large blobs and generated C are under /tmp/vc3-geometry-check, not committed.

## Shader candidate

CHIHIRO_VULKAN_SPECIALIZE_FETCH=1 opts into immutable shader metadata specialization. It is OFF by default because current frame-time evidence does not establish a win. The implementation is shared and contains no title/address/scene checks.

- Programmable vertices specialize input formats and their conservative instruction-read mask, removing unused input fetches. Only pipeline creation scans instruction metadata; transforms remain on the GPU.
- Fixed-function vertices specialize attribute formats, skin/composite/lighting/material flags and texgen/texture-matrix enable flags. Numeric matrices, lights and colors stay per-draw values. Shared GL/Vulkan vertex arithmetic is unchanged.
- Specialized combiners specialize missing/2D/cube sampler identity. Descriptor contents, coordinates, factors and texture lifetimes remain dynamic and unchanged.
- Fixed shader build optimization is enabled. New shader binaries require corresponding driver-cache coverage; an old Quest seed does not cover them automatically.

Offline SPIR-V specialization/optimization of one illustrative unlit composite case reduces static OpLoad instructions 369 -> 224, conditional branches 184 -> 67, and total instructions 3658 -> 1815. This demonstrates removable code, not actual driver ISA, executed memory transactions, frame-time benefit or Quest performance.

## Correctness and missing geometry

Native VC3 and Android chihiro_vulkan builds pass. Four headless GPU fixture executables pass KosmicKrisp synchronization validation with the candidate enabled: fixed specialization (48 skin/light/material/texgen/fog/clipping cases, exact sample counts and <=1 UNORM byte), program rendering and compact/expanded encoded streams, specialized combiners including 2D -> cube -> missing -> different 2D transitions, and texture/cache/coherence tests. Fixed/program winding tests cover strip/fan, both front-face conventions, FRONT/BACK/FRONT_AND_BACK culling, with exact expected coverage. An initial new-fixture failure was an accumulated sample counter in the test; resetting the counter between reference/candidate draws fixed it.

Final candidate completed 2100 frames with normal combat and E.S. slow-motion captures. Both enemies, room geometry, HUD and E.S. effects are visible in final-combat-slow.png. This does NOT close the user's missing-polygon report. No headset was touched and an exact reproduction of the reported holes is still needed.

A temporary no-culling diagnostic was removed from production source. It changes static pillar/slat shading but did not restore a broad missing object in these captures; cull-zoom.png is normal left / no-cull right. Do not claim pixel identity. The older missing actor at frame2040 reappears in newer normal-cull captures, so equal flip indices do not establish equal gameplay/animation state.

The OpenGL reference disables culling by default and often uses a texture/diffuse/lightmap approximation instead of full combiners. Enabling GL_COMBINERS=1 produces a visibly broken dark scene (glrc-vk-1440.png, GL left). Thus color differences alone do not locate a Vulkan defect, and this GL control is not a correctness oracle. Default OpenGL's usable image remains a visual reference.

## Performance limits

All timing is Mac guest completion, not display or Quest. Scene windows: combat1322..1560, activation1562..1680, held1682..1920. Logs attribute graphics pipeline creation to the following completion interval. Excluding those frames does not eliminate every possible driver/cache effect.

The intermediate candidate before texgen specialization had combat p95 22.443 ms vs control19.887; repeat29.356 vs20.292. Other game processes and then a large external clang/link build overlapped; external-build.txt records the latter. Neither pair supports enabling the candidate. No other agent process was stopped.

Final texgen-specialized capture is a compilation-containing training/visual run under contention, NOT a warm acceptance run. Its combat/activation/held p95 is19.433/19.893/19.230 ms; zero-logged-pipeline-frame p95 is19.369/19.203/19.182. Compilation spikes reach265/388/296 ms and FPS is54.72/47.19/56.44. Do not present these as steady60 numbers. The latest default-path comparison is the intermediate fixed-warm-off: combat59.96FPS/p9519.887, held59.98/p9520.429; activation includes a267.868ms compilation stall. It is one contended run, not a fresh final-default acceptance result.

OpenGL combat p9517.043 ms remains unmet. Missing polygons, stable warm-cache p95, and updated Quest driver-cache coverage remain open. No APK install, headset run, remote push or performance-completion claim. Raw logs, source-era binaries and hashes are in /tmp/vc3-fetch-p95 and /tmp/vc3-geometry-check. The local binaries also include preserved unrelated host_input.c and earlier vk_target.c boundary diagnostics, excluded from this checkpoint commit.


## Follow-up pipeline identity audit

A final-code monitored comparison again encountered another game's runs (all candidate23 / control21 activity samples). Warm candidate/control combatp95=21.146/21.095ms, activation24.320/23.174, held23.349/21.307. Compilation remains present and no parity/win is established; candidate stays opt-in. Actual process samples are in monitored-runs.json. No other workload was stopped.

The optional pipeline log now includes stage-program bits, cull/front state and a semantic key fingerprint, excluding Vulkan headers/pointers/padding. This is diagnostic only and does not change pipeline selection. It separates new application variants from previously trained ones across process restarts. Two2100-frame default-path runs completed. In the repeat,121 creations with keys seen in the preceding run all took<=2.435ms, with zero>20ms; the new keys include114.807ms and52.759ms compilations. Thus the cache does reuse covered variants, while this scripted path still varies enough to expose new keys. There is no full-coverage/no-compilation claim. Repeat combat59.57FPS/p9519.986ms, activation51.01/p9529.486, held60.00/p9519.836; external activity means this is diagnostic evidence, not isolated acceptance.

All observed texture stage modes were0,1,3. The unimplemented>4 stage-fetch modes therefore do not explain missing geometry in this captured route. This rules out one hypothesis, not the user's visual report. Raw follow-up evidence is /tmp/vc3-final-warm. The updated native build differs from6771434 only by these diagnostic fields and the already-preserved unrelated edits.
