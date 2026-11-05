# Silent Scope VR on Quest 3: design

Branch `ssc` of `vc3-recomp`. Silent Scope 1 (`ss1.xbe`) first, then
Silent Scope 2 and 3.

## Intent

Silent Scope already runs on the Mac on the runtime it shares with Virtua
Cop 3. The next step is a VR version on the Quest 3 that feels like holding
a sniper rifle: the world around you at full size, a rifle in your hand,
and a magnified picture on its scope that you aim with. VC3's VR work (the
immersive renderer, the settings panel, the gun overlay, the build and test
tooling) is reused, not rebuilt; Silent Scope adds only what a rifle and a
scope need.

## What the player experiences (the user's decisions)

- The game's 3D world surrounds the player at full size, as VC3's does in
  immersive VR.
- A sniper rifle with a scope is held in **one hand, on one controller**.
- The game's own round scope picture is **not drawn** over the view.
- To take a shot, the player looks at the rifle's scope. Its lens shows a
  **magnified picture of wherever the barrel points**; the rest of the world
  stays at normal size around it. The player "aims with the eye and glances
  down at a magnified picture on the scope".
- **Thumbstick up zooms in, down zooms out.**
- The trigger fires; the shot goes where the scope's centre points.
- The aim is **steadied a little**, so hand tremor doesn't make the
  magnified picture jump.
- The settings panel is **VC3's**, minus what doesn't apply.
- Order: Silent Scope 1, then 2, then 3.

## Goals

1. Silent Scope 1 playable from title to the end of a stage on Quest 3 with
   the rifle, the scope picture, zoom and steadying.
2. The game holds its own 30 fps logic rate without hitches; the headset
   shows its normal display rate.
3. Everything new is either generic (usable by VC3 and the later Silent
   Scope games) or lives in `games/ss1/`.

## Non-goals (for now)

- Holding the rifle with two hands.
- The Silent Scope Complete menu launcher (`default.xbe`): the VR app
  starts Silent Scope 1 directly.
- The Steam Frame. The XR host is shared, so it should follow, but it is not
  targeted or tested here.
- The desktop frame interpolation (it is desktop-only and stays that way).

## Anti-goals (failure even if it technically works)

- VC3 gets worse, in VR or flat. Every runtime change is checked against
  VC3 (Mac attract run; Quest build still compiles and links).
- Silent Scope code in the shared runtime. Game addresses and constants
  live in `games/ss1/src/` or in the game's profile.
- A new settings UX. The panel is VC3's; rows are only removed or relabelled.
- A scope picture that freezes on a stale image without the player noticing,
  or that lags or jitters enough to make aiming feel wrong.

## Constraints

- **One shared runtime** with VC3 (`runtime/`, toolkit fork
  `lialan/xboxrecomp` branch `ssc`); the toolkit is pushed before the parent.
- **The headset:** the Quest 3 is used only when the user says so at that
  point, and never while they may be playing (check that the app isn't
  running first). Build and check everything possible on the Mac first.
- Re-lift `games/ss1/generated` with the current lifter before every build
  that is tested.
- The Quest 3 GPU is weaker than the Steam Frame's; measure there, not on
  the Mac.

## How it works (decided from the research)

All addresses below are Silent Scope 1's, from reading its code (see
"Sources").

### The scope picture is the game's own

Silent Scope already renders its scope view every frame, into its own
**256×256 render target** (texture `0x1ED210`, surface `0x1ED214`, depth
`0x1ED218`; X8R8G8B8 swizzled), from a scope camera that sits at the main
camera's position and is rotated by the aim. Its reticle (a 3D model 570
units ahead) and its "WAIT" / "NOW RELOADING" text are drawn into that
target too. The rifle's lens shows this picture.

- The renderer publishes that render target to the XR host each frame,
  alongside the eye pictures and the HUD, through a **generic profile field**
  ("an extra picture the host may show on a model": the surface address).
  VC3 leaves it empty.
- The host draws it on the scope's lens in the overlay layer, as a **flat
  picture on the lens**, readable from any angle (a small screen on the
  scope, not an eye-box optic). *My call, from "glance down at a magnified
  picture on the scope".*
