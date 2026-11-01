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

Remaining performance work includes keeping a render
pass open across draws, with resources retained until explicit completion.
Each draw currently still ends a render pass and synchronizes; target residency
does not yet provide command batching. Preserve queries, CPU edits and fences
while removing those per-draw tile stores/submits/waits. Combat performance
remains an unfulfilled acceptance gate.

## Immutable texture cache and backend phase profile

Native runtime texture/sampler reuse is now enabled by default; set
`CHIHIRO_VULKAN_TEXTURE_CACHE=0` for the uncached control. The bounded cache
checks the exact level-zero encoded storage and P8 palette, with texture and
sampler metadata in the key. GPU images remain immutable while pinned, so
editing a guest address cannot change an image already acquired by a draw.
Unpinned entries are evicted by age; unsupported storage and a full pinned
budget retain the decoder/upload fallback. Invalid guest bounds fail before
decoding. Pins currently release after synchronous GPU completion; future
command batching must retain them until that batch completes.

Quest GPU tests passed cache hits, same-address texel/palette mutation while
the original image remains pinned, sampler changes, pinned-budget fallback,
eviction and guest bounds. Existing render-target feedback, queries, CPU edits,
semaphore visibility, deferred flush modes and overlay comparisons also passed.
The Mac actual-game run completed 60 startup frames with 3,121 cache hits,
14 uploads, no evictions and 7.1 MiB live storage. Captures 30-32 are
byte-identical to the preceding reduced-combiner implementation. This is
startup/correctness evidence, not combat acceptance. Reports and log excerpts
are in `validation/direct-vulkan-texture-cache/`.

A fresh locked Quest run of APK `7235efe9fc0f556b72b6cec7e661ab1e98e3bdbb56d0fc90d6e28ffe8ecca366`
verified its unique log marker. It briefly reached FOCUSED, then lost the
running/focused session. Subsequent unfocused guest intervals remained about
400-440 ms. A representative one-second backend bucket had 130 batches:
surface setup 2.5 ms, CPU color observation 132.1 ms, imports 325.4 ms,
draw 92.7 ms and writeback 320.3 ms. Texture resolution was only 3.9 ms
of the draw phase. Fence logs recorded four sleeps totaling about one second;
the slow consumer still advances semaphore values, so these timeouts alone
do not establish a broken fence implementation. These are overlapping
producer/consumer measurements and must not be added together as frame time.

Depth import/writeback and repeated submissions are now the primary performance
investigation. The GL reference keeps per-surface depth on the GPU. Vulkan's
strict D32 image path currently converts to/from quantized guest depth around
draws. Removing those transfers must preserve guest CPU edits, shared zeta
ownership, depth precision, queries and completion visibility. Tracking loss
means this run is not a worn combat comparison. The test app was stopped,
its environment restored and proximity automation disabled before releasing
the shared lock.

### Depth precision prerequisite

`ch_vk_target_create_depth` now allows an explicit native D24S8 attachment;
the existing constructor and runtime continue using D32S8. This is a
prerequisite for depth residency, not an enabled performance optimization.
The explicit constructor rejects unsupported formats instead of silently
changing comparison precision. Float depth/stencil transfers convert the D24
depth aspect's packed storage independently of stencil.

Quest Adreno tests passed exact normalized depth round trips at the endpoints,
neighboring values and the midpoint, independently varying stencil bytes.
Resident D24 draws and an otherwise identical path with CPU depth/stencil
round trips after each draw produce identical color for ALWAYS, EQUAL and
LESS comparisons, with depth and stencil checked afterward. Invalid NaN input
is rejected before transfer. The complete existing D32 renderer suite also
passes. The Mac KosmicKrisp device reports D24S8 unsupported and the standalone
fixture explicitly skips there; it provides no Mac D24 correctness evidence.
See `validation/direct-vulkan-depth24/quest-renderer-checks.log`.

### Opt-in depth residency

`CHIHIRO_VULKAN_RESIDENT_DEPTH=1` now selects native D24S8 residency when
supported. The default remains the strict path while game validation continues.
Unsupported D24 attachment formats retain strict D32 transfers; Z16 also stays
strict. One depth owner is retained across consecutive draws. Target/zeta/layout
changes publish the previous owner before importing the next, preserving shared
and overlapping depth ranges. Exact raw snapshots detect CPU edits, merging
depth and stencil separately into the GPU image. Padding does not cause a GPU
merge. Fences, flips, texture/palette aliases and teardown publish pending depth.
Unknown texture storage conservatively flushes. Color/depth storage aliasing
fails explicitly until unified image ownership is implemented. Host routing
hooks currently require both residency modes disabled.

Actual Quest tests cover 100 repeated bindings with one import/no writebacks,
depth-only and stencil-only CPU edits while the other GPU component remains
dirty, row padding, Morton storage, shared and shifted zeta ownership, texture
ranges starting before depth, P8 palette aliases, invalid bounds, precise depth
visibility queries, and real executor semaphore publication. All existing
renderer/deferred/XR factory tests also passed. See
`validation/direct-vulkan-depth-resident/quest-renderer-checks.log`.

