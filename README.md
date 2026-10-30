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
