# vc3-recomp

Native macOS port of *Virtua Cop 3* (Sega Chihiro) by static recompilation with
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp). No game data is included:
you need your own Chihiro dump containing a decrypted `vc3.xbe` and `media/`.

See [docs/PLAN.md](docs/PLAN.md) for status and roadmap.

```bash
git submodule update --init
python3 -m venv .venv && .venv/bin/pip install capstone
brew install sdl2 libepoxy
VC3_GAME_DIR=/path/to/vc3 scripts/analyze.sh
```

Build and run (the lifted C in `src/recomp/gen` is generated, not committed):

```bash
scripts/lift.sh
cmake -S . -B build -G Ninja && cmake --build build
build/vc3 /path/to/vc3          # or VC3_GAME_DIR=...; muted unless VC3_SOUND=1
```

Mac app: `scripts/build-macos.sh` makes `build/VC3.app` with its Homebrew
libraries (SDL2/SDL3, libepoxy, libcrypto) bundled and ad-hoc signed. It
plays with sound and finds the game folder from `VC3_GAME_DIR`, the folder
saved in `~/Library/Application Support/VC3/game_dir`, or
`~/Downloads/VR/VC3/extracted`, and otherwise asks for it once. No game files
go in the app.

Controls: mouse aims the gun, left button fires, right button reloads;
5/6 coin, 1/2 start, F2 test, F1 service. Saves go to
`~/Library/Application Support/vc3-recomp` (`VC3_SAVE_DIR` to change).

Display settings, from the **Display** menu (each choice is saved to
`~/Library/Application Support/VC3/settings` for the next launch; an
environment variable overrides it for one run, and `VC3_CONFIG_DIR` moves
the settings folder). The game itself always runs at 640x480 and 60 fps;
these change only how it is drawn and shown.

- **Internal Resolution** (`scale=`, `VC3_SCALE=1..4`): draws at 1x
  (640x480, default) up to 4x (2560x1920). The window shows the full
  resolution; guest memory and frame dumps still get 640x480.
- **Aspect Ratio** (`aspect=`, `VC3_ASPECT=4:3|16:9`): 16:9 draws the
  screen a third wider and extends the 3D view into the sides (hor+); the
  HUD stays in the middle 4:3 where it was, and full-screen passes and
  fades stretch to the edges. The gun aims at the middle 4:3 as before; the
  sides count as off the screen (a shot there reloads). The game culls
  what its own 4:3 view cannot see, so objects can appear late at the side
  edges, and 2D banners sliding in from off screen show in the sides.
- **Frame Rate** (`fps=`, `VC3_FPS=60|120`; F10 toggles): at 120 fps the
  window shows a motion-interpolated frame between each pair of real ones
  (made on the GPU by block-matching motion search on the 3D view; the HUD,
  text and lock-on rings come from the real frame as they are, still pixels
  are copied exactly, scene cuts repeat the real frame). It costs one 120 Hz
  frame (8.3 ms) of display latency; the gun reads the mouse as before.
  Best on a 120 Hz display. Effects that appear between two frames
  (tracers, muzzle flashes) can look partial in the made frames.

With `VC3_FPS_LOG=1` the `[FPS]` lines add `shown` (frames put on screen per
second) and `made` (interpolated frames per second). For checking frames
headless: `VC3_FPS_DUMP=<prefix>` writes what the window would get, as
drawn, as BMPs (`<prefix><flip>_<k>.bmp`, last = real frame) while the file
named by `VC3_FPS_DUMP_FLIPS` exists (or always, with `1`), whenever the
window gets more than guest memory holds (above 1x, 16:9 or 60 fps);
`VC3_FPS_DEBUG=1` logs the scene-cut measure per flip (stalls; debug only).
