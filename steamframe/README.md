# Virtua Cop 3 on Steam Frame

A native AArch64 Linux build, made on the Frame itself (SteamOS has clang,
cmake, ninja, glslc, SDL2 and the Vulkan and OpenXR headers), Vulkan on
Turnip, VR through SteamVR's OpenXR runtime with the Quest's host
(`runtime/src/xr/quest_host.c`, `CHIHIRO_XR_DESKTOP`).

## Build (on the Frame)

```sh
# the lifted code (games/vc3/generated) comes from scripts/lift.sh on the Mac
cmake -S . -B build-xr -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHIHIRO_RENDERER=vulkan \
      -DCHIHIRO_XR_DESKTOP=ON -DOPENXR_LIB=$HOME/work/vc3-deps/libopenxr_loader.so \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
ninja -C build-xr vc3                      # VR
cmake -S . -B build-vk -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHIHIRO_RENDERER=vulkan \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
ninja -C build-vk vc3                      # flat
```

SteamOS's `libopenxr_loader.a` is an empty stub: link the Steam Runtime
loader (`libopenxr_loader.so.1` and its `libjsoncpp.so.26`, e.g. from the
HOTD4 Frame package) and ship both in `lib/` beside the game.

With a pipeline seed from `harvest.sh` (below), add
`-DCHIHIRO_VULKAN_PIPELINE_CACHE_SEEDS=$PWD/games/vc3/pipeline-cache/steamframe/turnip-seed.cache`
to the `build-xr` line: the binary carries the harvested pipelines compiled
(2.4 MB compressed; `[Vulkan pipeline cache] packaged_bytes=...` at start).

## Install

`~/Games/VC3/`: `vc3` (VR), `vc3-flat`, `lib/`, `launch.sh` (VR) and
`launch-flat.sh` from this folder, and the harvest's `pipelines.recipes`.
Add `launch.sh` to Steam as a non-Steam game. `env.txt` holds extra options.