- The game's own reticle and texts are used; no extra crosshair is added.
  *My call.*
- When the game skips the scope pass (it does in some states: flags
  `0x1CEAFC8 & 0x200000`, `& 0x100`, `word 0x1CEB03C == 1`, scope-off
  `0x1CEA0A4`), the lens shows dark glass instead of the last picture.
  The renderer knows whether the target was drawn this frame.
- The picture updates at the game's 30 fps and lags the controller by one
  to two game frames. That is accepted; steadying hides most of it.

### Aiming

The rifle's forward direction (after steadying) is turned into the game's
aim angles **relative to the rail camera's axes**, and written every frame
right after the game's own aim update `sub_000588C0` (the existing ss1
wrapper in `games/ss1/src/wide.c` is extended, in the style of VC3's
`vr_aim.c`).

- Pitch `0x1CEB75C` and yaw `0x1CEB75E`, signed 16-bit, 65,536 per turn.
- Writing after the original bypasses its ±5500 / ±4300 clamps. The scope
  camera has its own culling, so any direction where the game has geometry
  renders.
- Shots start at the scope camera and fly along its forward axis
  (`sub_0003EB30`), so **the shot goes where the scope's centre points** with
  no extra work. The game's aim assist (`sub_00057550`) only nudges the
  on-screen circle, which is no longer drawn.
- The mapping from headset space to the game camera is the one the
  immersive renderer already uses to place the world around the player.

### Zoom

The scope's focal length is computed in `sub_00074610` as
`256 / (2·sin)·cos · [0x1ADFC8]`; `0x1ADFC8` is read nowhere else. It
becomes `game value × player zoom`, set every frame.

- Each stage sets its own base field of view (1.1° to 5°), and scripted
  moments animate it; the player's zoom multiplies on top, so those still
  work. Don't write the field of view itself (`0x1CEB316`): aim assist reads
  it and the scripts overwrite it.
- Thumbstick up/down changes the zoom smoothly while held. Range
  **0.5× to 4×** of the game's value, starting at 1×, kept across stages,
  back to 1× on a game reset. *My call; tune on the headset.*
- The scope's culling follows one frame later. Accepted.

### Not drawing the circle

`sub_00075F40` draws the scope circle and its frame model onto the screen,
and is also used for a fixed bottom-left scope. In VR it is replaced in
`games/ss1/src/` by a version that draws nothing but still computes what the
original computes for `0x1ADFC8` (1.0, or 1/s for the type-3 scope), times
the player's zoom. Using the game's own scope on/off toggle instead would
also stop the scope render and bring back the main-screen crosshair, so it
isn't used.

`sub_00076490` also draws two aim marks at the aim's screen position (models
`0x1DB2482` / `0x1DB2480`, calls at `0x766E4` / `0x7670A`), and the
fixed-scope mode (`0x1CEA0A0 == 1`) draws only those plus the bottom-left
scope. **While the scope pass renders, those marks are hidden too**, so the
lens is the only aiming reference. **When the game skips the scope pass but
still accepts shots, the marks are left as the game draws them**, as the
aiming aid for those moments. *My call.*

### Steadying

The aim direction is filtered (an adaptive low-pass that follows fast
swings closely and smooths slow tremor, e.g. a one-euro filter), with
**more smoothing at higher zoom**. The rifle model itself follows the hand
exactly; only the aim and so the scope picture are steadied. *My call;
strength tuned on the headset, "a little" by the user's word.*

### The rifle

A sniper rifle model built from boxes, like VC3's handgun
(`runtime/src/xr/quest_gun_geometry.inc`, drawn by
`runtime/src/gpu/vulkan/vk_overlay.c`): stock, receiver, long barrel, a scope
tube on top with a round rear lens that carries the picture. The overlay
needs one new thing, a textured lens. The rifle replaces the handgun when
the game's profile says so; VC3 keeps its handgun and sights.

### The world, the HUD and the settings