A fresh opt-in game run of APK
`f3f05a6f2b2ee4c91df9b5c7a76a9b76052ba2f4a8fb92e58c0ef75345ecb1ee`
completed at least 60 startup frames. At frame 60 the depth counters recorded
3,459 bindings, 375 imports and 387 writebacks, replacing strict per-draw
transfers. Representative profile buckets show writeback reduced to 0.1-0.2 ms
per second of draws. Unfocused guest frame intervals were mostly 200-215 ms,
still far from the combat target. Color observation and synchronous draws remain
large costs. This is a different unfocused run, not a controlled speedup claim
or combat/correctness acceptance. The session again left running/focused state;
the app was stopped, environment restored and automation disabled before lock
release. Log excerpts and APK identity are preserved alongside the test log.

### Scope color observation to actual accesses

Resident color now checks the destination before each draw/clear and source
targets before sampling, instead of scanning every allocated target per draw.
Completion still checks every target. Host routing notifications retain their
existing full observation. Source checks use the whole encoded level-zero range
and P8 palette range, including views beginning before a render target; unknown
storage conservatively reconciles all targets. A cache span fix also includes
the complete shared chroma pair for odd-width YUY2/UYVY.

Quest tests cover CPU edits to a dirty sampled source, a pitched texture view
beginning before the source, a dirty target used as a P8 palette, and mutation
of the last chroma byte of a one-texel YUY2 image while its previous GPU image
remains pinned. Existing renderer/depth/query/fence tests pass.

Fresh Quest startup captures 0-3 are byte-identical before/after scoped color
observation. The scoped APK is
`07fbbf9a6579ca872463297b44c72afef2a51cd4ef7f256943ffceebf7d0efdf`
(built before the additional odd-YUV span fix). In a representative unfocused
bucket, 474 batches took 100.1 ms for color observation, 168.1 ms for imports
and 269.2 ms for draws; guest intervals remained roughly 130-150 ms. This is
still not acceptable gameplay performance. Both runs lost running/focused
session state, so no controlled combat speedup is claimed. Mac fallback-to-
strict-depth startup completed 60 frames, with captures 30-32 byte-identical
to the earlier cache checkpoint. See `validation/direct-vulkan-scoped-color/`.

Longer local exploration also found a real remaining correctness failure:
the attract run rejects a surface reinterpretation at guest frame 916.
The GL reference grows/reuses framebuffer storage when clip dimensions change;
native Vulkan currently requires identical dimensions. This must be resolved
before claiming the full game runs. A headless launch cannot use the SDL
window input self-test; use `AUTOKEYS`/`AUTOAIM` for headless scripted input.
The local runner now records those controls and depth-residency flags in its
report so the evidence includes the actual test configuration.

## Target reuse and longer game flow

The failing attract diagnostic identified the exact reuse at `80408000`:
640x480 linear (`format=00000124`) to 256x256 Morton
(`format=08080228`), with pitch 2560 and four bytes per pixel. Target storage
changes now reconcile CPU edits, publish resident color/depth, release the
old view, and initialize a new target from encoded guest storage. The old
guest footprint is retained, so a later larger clip can recover pixels
outside a temporarily smaller view. The backend also checks the entire new
footprint against other color targets before accepting a resize; overlapping
distinct target addresses still require unified ownership and fail explicitly.

Quest tests pass shrink/grow with masked color draws, an intervening CPU edit,
pixels outside the smaller clip, linear/Morton reinterpretation, and D24
ownership across target destruction/recreation. After a partial depth clear in
the smaller target, a precise query on the regrown target sees exactly the
four updated pixels. Existing renderer/deferred/XR tests remain passing.

A native Mac attract replay completed 1,000 frames and captured the cinematic
at frame 917. Its trace did not contain a resize, so it proves longer execution
but does not itself reproduce the previously failing transition; a longer
replay is required. Separately, a headless timed-input probe reached Mission
Select, captured at frame 900, and reached at least frame 1,140 before its
125-second limit. It did not establish combat or frame pacing acceptance.
`AUTOAIM` expects coordinates such as `0.5,0.5`; the initial probe used `1`,
which places the horizontal aim at the right edge. Future gameplay probes
must use explicit coordinates and schedule input for the slower game rate.
Reports, diagnostic excerpts, Quest checks and these game captures are in
`validation/direct-vulkan-target-reuse/`.

The extended attract replay reached frame 1,108 and began actual skinned 3D
draws. It then rejected a DXT1 256x256 texture at `81A96780`, with nine stored
levels and `filter=02062000` (linear texels with linear mip interpolation).
This is now a concrete game-path mipmap requirement, not a synthetic feature
request. The reference GL renderer decodes level zero and generates its mip
chain with `glGenerateMipmap`; its default policy enables that for guest mip
filters 3-6, more than one stored level, and dimensions at least 4x4. Native
Vulkan should implement that generated-chain policy rather than silently
dropping filtering or assuming GL uploads the guest's stored lower levels.
The failed extended report/excerpt is preserved here. Combat rendering and
performance remain unverified. The newest Quest candidate builds without GL
imports, but this latest APK has not been installed for a full game test.

