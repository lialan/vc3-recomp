# VC3 direct Vulkan continuation

Branch: work/vulkan-direct-20261031. Base: 63d8907, the existing OpenGL VC3
worktree baseline. Zink remains saved separately at
work/vulkan-zink-render-diagnosis-20261031 (db0f4a9).

## Current implementation

`CHIHIRO_RENDERER=opengl` retains the existing renderer and is the default.
`CHIHIRO_RENDERER=vulkan` now builds a separate macOS VC3 executable, registers
the native Nv2aBackend before guest execution, and excludes the GL renderer,
texture decoder and toolkit host D3D/OpenGL layer. The actual executable's
imports and library list contain no GL/EGL/CGL/SDL_GL entries, OpenGL framework
or libepoxy. The guest uses the shared CPU vertex transforms and texture decoder;
Vulkan rasterizes its decoded batches. SDL scanout selects Metal on macOS.
Only headless game execution has been verified so far.

A real VC3 boot completed four native Vulkan frames. A longer bounded run reached
at least frame 32, captured the SEGA logo/credits, then timed out before its
60-frame goal. This synchronous implementation imports/reads back guest surfaces
per draw and is not a performance candidate. No full-game, combat or VR acceptance
is claimed. Startup uses the same XBE/media and isolated save/config directories.

Real execution exposed and repaired a texture-descriptor identity assumption and
missing guest Morton surface storage. Shared guest layout helpers now handle
pitched/Morton ARGB8888/RGB565 color transfers and Z16/Z24S8 depth transfers,
including bounds and padding. Inactive depth is not imported/written unnecessarily.
The 256x256 offscreen startup passes use a stale 640-pixel pitch: Morton layout
must derive from SURFACE_FORMAT, not that pitch.

`validation/direct-vulkan-runtime/` contains actual startup logs, binary/import
records, captures and Quest test evidence. The static logo phase comparison is
within one RGB level of the OpenGL capture, with no spatial masking; fade phase
was selected from surrounding reference captures. This is not synchronized
scene/gameplay parity. Historical `validation/vulkan-*` files refer to earlier
individual binaries; do not treat them as fresh full-game results.

## Reproduce locally

Configure with `cmake -S . -B build-vulkan-direct -G Ninja
-DCHIHIRO_RENDERER=vulkan -DCHIHIRO_GAMES=vc3 -DCMAKE_PREFIX_PATH=/opt/homebrew
-DCHIHIRO_GLSLC=<NDK>/shader-tools/darwin-x86_64/glslc
-DCHIHIRO_VULKAN_CPU_TESTS=ON`, then build that directory.

`tools/run-vulkan-local.py --executable build-vulkan-direct/vc3 --game <game-dir>
--output <isolated-output-dir> --icd <Vulkan-ICD-json> --frames 4 --timeout 30`
checks imports, boots the actual guest, captures pixels and records its bounded
exit. Use a fresh output directory for each run. Run
`build-vulkan-direct/runtime/src/gpu/vulkan/vulkan_guest_layout_test` for the
storage checks without creating a graphics context.

## Quest verification

The user made the headset available again. The installed working APK was read
back and its hash matched the preserved baseline before the standalone checks.
Fresh tests passed on Adreno 740: native offscreen rendering, real executor texture
decoding, blend/depth/stencil/queries, frame submission and scene routing; overlay
comparison against GLES; rectangular Morton and pitched storage; injected-XR
ownership/device contracts with real Vulkan transfers; and all 256 GPU combiner
cases against the shared CPU evaluator. The initial combiner invocation omitted
its expected shader path; a corrected explicit private shader path passed.
Temporary test files were removed. No game APK was installed or replaced.

## Still required

- Port the shared Quest host to native Vulkan device/session/swapchain handling
  and complete game renderer attachment. Android Vulkan game configuration
  currently fails explicitly rather than silently executing GL.
- Reuse shared input/session/menu/overlay logic; complete immersive world/HUD,
  screen transitions, gun/menu rendering and rendered-pose ownership.
- Finish missing sampler/mipmap/cube/effect and surface alias semantics against
  real game evidence as execution reaches them. Preserve guest fence semantics.
- Establish scene-matched intro/combat/E.S./zoom/head-motion/menu/restart parity
  with OpenGL and obtain user acceptance. Then optimize the synchronous path.

Preserve `validation/baseline/VirtuaCop3VR-working.apk` (local, ignored), SHA256
3a0660db0d8a5d8e6d95a555dc087bd8f9351f2f8df91e601599ae2789b53ad6, and the
installed original package. The earlier direct module was recovered from 493ae0b;
there had not been a separate pre-Zink native renderer commit.

A separate four-frame macOS rerun requested VK_LAYER_KHRONOS_validation and
emitted no validation errors/VUID diagnostics; its request and logs are recorded.
