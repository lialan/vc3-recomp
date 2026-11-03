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

## Install

`~/Games/VC3/`: `vc3` (VR), `vc3-flat`, `lib/`, `launch.sh` (VR) and
`launch-flat.sh` from this folder. Add `launch.sh` to Steam as a non-Steam
game. The game folder (`vc3.xbe`, `media/`) is `~/work/vc3-game` unless
`game-dir.txt` names another; `env.txt` holds extra options.

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
most, no frame off its vblank. `pipelines.recipes` beside the game (from
long scripted runs: combat, E.S., death, continue, game over, name entry,
the attract demos) is added to the save's once per seed by the launchers;
what play adds on top is precompiled from the next start. The log line
`[Vulkan pipelines] precompiled N of N recipes in X ms` reports it.

## Performance defaults and switches (`env.txt`)

### Immersive VR

The game's 3D is drawn from each eye at the panel's native 2160x2160
(SteamVR recommends 1728; `VC3_IMM_EYE=n` for n x n, up to 2560), with
foveated shading (`VK_KHR_fragment_shading_rate` attachments on the eye
images): full rate within 30 degrees of each eye's view axis, 2x1 to 45, 2x2
to 60, 4x4 beyond -- 12%, 25%, 52% and 9% of the image
(`VC3_XR_FOVEATION=inner,outer,far`; `0` off). Coverage stays per pixel, so
edges stay sharp; textures soften toward the lens edge. Screens-on route at
2160: E.S. held 70 of 239 game frames late without it, 0 with it; combat
and E.S. activation 0-2. Mipmapped textures are
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
  maps made from them sample the eyes.
- **Eye images sampled in place** (`CHIHIRO_VULKAN_IMMERSIVE_LEND=0`: copied
  first, up to four 24 MB copies an E.S. frame, ~2.7 ms of GPU).
- Texture cache sized for eye snapshots; the culling frustum widened to 140
  degrees while immersive (`VC3_IMM_CULL_FOV`); the pipeline cache saved
  every ~10 s when it grew, not only on a clean exit.

Headroom: `RECOMP_VBLANK_HZ=120 VC3_XR_CADENCE=0` runs the game faster than
real time, as fast as the pipeline allows (native 1728/eye: combat ~101 fps,
E.S. held ~90, E.S. activation ~83). `bench-light.sh` reports the headset
side too (`/tmp/takestat.py`: display frames per game frame, 2 at 60 fps).

Game zooms (a far enemy, E.S., the death close-up: the lens narrows from
~45-60 to ~14 degrees) zoom the headset's world: vertices are read back
through VC3's usual 45-degree lens, so a longer one magnifies, at most 1.75
times (`CHIHIRO_VULKAN_IMMERSIVE_ZOOM_MAX`, 0 no limit: from 2 on, the
textures a mip level sharper, E.S. held ran 141-151 of 239 frames off its
vblank with the headset on against 52-58 unzoomed; 1.75 cost nothing
measurable); never zoomed out (`CHIHIRO_VULKAN_IMMERSIVE_ZOOM=0`: no zoom; 1: the OpenGL
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