## Generated mipmaps

OpenGL and Vulkan now share `nv2a_texture_mips.h` for chain eligibility and
full-chain length. Vulkan uploads decoded level zero, generates the remaining
RGBA8 levels using filtered GPU blits, transitions every level to shader-read
layout, and supports all four guest mip filters (3-6). Point/linear requests
still sample level zero; small/single-level textures retain GL's linear fallback
for a mip request without an eligible chain. No stored lower guest levels are
decoded, matching the reference upload policy. Convolution/cube sampling still
fails explicitly. Native anisotropy and GL's streamed/render-target exceptions
remain separate parity work; the shared predicate establishes eligibility,
not complete texture-cache policy equivalence.

The immutable cache regenerates mips when encoded level zero changes, retaining
the preceding pinned chain. Its budget now includes estimated mip image bytes
and the retained base staging buffer as well as encoded storage/palette.
Quest tests cover old/new mip chains at the same guest VA and a budget that
admits the base image but rejects the larger generated chain.

Actual Adreno GLES/Vulkan comparisons produced byte-identical pixels for all
seven levels of a 64x32 generated chain and twelve minified draws: all four
mip filters at three scales, including fractional LOD. Nearest-sampling tests
avoid exact texel-boundary ties without relaxing pixel tolerance. Existing
renderer, depth, query, deferred completion, overlay and injected XR tests pass.
Both Mac OpenGL and native Vulkan builds pass. The isolated native Quest APK
build/import audit also passes, with no production GL imports; it has not yet
been installed for a full game run. See `validation/direct-vulkan-mipmaps/`.

The longer native Mac attract run passes the former frame-1108 mip-filter
rejection and reaches frame 1,244 before another draw is rejected. Its sampled
texture is ordinary 256x256 Morton ARGB, single-level linear filtering, so
the next investigation is the rejected render state. The frame-1200 capture
is mostly black and is not visual proof of correct 3D. Full gameplay and Quest
combat performance remain unverified.

A repeat with full rejected-state diagnostics completed 1,300 frames. Its
trace explicitly exercised both `80408000` and `80660000` changing from
640x480 linear to 256x256 Morton and back, without rejection. This provides
actual-game coverage of the earlier target-reuse fix. The repeat did not
reproduce the frame-1244 draw rejection; it is not declared resolved. The
new diagnostic prints raw-batch presence, alpha/blend/cull/depth/stencil state
on future rejected draws. Its frame-1238 capture shows the communication
panel, not a verified combat scene. The next bounded run uses timed coin/start
input and explicit `AUTOAIM=0.2,0.5` to select the first mission.

## Directional cube textures

The timed mission probe reaches a real skinned 3D draw at frame 757 with a
64x64 DXT5 cube on combiner stage one (`stage_program=00000061`). Native
Vulkan now supports cube-compatible six-layer images, cube views and typed
cube descriptors on all four combiner stages. Directional sampling uses XYZ,
without projecting it as 2D UV. Missing 2D and cube descriptors use separate
valid white resources. Cube stages retain the general combiner shader rather
than entering the 2D modulation reduction.

OpenGL and Vulkan share `nv2a_texture_cube.h` for the stored face stride:
each full guest mip chain is padded to 128 bytes, in +X/-X/+Y/-Y/+Z/-Z order.
Both decode only each face's level zero and use linear/clamped cube sampling.
The native cache checks the complete six-face encoded footprint, validates
guest bounds, and retains immutable old cubes while changed faces are uploaded.
Uncached draws decode/upload six faces directly. A CPU resolver's single plane
does not substitute for six faces: it falls back to the guest cube decoder.

Actual Quest GLES/Vulkan comparisons pass six face orientations, interpolated
directions, shared edges and corners: eight cases are byte-identical and two
corner cases differ by at most one UNORM step. A real executor DXT5 decoder
test covers padded stored mip-chain strides, all six decoded face colors,
changed-face invalidation while an old cube remains pinned, out-of-bounds
rejection, and cached/uncached stage-one draws. The existing renderer suite
also passes. See `validation/direct-vulkan-cubes/`.

A subsequent mission replay stops on `stage_program=00001821`, whose stage
two requests mode 6. The GL fragment-stage helper returns zero for modes above
4; native validation still rejects them. Porting that existing reference
behavior is the next compatibility step. A separate run stopped on an invalid
color layout at frame 460, so a surface-description diagnostic has been added;
that rejection has not been declared resolved. Full mission rendering and
Quest combat performance remain open. The cube APK build/import audit passes;
the production candidate has not yet been installed for a full game run.
