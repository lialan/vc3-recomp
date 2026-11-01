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
Quest now also builds a native Vulkan game APK through the shared Quest host.
Input/actions, menu painter and hit testing, tracking/view publication, refresh
selection, frame lifecycle and guest startup remain common with OpenGL. Native
presentation lives in `quest_vulkan.inc`; it creates the runtime-selected Vulkan
device through `XR_KHR_vulkan_enable2`, hands that borrowed device to the guest
renderer, serializes the shared queue, and copies owned CPU frame packets into
opaque XR screen quads. Gun/effect overlays use the shared geometry/math and
native Vulkan passes; the CPU-painted menu uses its own Vulkan quad. Headset
input/overlay/menu interaction acceptance is still pending. No GLES context or
GL fallback is created in the Vulkan host.

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
Temporary test files were removed. These were standalone tests; subsequent
native game integration evidence is described below.

The native candidate `org.vc3recomp.vulkan.direct` (label `VC3 Vulkan Direct`)
created a real Vulkan OpenXR session on Quest 3, submitted layers successfully,
and displayed the startup text and SEGA screen in both eyes. The user confirmed
the screen was visible but very slow. Current startup logs show roughly 1-2
completed guest frames/s; this is not playable or performance acceptance.
`validation/direct-vulkan-quest/` records the native APK/import audit, actual
headset captures/log, and fresh offscreen/injected-XR checks. Tracking initially
blocked capture and the user restored it. The first capture-directory attempt
failed due to shell-owned directory permissions; writing directly into the
app-owned external files directory fixed it. The next run found an uninitialized
presentation target, now cleared before import. Both failures stopped execution.

A packaging error initially installed the candidate under the original ID:
the module's ext.appId overrode Gradle's project property. It was caught before
launch and the exact preserved working APK was reinstalled. Gradle now reads
CLI IDs through providers.gradleProperty, and the build helper checks the APK's
actual manifest ID and native contents before producing it. The original APK's
on-device hash matches the preserved baseline after testing. Its settings were
not changed. Subsequent candidates use the separate package and per-app env.txt.
All bounded test runs were stopped and the temporary proximity override disabled.

Pipeline compilation results now persist in a VkPipelineCache. Consecutive draws
also reuse a pipeline when immutable blend/depth/stencil/cull/shader state matches;
texture descriptors, combiner uniforms, alpha-test push constants and effect data
still update per draw. Fresh Quest offscreen checks pass, including texture/state
changes, exact guest CPU edits, queries, and all 48 GLES/Vulkan overlay comparisons.
This does not remove per-draw surface/depth imports, readback, guest writeback,
texture upload, allocations or queue waits. Unlike the reference Quest GL path,
which keeps targets on the GPU and skips routine guest readback, this remains a
synchronous correctness path. GPU batching/residency is the next performance task.

Build the isolated APK with `rtk proxy python3 tools/build-vulkan-quest.py`.
The helper performs no installation/device access. Its output is
`build-quest-vulkan-direct/VC3-Vulkan-Direct.apk` and `apk-report.json`.
It keeps default OpenGL native staging separate. Per-app diagnostic overrides
can be placed in `/sdcard/Android/data/org.vc3recomp.vulkan.direct/files/env.txt`;
they load after the shared game env.txt, before private env.txt. Use `VC3_SOUND=0`
for muted testing. Avoid shell-owned capture subdirectories.

## Still required

- Replace per-draw synchronization/transfers with GPU-resident rendering and
  batching, using the OpenGL implementation's completion/readback behavior as
  reference. Preserve explicit guest queries/fences and CPU edit coherence.
- Complete native immersive world/HUD and screen transitions, rendered-pose
  ownership, and input/gun/menu acceptance. The current game host presents flat
  packets; shared settings still expose options whose native attachment is
  pending. Do not treat the screen candidate as immersive parity.
- Finish missing sampler/mipmap/cube/effect and surface alias semantics against
  real game evidence as execution reaches them. Preserve guest fence semantics.
- Establish scene-matched intro/combat/E.S./zoom/head-motion/menu/restart parity
  with OpenGL and obtain user acceptance. The severe lag is unresolved.

Preserve `validation/baseline/VirtuaCop3VR-working.apk` (local, ignored), SHA256
3a0660db0d8a5d8e6d95a555dc087bd8f9351f2f8df91e601599ae2789b53ad6, and the
installed original package. The earlier direct module was recovered from 493ae0b;
there had not been a separate pre-Zink native renderer commit.

A separate four-frame macOS rerun requested VK_LAYER_KHRONOS_validation and
emitted no validation errors/VUID diagnostics; its request and logs are recorded.

## Shared headset ownership

