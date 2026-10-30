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

Controls: mouse aims the gun, left button fires, right button reloads,
middle button is the gun's CHANGE button; Space is the cabinet's foot pedal;
5/6 coin, 1/2 start, F2 test, F1 service, F10 toggles 60/120 fps. The
**Controls** menu rebinds each of these keys (and can give fire and reload a
key too): choose an action, then press the new key in the game window
(Escape leaves it unbound); the choice is saved as `key_<action>=` in
`~/Library/Application Support/VC3/settings`. Saves go to
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
  screen a third wider and extends the 3D view into the sides (hor+). In
  play the HUD panels at the screen's sides (score, gun and lives) move out
  to the edges, the rest of the HUD stays where it was, and the lock-on rings
  stay on their targets; menus and the mission map stay 4:3, so what you aim
  at is where the game looks; fades and full-screen passes cover
  the whole width. Movies and full-screen 2D pictures (menus, logos) stay
  4:3 in the middle with black sides, not stretched. The gun aims at the
  middle 4:3 as before; the sides count as off the screen (a shot there
  reloads). The game culls what its own 4:3 view cannot see, so objects can
  appear late at the side edges.
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
`VC3_STEREO=1` shows the Quest's stereo pair side by side in the window (left
eye, right eye), for checking the 3D without a headset.

## Meta Quest

`quest/build.sh` builds `quest/VirtuaCop3VR.apk` (Quest 3 / 3S / 2 / Pro): the
same recompiled game for arm64 Android, its GL back end on OpenGL ES 3.2, shown
on a large screen in front of you (an OpenXR quad layer the headset keeps
steady at its own refresh rate, 120 Hz asked for, while the game runs at 60),
in stereo 3D: the 3D view is drawn once per eye with depth around the
screen's plane -- the street behind the screen, what is near coming out of
it -- while the HUD stays on the screen.
It needs the lifted code (`scripts/lift.sh`), the Android SDK with NDK 27, a
JDK 17+, and SDL2 and the OpenXR loader built for arm64 Android in `$DEPS`
(default `~/Downloads/VR/deps`; the lindbergh-recomp-ports checkout's
`quest/deps.sh` makes the same folder). No game files go in the APK:
`quest/install.sh /path/to/vc3` installs it over adb and copies your game
folder to `/sdcard/VirtuaCop3` (the app asks for all-files access).

In the headset the controller is the gun: aim at the screen (a red dot shows
where) and pull the trigger; pull it pointing away from the screen, or squeeze
the grip, to reload; the other hand's grip is the foot pedal. A / X inserts a
coin and starts, B / Y inserts a coin, the
left menu button is the cabinet's test switch. The other hand's thumbstick
moves the screen (up/down: nearer/farther, left/right: smaller/larger); the
left thumbstick click turns 3D on and off, the right one switches 4:3 / 16:9.
The Quest defaults to 2x, 16:9 and 3D; everything is saved in the app's
settings file (`scale=`, `aspect=`, `stereo=`; `VC3_STEREO_DEPTH` moves the
depth the screen sits at, in the game's units, about metres, default 8). `env.txt` in `/sdcard/VirtuaCop3`, one `NAME=value`
per line, sets the usual `VC3_*` options; the last run's log is
`/sdcard/VirtuaCop3/vc3.log`. Without an OpenXR runtime (or with
`VC3_NO_XR=1`) the game shows in the Android window instead, which is how it
runs in the Android emulator.