- **World orientation (the user's decision):** the world follows the game's
  camera fully, turning, looking up or down and tilting with it, so the
  centre of the game's view stays straight ahead of the player. It is not
  levelled to the real floor. This is the mapping the immersive renderer
  already uses (eye matrices in the game camera's view space; Recentre
  corrects only the player's own heading), so no levelling option is added,
  and "aim relative to the rail camera's axes" stays as written.
- **Immersive world:** VC3's `vk_immersive.c`. Its VC3-specific constants
  (the 772.5 px lens `IMM_ZOOM_BASE` / `HUD_K_MAX`, `AIM_WIDE`, the 640×480
  aim conversion, the HUD split rectangles) move into the game profile, with
  Silent Scope's values measured (main view 46.8° vertical). The culling
  widening VC3 does with `vr_cull.c` (140°) is done for ss1 in its own
  folder, extending `wide.c`.
- **HUD:** the game's 2D HUD on the HUD layer, spread toward the edges of
  the view as in VC3, with Silent Scope's own layout rectangles.
- **Settings panel (VC3's):**
  - **Removed:** Sight and Laser (the scope is the sight); Coins, Insert a
    coin, Arcade Test and Service buttons (arcade-only); the **Screen** row:
    Silent Scope is always immersive. The automatic fallback to the virtual
    screen for frames with no 3D (menus, movies) stays as in VC3, so Screen
    distance and Screen size stay for that screen. There is no
    light-gun-style aiming at the virtual screen.
  - **Kept as in VC3:** every other row (detail, display rate, Gun model,
    Gun hand, Stereo 3D, recentre, reset, stats, resolution).
  - **Controls page:** trigger fires, thumbstick up/down zooms, the
    remaining buttons follow VC3's layout where Silent Scope has the same
    action (reload, Start, the menu buttons). Exact buttons are the
    builder's call.
  *My call: these removals.*
- **Display rate:** the headset's normal rate. The game updates the world
  at 30 fps; head movement between game frames is handled by the headset's
  reprojection, as for any 30 fps VR content. The rifle and its lens are
  drawn every display frame at the newest controller pose.

### Packaging and tooling

- A Quest app for Silent Scope 1, copied from `quest/vc3` (Gradle module,
  manifest, activity, strings) with its own package and folder on the
  headset; the Java check for VC3's `media/` folder becomes per game.
- `games/ss1/game.conf` gains the Quest fields (`APP`, `APK`, `PKG`,
  `ACTIVITY`, `HEADSET_DIR`, `LOGTAG`).
- Pipeline seeds for ss1 are harvested on the Quest after a real play
  session, as for VC3.
- A Mac-side test path like VC3's fake-gun dump renders the rifle, the lens
  and a held aim into a picture, so the overlay can be checked without the
  headset.

## Success looks like

The user plays Silent Scope 1 on the Quest 3: the rifle is in hand, the
scope lens shows the magnified view, zooming works with the thumbstick, the
shot lands at the scope's centre, the round scope circle is gone, and the
game runs at its steady 30 fps without hitches. VC3 is unaffected.

## Left to the builder

- The rifle's exact shape, size, and where the lens sits relative to the
  hand.
- Lens size and the scope picture's resolution (the 256×256 target may be
  rendered at a higher scale if it looks soft on the headset).
- Filter parameters and zoom speed within the decisions above.
- Silent Scope's HUD rectangles, and the button map details.
- The Quest app's identity (package, label, folder name).
- Whether Silent Scope 2 and 3 need different scope or zoom hooks (they
  will need their own addresses; Silent Scope 3's anti-aliasing resolve
  should be fixed before its VR work).

## Sources

- Silent Scope 1's code: `games/ss1/analysis/disasm/asm/text.asm`, lifted
  `games/ss1/generated/`. Scope target created in `sub_00011A10`; scope pass
  in `sub_00059DC0` → `sub_000744B0`; scope camera `sub_00059F80`; focal
  length `sub_00074610`; circle `sub_00075F40` (from `sub_00076490`,
  `sub_00076230`); shots `sub_00058160` → `sub_00057D30` → `sub_0003EB30`;
  aim update `sub_000588C0`; main camera `sub_0005A130`.
- VC3's VR: `runtime/src/xr/` (host, panel, gun overlay),
  `runtime/src/gpu/vulkan/vk_immersive.c`, `vk_overlay.c`,
  `games/vc3/src/vr_aim.c`, `vr_cull.c`, `quest/`.
