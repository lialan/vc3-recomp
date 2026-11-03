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

## Performance defaults and switches (`env.txt`)

### Immersive VR

The game's 3D is drawn from each eye at the headset's native size (1728 per
eye on the Frame; `VC3_IMM_EYE=n` for n x n). Combat and E.S. hold 60 fps
there, each game frame shown twice at 120 Hz, with ~45% of the GPU and the
busiest CPU thread ~33% busy. What gets it there:

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

Diagnostics: `CHIHIRO_VULKAN_IMMERSIVE_LOG=1|2`, `CHIHIRO_VULKAN_IMMERSIVE_DUMP=dir`
(scene eyes and guest picture), `VC3_XR_DUMP=dir` (the headset's eyes and HUD),
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