Game files: copy your own game folder (`vc3.xbe` and `media/`, see the main
README's "Game files") to the Frame; none is in this repository. With ssh
enabled on the Frame, from the computer that has it:

```sh
rsync -a /path/to/vc3-game/ steamos@<frame-address>:work/vc3-game/
```

The launchers look in `~/work/vc3-game` unless `~/Games/VC3/game-dir.txt`
holds another path, and stop with "no vc3.xbe in ..." if it is missing.

## What the Frame needs

- **SteamVR's per-app refresh rate.** The Frame runs every app at its
  per-app rate, 72 Hz by default, whatever the game requests; `launch.sh`
  sets `preferredRefreshRate` for the game's keys with `vrcmd`.
- **The game on its own VkDevice** (default on desktop OpenXR): SteamVR puts
  per-display-frame waits on the session's queue, and sharing it halved the
  frame rate. Frames cross to the headset's device through shared buffers
  (`vk_xr_bridge.c`).
- **sRGB swapchains** (SteamVR offers no 8-bit UNORM), **PipeWire audio**
  (SDL's PulseAudio backend never opens), and in the flat build
  `SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD=1` for the controllers.

## Benchmark

`bench.sh <label> [binary]` runs the scripted combat route (2100 frames) and
prints frame times and costs for the combat, E.S. activation and E.S. held
windows; extra options come from the environment. Its profiling (per-draw
timers) costs a few ms a frame: `bench-light.sh` runs the same route without
it and scores the frame intervals as a player sees them. Both keep the
pipeline cache between runs (`/tmp/vc3s/save`).

## The headset loop never waits for the GPU

SteamVR gates the session's queue once a display frame, so every wait for
the GPU in the headset loop cost up to a display frame: the world copy, the
HUD import and refresh, the overlay clear and its copies took the loop to
8-27 ms (budget 8.3 at 120 Hz) and SteamVR throttled it to 120/n Hz, down to
20 Hz, with the headset on. The loop's own GPU work (world and HUD copies,
overlay exports) is recorded into a ring of command buffers and submitted
without a wait; OpenXR needs it submitted before the image's release, no
more. Fences are waited for only when a command buffer comes round again,
and the world copy's before take_frame gives its shared buffer back.
`VC3_XR_LOOP_LOG=1` prints, once a second, the loop's rate, its time in
xrWaitFrame and its work, split by step.

The loop paces itself only when SteamVR has clearly stopped pacing it (16
loops in a row under half a display period; `VC3_XR_SELF_PACE=0`: never),
and stops as soon as xrWaitFrame blocks again. Sleeping whenever a single
xrWaitFrame came back early started that frame late; SteamVR throttled the
app to 60 and the sleep, on its own clock, kept every later frame late: 40,
30, 24 Hz, back to 120 and down again every ~20 s, 59% of frames
reprojected in play (SteamVR's own count, `Cumulative stats for pid` in
`~/.local/share/Steam/logs/vrcompositor.txt` when the app exits).

Measuring as when worn: SteamVR's `power.pauseCompositorOnStandby` 0 keeps
the session rendering (state 5) with the headset off, but the display stays
in standby and SteamVR *drops* every frame -- it never judges whether frames
were on time. Also set `power.turnOffScreensTimeout` high
(`vrcmd --set-settings-float power.turnOffScreensTimeout 100000`): frames are
then displayed (`0 dropped`), reprojected and throttled exactly as in play.
Set both back after (`--set-settings-bool ... 1`; "true" reads as false;
timeout 5.0). `~/work/lit.sh` on the Frame does this around a scripted run.

## Shaders compiled before play

A pipeline's first build compiles its shaders: up to ~100 ms each on the
Frame, ~220 on the combat route (18 s cold), so a new scene used to stall
play for seconds. Every pipeline the game builds is recorded in
`save/vulkan-pipelines.recipes` (its key and the target properties it
depends on); at start-up 4 threads (`CHIHIRO_VULKAN_PRECOMPILE_THREADS`, 0:
record only) build every recorded one into the pipeline cache while the game
boots (~19 s to attract mode), so play finds them compiled (~1 ms each).
Measured cold (no driver shader cache): 217 in-game compiles, 17 frames off
their vblank -> 184 recipes precompiled in 10.6 s, in-game builds 3 ms at
most, no frame off its vblank. Recipes describe the game's own shaders, so
none is in the repository: the first time you reach a scene it can stutter
once, and from the next start it is precompiled. To record ahead of play,
run long scripted routes with `bench.sh` (combat, E.S., death, continue,
game over, name entry, the attract demos) and copy the run's
`/tmp/vc3b/<label>/save/vulkan-pipelines.recipes` to
`~/Games/VC3/pipelines.recipes`: the
launchers add a `pipelines.recipes` beside the game to the save's once per
seed. The log line
`[Vulkan pipelines] precompiled N of N recipes in X ms` reports it.

To cover every scene before anyone plays it, `harvest.sh [seconds]` plays
each mission unattended (`play.sh`: sweeping aim, a shot every 0.25 s,
reloads, the pedal, Start and coins to continue -- the mission 2 boss is
~11 minutes in), merges every recipe the runs recorded with the installed
game's, and boots once from an empty save with only those, filling a fresh
driver cache: `/tmp/vc3p/harvest/pipelines.recipes` goes beside the game,
`turnip-seed.cache` into `games/vc3/pipeline-cache/steamframe/` and the build
(`-DCHIHIRO_VULKAN_PIPELINE_CACHE_SEEDS=...`, see the README there), so the
binary starts with them compiled. A cold compile in play costs 100 ms to
1 s; `play.sh` alone is also the way to look for slow scenes (its log has
`[stats]` every 5 s and `[immersive switch]` lines).

What no harvest has met yet is built in the background
(`CHIHIRO_VULKAN_ASYNC_PIPELINES=0`: in the draw, as before): a draw whose
pipeline is neither recorded nor built goes on with the generic combiner and
fetch shaders, or -- a vertex program's, which has no generic form -- is
left out until its pipeline is ready, the first time ever. A third-mission
run from an empty save with Mesa's shader cache off: 59-61 fps throughout,
150 pipelines built off the draw, one 0.8 s compile left (the generic
combiner shader itself, once per device); the same scene in play had
stopped the view for up to 3.6 s.

## Performance defaults and switches (`env.txt`)

### Immersive VR

The game's 3D is drawn from each eye at the panel's native 2160x2160
(SteamVR recommends 1728; the settings panel's Resolution per eye, 1216 to
1920 or Native, from the next start; `VC3_IMM_EYE=n` for n x n, up to 2560), with
foveated shading (`VK_KHR_fragment_shading_rate` attachments on the eye
images): full rate within 30 degrees of each eye's view axis, 2x1 to 45, 2x2
to 60, 4x4 beyond -- 12%, 25%, 52% and 9% of the image
(`VC3_XR_FOVEATION=inner,outer,far`; `0` off). Coverage stays per pixel, so
edges stay sharp; textures soften toward the lens edge. Screens-on route at
2160: E.S. held 70 of 239 game frames late without it, 0 with it; combat
and E.S. activation 0-2.

Dynamic resolution keeps later, heavier areas at 60 (area 3 at a fixed 2160
ran at 40 fps in play): after half a second with 2+ late game frames the
eyes drop a level -- 90, 80, 70% of each eye's width and height, the picture
in a corner of each eye's half, the rest clipped (no cost) and outside the
headset's `imageRect` -- and come back after 3 calm seconds (longer after a
failed try). Each level has its own foveation pattern, kept on each eye's
view axis. `CHIHIRO_VULKAN_IMMERSIVE_SCALE=1`: native always (or 0.9/0.8/0.7
fixed); level changes are logged as `[immersive] render scale`. At 2560 per
eye (an overload): late frames combat/E.S. activation/held 5/8/0 against
4/15/13 with the fixed pattern. Mipmapped textures are
filtered 8x anisotropically (`CHIHIRO_VULKAN_ANISO=n`, 1 off; free on the
Frame). With the headset on, SteamVR's compositor works at 120 Hz on the
same GPU, and every full-view layer it blends costs: the HUD is a quad layer
of its own at the game's 640x480 (`VC3_XR_HUD_OVERLAY=1`: drawn into the
overlay as before) and the overlay, now only the gun, laser and dot, is 0.75
of the eye size (`VC3_OVERLAY_SCALE`). Game frames shown exactly twice
(scripted route, compositor kept running with `power.pauseCompositorOnStandby`
0): 84% with the HUD in a full-size overlay, 89% at overlay 0.75, 96% with
the HUD quad (game frames shown four times: combat 7, E.S. activation 23,
E.S. held 12). 2048 per eye showed 71% against 1728's 84%; with the
compositor idle it held every frame, so measure with it running (below). What gets it there:

- **Per-draw constants in uniform buffers** (constant registers on Adreno);
  devices enable `VK_KHR_uniform_buffer_standard_layout` for the std430
  blocks. Without it the GPU vertex path is off (CPU transforms).
- **Fetch specialization** (`CHIHIRO_VULKAN_SPECIALIZE_FETCH=0`: the generic
  vertex shaders, ~2500-3400 instructions each).
- **Only the eyes are drawn** for screen-shaped surfaces in immersive frames
  (`CHIHIRO_VULKAN_IMMERSIVE_SKIP_GUEST=0` draws the guest copy too); effect
  maps made from them sample the eyes. Visibility queries on those surfaces
  count as visible (the pixels the query covers) instead of a flat draw that
  waits for the GPU each time -- their flat depth is stale anyway; the
  mission 2 boss runs ~1500 a frame and sat at 10-15 fps
  (`CHIHIRO_VULKAN_IMMERSIVE_ZPASS=0`: the flat draw and its real count).
- **A slow frame spread over several flips stays in the eyes**: a flip that
  drew nothing on the presented surface shows the last picture again
  (`[immersive switch] ... the last picture again`), without handing the
  headset a second copy; dropping to the screen there drew the next frame
  flat as well, slower still.
- Effect buffers (the glow VC3 adds back onto its scene) get eyes at half
  the size each way; `CHIHIRO_VULKAN_IMMERSIVE_EFFECT_DIV=4` a quarter:
  ~0.25 ms less GPU a frame in combat at native 2160 (0.88 -> 0.64 ms for
  that buffer), a softer glow -- an option for a weaker GPU (the Quest).
