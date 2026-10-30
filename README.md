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

Display frame rate: the game runs at 60 either way; at 120 fps the window
shows a motion-interpolated frame between each pair of real ones (made on
the GPU by block-matching motion search; HUD and other still pixels are
copied exactly, scene cuts repeat the real frame). It costs one 120 Hz frame
(8.3 ms) of display latency; the gun reads the mouse as before. Choose it
from the **Display > Frame Rate** menu (60 fps / 120 fps; F10 toggles), which
saves `fps=120` to `~/Library/Application Support/VC3/settings` for the next
launch; `VC3_FPS=60|120` overrides the saved choice for one run
(`VC3_CONFIG_DIR` moves the settings folder). Best on a 120 Hz display.
With `VC3_FPS_LOG=1` the `[FPS]` lines add `shown` (frames put on screen per
second) and `made` (interpolated frames per second). For checking frames
headless: `VC3_FPS_DUMP=<prefix>` writes each group as BMPs
(`<prefix><flip>_<k>.bmp`, last = real frame) while the file named by
`VC3_FPS_DUMP_FLIPS` exists (or always, with `1`); `VC3_FPS_DEBUG=1` logs
the scene-cut measure per flip (stalls; debug only).
