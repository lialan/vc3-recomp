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
windows; extra options come from the environment.