- **Eye images sampled in place** (`CHIHIRO_VULKAN_IMMERSIVE_LEND=0`: copied
  first, up to four 24 MB copies an E.S. frame, ~2.7 ms of GPU).
- Texture cache sized for eye snapshots; the culling frustum widened to 140
  degrees while immersive (`VC3_IMM_CULL_FOV`); the pipeline cache saved
  every ~10 s when it grew, not only on a clean exit.

Headroom: `RECOMP_VBLANK_HZ=120 VC3_XR_CADENCE=0` runs the game faster than
real time, as fast as the pipeline allows (native 1728/eye: combat ~101 fps,
E.S. held ~90, E.S. activation ~83). `bench-light.sh` reports the headset
side too (`/tmp/takestat.py`: display frames per game frame, 2 at 60 fps).

Where the GPU goes (combat route, native 2160, scale fixed with
`CHIHIRO_VULKAN_IMMERSIVE_SCALE=1`, headset off, GPU timestamps): the eyes
~4.8-5.5 ms a frame -- the scene's ~430-490 draws ~3.5-4 ms (fixed-function
3D is ~85% of draw time: pixels, not draw count), the presented surface's
eyes ~1.2 ms (its full clear, color and depth, ~0.8; the composite of the
scene ~0.45), the glow buffer at half size 0.6-0.9 ms; the game's own
640x480 surfaces ~0.3. Knobs for a weaker GPU (the Quest), each measured
alone: foveation off 6.38 ms, the default 30/45/60 degrees 5.46,
`VC3_XR_FOVEATION=20,35,50` 4.97 (-9%, a smaller sharp centre);
`CHIHIRO_VULKAN_IMMERSIVE_EFFECT_DIV=4` -0.25 ms (softer glow); a lower
Resolution per eye scales the rest. Ruled out: presenting the scene's eyes in
place of the presented surface's (VC3 clears or reuses the scene buffer
after compositing it, before the flip). Run to run the route's GPU time
varies ~0.5 ms: compare within one run (split the 4320x2160 lines by draw
count) or repeat.

The heaviest scene measured is mission 2's first area (`stage2a`; the left
mission on Mission Select): 390-850 eye draws a frame, 7-10.6 ms of GPU at
native 2160 (`AUTOAIM=sweep:26:0.2,0.5 CHIHIRO_VULKAN_PROFILE=1
CHIHIRO_VULKAN_GPU_TIMESTAMPS=1 CHIHIRO_VULKAN_IMMERSIVE_SCALE=1 play.sh m2
120`, the 4320x2160 lines summed per frame from 45 s on). Its cost follows
pixels, not draws: ~13 us a draw at full scale, 6.45 at 0.7 (half the
pixels), so almost nothing per draw beyond its pixels; and not shading:
`VC3_XR_FOVEATION=10,20,30` takes only 8% off. What is left is raster,
depth and colour traffic -- overdraw. Rendered in tiles (`TU_DEBUG=gmem`
here; Qualcomm's own driver, the Quest's, usually tiles) the eyes
cost +60% (20 us a draw) and the other targets twice as much: each eye
surface's batch ends 4-6 times a frame where another samples it (scene ->
glow -> composite -> trail), and every end stores and reloads whole images.
For the Quest that is the work: fewer of those ends, then load/store ops.
A full clear that opens a batch is now the render pass's own clear (load
op CLEAR) rather than one drawn inside it: mission 2 -4% (12.70 -> 12.35 us
an eye draw, two runs each), tiled 20.2 -> 19.5 us; combat unchanged
(`CHIHIRO_VULKAN_PASS_CLEAR=0`: as before). Ruled out: leaving out the corners of each eye's picture that the lenses
never show (XR_KHR_visibility_mask: 7% of a picture on the Frame) -- no
measurable change. Run to run the same route varies ~5%.