All access to Quest serial `2G97C5ZJ0G02DQ` must run through
`python3 /Users/lialan/.local/bin/quest3-lock <whole-test-command>`. The shared
exclusive `fcntl.flock` file is `/tmp/quest3-2G97C5ZJ0G02DQ.lock`; never delete it.
Hold the lock for installation, launch, screenshots, logs, stopping the app and
cleanup. Put cleanup in the test script's finally/trap block before exit. Every
ADB invocation must target `-s 2G97C5ZJ0G02DQ`. A busy lock returns 75: continue
local builds/tests/code and retry later. Separate Codex threads do not otherwise
coordinate device ownership; direct ADB bypasses this lock.

## Combat performance acceptance

The user requires Quest 3 Vulkan performance to avoid regression against the
existing OpenGL renderer: target steady 60 FPS in combat, allow occasional dips
but require a sustained high frame rate. The user clarified that frame pacing means no stutter or uneven frame times;
it does not request removal of artificial caps. Preserve this requirement through future backend work. Startup
FPS, offscreen tests and compositor refresh rate do not establish combat FPS.
Record comparable combat scenarios and frame-time distributions on the headset,
including sustained rate, long frames and regularity, then obtain user acceptance.
The present 1-2 FPS native startup candidate fails this performance requirement.

## Resident color and measured combiner reduction

The runtime now enables resident ARGB8888 color targets by default. Set
`CHIHIRO_VULKAN_RESIDENT_COLOR=0` to compare the strict path. Guest storage is
compared against an owned byte snapshot; changed pixels merge into the GPU
target without losing untouched GPU pixels. RGB565 and depth/stencil retain
the strict quantized/synchronous path. Dirty color is copied back at flips,
before sampling a dirty guest target, and at explicit completion points.
The toolkit's optional `Nv2aBackend.flush` hook runs before queued/direct guest
completion writes, kickoff markers and reports. Its deferred proxy replays the
flush on the submission thread. Standalone tests cover precise queries, CPU
pixel edits, self-sampling and semaphore/report visibility, plus both deferred
ring modes. Render routing hooks currently require strict color mode.

A Quest run initially exposed a 64x64 DXT5 texture with 7 guest levels and plain
LINEAR minification. Multiple stored levels do not imply mip sampling: the
reference GL path uses level zero for NEAREST/LINEAR. Native Vulkan now accepts
that metadata while actual mip filters still fail explicitly.

Profiling (enabled only with `CHIHIRO_VULKAN_PROFILE=1`) separates queue ownership,
vertex conversion, texture upload, draw setup/execute, and GPU timestamp
measurements. A successful Quest startup trace attributed about 98% of the
queue time to guest rendering rather than XR presentation. Later launches were
blocked by tracking and returned stale game logs; those are not fresh results.
Future headed runs must include a unique per-run env sentinel and verify it in
the log before recording FPS. Screenshot the system state when the sentinel is
missing. The game candidate and temporary proximity override were stopped.

The shared pure predicate `nv2a_combiner_simple.h` proves texture0*diffuse
modulation, optional texture-only alpha, intervening identity stages, and an
identity final combiner. The adapter uses a plain textured shader only for
that subset, preserving the general combiner for other expressions or varying
projective q. Do not broaden this into a heuristic combiner bypass. Seventy-two
GPU comparisons cover linear/Morton/DXT sampling, bilinear/projective UV, alpha
testing, swapped inputs, identity/final stages and the varying-q fallback.

The actual Mac VC3 startup trace uses the four-stage identity form; all traced
startup batches reduce. Native captures 30,31,32 are byte-identical between the
generic and reduced paths, and both bounded Mac runs completed 60 frames.
This is startup evidence only. On Quest, a same-process synthetic 640x480
four-stage draw benchmark measured 100 separate draws per case:

| Shader | Color readback per draw | Mean per draw |
| --- | --- | --- |
| General combiner | yes | 9.665 ms |
| General combiner | deferred | 6.681 ms |
| Reduced modulation | yes | 0.915 ms |
| Reduced modulation | deferred | 0.571 ms |

All output checksums and renderer checks passed; see
`validation/direct-vulkan-resident/`. These numbers are synthetic per-draw
costs with diagnostics enabled, not combat FPS or pacing acceptance. Set
`CHIHIRO_VULKAN_FORCE_COMBINER=1` to retain the generic path for A/B tests.
A subsequent locked run installed the reduced candidate and verified a unique
`CHIHIRO_TEST_RUN_ID` in its log. It reached FOCUSED then left the running/focused
session within the first five seconds as tracking became unavailable. The guest
continued producing frames at roughly 550-610 ms intervals while unfocused.
This still fails the performance objective and is not a controlled worn/combat
measurement. Do not infer game FPS from the improved synthetic draw cost.
`fresh-headed-run-excerpt.log` records this transition and pacing evidence;
the test app was stopped, diagnostic env restored and proximity override disabled.

Remaining performance work includes texture/sampler reuse and keeping a render
pass open across draws, with resources retained until explicit completion.
Each draw currently still ends a render pass and synchronizes; target residency
does not yet provide command batching. Preserve queries, CPU edits and fences
while removing those per-draw tile stores/submits/waits. Combat performance
remains an unfulfilled acceptance gate.