Game zooms (a far enemy, E.S., the death close-up: the lens narrows from
~45-60 to ~14 degrees) zoom the headset's world: vertices are read back
through VC3's usual 45-degree lens, so a longer one magnifies as much (up
to ~4.6x in combat). A smaller magnification shows targets smaller than the
game judges hits by, and shots that look on target miss, so there is no
limit by default (`CHIHIRO_VULKAN_IMMERSIVE_ZOOM_MAX=n`: at most n). From
2x on the textures are a mip level sharper, which cost E.S. a third of its
frames before the dynamic resolution; the resolution now steps down for
it. Never zoomed out (`CHIHIRO_VULKAN_IMMERSIVE_ZOOM=0`: no zoom; 1: the OpenGL
renderer's normalisation to the most used lens). Aim and the HUD panel use
the same projection.

What the headset gets: VC3 draws its scene off-screen and composites it
onto the presented surface with one full-screen draw, so a frame whose
presented eyes were composited from eyes counts as 3D (before, frames with
none of the few direct 3D effects fell to the fill-view screen: the 2D
picture over the view). When the presented picture holds no scene but the
frame drew one (game over, name entry), the scene with the HUD over it;
the screen only once a frame has no 3D. The HUD panel follows the game's
lens down to its usual 45 degrees and no further, so the death and
continue close-up (~14 degrees) no longer shrinks "CONTINUE?".

Diagnostics: `CHIHIRO_VULKAN_IMMERSIVE_LOG=1|2` (with 1: every switch
between eyes, scene and screen, and 2D draws over 20% of the HUD), `CHIHIRO_VULKAN_IMMERSIVE_DUMP=dir`
(scene eyes, guest picture and HUD), `VC3_XR_DUMP=dir` (the headset's eyes and HUD;
with `VC3_XR_FAKE_GUN=1 VC3_XR_DUMP_ZOOMED=1500` the left eye's overlay with the
HUD panel at the death/continue close-up),
`CHIHIRO_VULKAN_DRAW_TIMING=1` (GPU time per draw kind), and Mesa's
`IR3_SHADER_DEBUG=vs,fs` with `MESA_SHADER_CACHE_DISABLE=1` (shader stats).

### Flat

E.S. mode runs at 60 fps on the Frame (about 13 ms of work per frame).
What gets it there, and how to turn each part off:

- **Guest completions don't wait for the GPU** (`CHIHIRO_VULKAN_LAZY_COMPLETION=0`
  restores waiting and writing rendered color back at each kickoff/semaphore)
  and **depth stays on the GPU** (`CHIHIRO_VULKAN_COMPLETION_DEPTH=1` publishes
  it again). The OpenGL renderer has no completion hook at all; VC3 never
  reads rendered pixels or depth from the CPU.
- **Batch ring**: four command buffers per target, a batch submitted every 64
  draws (`CHIHIRO_VULKAN_BATCH_RING=0`, `CHIHIRO_VULKAN_BATCH_LIMIT=n`).
- **Frame queue**: two finished frames may wait for the display and are shown
  in order, each for two 120 Hz frames (`VC3_XR_FRAME_QUEUE=1`: newest only).
- Eye-mapping diagnostics: `VC3_XR_EYE_TEST=1` tints the left eye's overlay
  red and the right eye's blue; `VC3_XR_SWAP_EYES=1` draws each eye's overlay
  from the other view. The log's `[xr] eyes:` line shows the runtime's layout.

Benchmarking: the Frame's speed varies by a few ms with SteamVR's state, so
compare builds back to back; a cold pipeline cache adds ~1 s shader-compile
stalls; the forced exit at `VC3_VULKAN_FRAME_LIMIT` often crashes at shutdown
(an existing shutdown-order bug, not seen in play). `CHIHIRO_VULKAN_WAIT_STACKS=1`
prints where the host waits for the GPU.
