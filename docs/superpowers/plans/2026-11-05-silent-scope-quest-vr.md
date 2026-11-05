# Silent Scope VR on Quest 3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Silent Scope 1 playable on Quest 3 in immersive VR with a one-handed sniper rifle whose scope lens shows the game's own magnified scope picture, thumbstick zoom, a steadied aim, and VC3's settings panel.

**Architecture:** The shared runtime gets three generic seams: profile hooks in place of VC3's hard-wired `vc3_vr_*` externs, a "lens picture" the renderer publishes next to the eyes and HUD, and a rifle weapon in the overlay. Everything Silent Scope-specific (addresses, the aim/zoom/circle hooks) lives in `games/ss1/src/`. Pure maths (aim angles, steadying, zoom) is unit-tested in C; the game hooks and the immersive path are checked headless on the Mac; the overlay and play are checked on the Quest, only with the user's go-ahead.

**Tech Stack:** C11, Vulkan (shared `runtime/src/gpu/vulkan`), OpenXR (`runtime/src/xr`), Android NDK/Gradle (`quest/`), the xboxrecomp toolkit (`external/xboxrecomp`, branch `ssc`).

**Spec:** `docs/superpowers/specs/2026-11-05-silent-scope-quest-vr-design.md`

## Global Constraints

- One shared runtime with VC3; Silent Scope addresses and constants only in `games/ss1/src/` or its profile (`games/ss1/src/game.c`).
- VC3 must not get worse: after any runtime change, VC3's Mac attract run still reaches its attract at 60 fps with the same picture, and `quest/build.sh vc3` (with `CHIHIRO_RENDERER=vulkan`) still links.
- The Quest 3 is used only after asking the user at that step, and never while the app may be running (`adb shell pidof <PKG>` empty first).
- Re-lift with `scripts/lift.sh ss1` before every tested build of ss1 (and after any change to `games/ss1/src/recomp_manual.c`).
- Toolkit commits are pushed before the parent's; push over SSH (`git@github.com:lialan/...`), branch `ssc` for both.
- Zoom range 0.5× to 4× of the game's value, starting at 1×, kept across stages, 1× on a game reset.
- The world follows the game camera fully (no levelling); aim angles are relative to the game camera's axes.
- Settings panel is VC3's minus Sight, Laser, Coins, Insert a coin, Arcade Test, Arcade Service and Screen; Silent Scope is always immersive (the automatic virtual-screen fallback for frames with no 3D stays).
- Mac runs use KosmicKrisp: `VK_ICD_FILENAMES=/opt/homebrew/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json`, build tree `build-vk` (`-DCHIHIRO_RENDERER=vulkan -DCHIHIRO_GLSLC=<NDK>/shader-tools/darwin-x86_64/glslc`).

## Review Focus

1. **Menus and the title screen with the rifle:** the game is navigated with the pad, so outside gameplay (no scope pass) the thumbstick must move the menu cursor (d-pad) instead of zooming, and A/B/Start must work. Test: Task 6 `stick_routes_to_dpad_without_scope`.
2. **The game skips its scope pass** (cut-scenes, reload, scripted moments): the lens goes dark that frame and comes back when the pass resumes, never showing a stale picture. Test: Task 4 step "lens freshness".
3. **Pointing far off** (behind, straight up or down): aim angles stay finite and continuous, no NaN reaches the game. Test: Task 2 `aim_angles_poles_and_behind`.
4. **Settings panel open:** the trigger chooses rows and fires nothing, the stick doesn't zoom, the game keeps its last aim. Test: Task 6 `panel_open_freezes_rifle`.
5. **Left hand:** "Gun hand: Left" puts the rifle, trigger and zoom stick on the left controller. Test: Task 6 `left_hand_uses_left_stick`.

---

### Task 1: Generic VR seams in the profile (VC3 unchanged)

Replace the host's hard-wired VC3 externs and constants with profile fields, so a game without them links and a game with a rifle can say so.

**Files:**
- Modify: `runtime/src/chihiro/chihiro_profile.h` (the `vr` block)
- Modify: `runtime/src/xr/quest_host.c:1540-1600` (`HUD_K_MAX`, `vc3_vr_aim_set`, `vc3_vr_cull_half_angle`), `:2399` (aim call)
- Modify: `runtime/src/gpu/vulkan/vk_immersive.c:15,265` (`IMM_ZOOM_BASE`)
- Modify: `runtime/src/xr/quest_vulkan.inc:759-770` (VC3's HUD split rectangles)
- Modify: `games/vc3/src/game.c` (fill the new fields with today's values)
- Create: `runtime/src/host/vr_rifle.h` (the `ChVrRifle` type only; Task 2 adds functions)

**Interfaces:**
- Produces, in `chihiro_profile.h` `vr` block:
  - `int weapon;` with `enum { CH_VR_HANDGUN = 0, CH_VR_RIFLE = 1 };`
  - `float lens_k;` the game's main-view focal length in pixels at 640 wide (VC3 `772.5f`; 0 = 772.5).
  - `struct { float x0, y0, x1, y1; } hud_tiles[8]; int hud_tile_count;` (VC3: today's rectangles from `quest_vulkan.inc:759-770`).
  - `void (*aim_screen)(int on, int x, int y);` (VC3: `vc3_vr_aim_set`)
  - `void (*cull)(float half_angle_rad);` (VC3: a setter for `vc3_vr_cull_half_angle`)
  - `void (*rifle)(const ChVrRifle *r);` (VC3: NULL)
- Produces, in `runtime/src/host/vr_rifle.h`:
  `typedef struct { int on; float yaw, pitch; /* radians in the game camera's view space: yaw + right, pitch + up */ float zoom; /* 0.5..4 */ } ChVrRifle;`

- [ ] **Step 1:** Add the fields and type above; in `quest_host.c` call `p->vr.aim_screen` / `p->vr.cull` when non-NULL instead of the externs, take `HUD_K_MAX` from `lens_k`, and in `vk_immersive.c` replace `IMM_ZOOM_BASE` with the profile's `lens_k` (0 → 772.5). The HUD split in `quest_vulkan.inc` reads `hud_tiles` (count 0: the HUD as one quad).
- [ ] **Step 2:** Fill VC3's profile with `lens_k = 772.5f`, its current HUD rectangles, `aim_screen = vc3_vr_aim_set`, `cull = vc3_vr_cull_set` (new two-line setter in `games/vc3/src/vr_cull.c`).
- [ ] **Step 3: Verify VC3 on the Mac.** Run: `sh $CLAUDE_JOB_DIR/tmp/vc3run.sh` (or the equivalent headless attract run). Expected: attract reached, `[GPU] 60.0x fps`, frame 1400 identical in content to before.
- [ ] **Step 4: Verify both Quest builds link.** Run: `CHIHIRO_RENDERER=vulkan quest/build.sh vc3` → APK built. (ss1's Quest build is Task 7.)
- [ ] **Step 5: Commit** `runtime: VR seams as profile fields (aim, cull, rifle, lens focal, HUD tiles)`.

### Task 2: Rifle maths (aim angles, steadying, zoom) with unit tests

Pure functions the host uses; no Vulkan, no OpenXR.

**Files:**
- Modify: `runtime/src/host/vr_rifle.h`
- Create: `runtime/src/host/vr_rifle.c` (added to `chihiro_rt` sources in `runtime/CMakeLists.txt`)
- Create: `tests/vr_rifle_test.c`, `scripts/vr-rifle-test.sh` (built like `scripts/vr-math-test.sh`: `cc -O1 -I runtime/src/host tests/vr_rifle_test.c runtime/src/host/vr_rifle.c -lm`)

**Interfaces:**
- Produces:
  - `void vr_aim_angles(const float dir[3], float *yaw, float *pitch);` dir in GL view space of the game camera (x right, y up, −z forward), any length; yaw = atan2(x, −z), pitch = atan2(y, hypot(x, z)); a zero vector gives 0, 0.
  - `typedef struct { float x[3], dx[3]; int init; } VrSteady;`
  - `void vr_steady(VrSteady *s, const float dir[3], float dt, float zoom, float out[3]);` a one-euro filter per component on the unit direction, renormalised: `min_cutoff = 1.2f / sqrtf(zoom)` Hz, `beta = 0.6f`, `d_cutoff = 1.0f` Hz; first call passes `dir` through.
  - `float vr_zoom_step(float zoom, float stick_y, float dt);` exponential: `zoom * powf(2.0f, stick_y * dt)` (full stick doubles per second), dead zone |stick_y| < 0.2 → unchanged, clamped to [0.5, 4].

- [ ] **Step 1: Write the failing tests** in `tests/vr_rifle_test.c`:
  - `aim_angles_basic`: dir (0,0,−1) → yaw 0, pitch 0; (1,0,−1) → yaw +π/4; (0,1,−1) → pitch +π/4 (±1e-5).
  - `aim_angles_poles_and_behind`: (0,1,0) → pitch +π/2, yaw finite; (0,−1,0) → pitch −π/2; (0,0,1) → |yaw| = π; (0,0,0) → 0,0; all outputs `isfinite`.
  - `steady_passes_first_and_still`: first call returns the input; 100 calls with the same dir at dt 1/72 return it within 1e-5.
  - `steady_smooths_tremor_more_at_zoom`: a 0.2° 8 Hz wobble around (0,0,−1) for 2 s at dt 1/72: output wobble amplitude < input's at zoom 1, and smaller at zoom 4 than at zoom 1.
  - `steady_follows_fast_swing`: a 90°/s yaw sweep for 0.5 s: lag behind the input < 3° at the end (zoom 1).
  - `zoom_step_range`: from 1, stick +1 for 1 s at dt 1/72 → 2.0 ±0.02; stick +1 for 10 s → 4.0; stick −1 for 10 s → 0.5; stick 0.1 → unchanged.
- [ ] **Step 2: Run** `scripts/vr-rifle-test.sh`. Expected: link error / FAIL (functions missing).
- [ ] **Step 3: Implement** the three functions in `vr_rifle.c` with the constants above.
- [ ] **Step 4: Run** `scripts/vr-rifle-test.sh`. Expected: `all passed`, exit 0.
- [ ] **Step 5: Commit** `runtime: rifle aim angles, one-euro steadying and zoom (tested)`.

### Task 3: Silent Scope's rifle hooks (game side), checked on the Mac

Aim, zoom, the circle and aim marks, scope-pass freshness, the lens surface, and VR culling, all in `games/ss1/src/`. A test rifle (`SS1_VR_RIFLE_TEST=yaw_deg,pitch_deg,zoom`) drives the same entry point without a headset.

**Files:**
- Create: `games/ss1/src/vr_rifle.c`
- Modify: `games/ss1/src/recomp_manual.c` (replacements for `sub_000744B0`, `sub_00075F40`, `sub_00076490`; the existing `sub_000588C0` / `sub_0006D650` wrappers call into vr_rifle when on)
- Modify: `games/ss1/src/wide.c` (`ss1_wide_camera` takes a VR half-angle when set)
- Modify: `games/ss1/src/game.c` (`vr.weapon = CH_VR_RIFLE`, `vr.rifle = ss1_vr_rifle`, `vr.cull = ss1_vr_cull`, `vr.lens_k`)
- Create: `runtime/src/host/vr_lens.c`, declarations in `runtime/src/host/vr_view.h`

**Interfaces:**
- Consumes: `ChVrRifle` (Task 1), profile hooks (Task 1).
- Produces:
  - `void ss1_vr_rifle(const ChVrRifle *r);` stores the latest rifle atomically (as `vc3_vr_aim_set` does).
  - `void ss1_vr_cull(float half_angle_rad);`
  - `void vc3_vr_lens_surface_set(uint32_t va);` / `uint32_t vc3_vr_lens_surface(void);` in `vr_lens.c`: the guest surface address the renderer publishes as the lens picture (0: none).
- Silent Scope 1 facts used (from the spec's Sources): aim pitch `0x1CEB75C`, yaw `0x1CEB75E` (int16, 65536 per turn); zoom multiplier `0x1ADFC8` (read only by `sub_00074610`); scope pass `sub_000744B0`; circle `sub_00075F40` (writes `0x1ADFC8` = 1.0 or 1/s for the type-3 scope); composite and aim marks `sub_00076490` (marks at `0x766E4` / `0x7670A`, models `0x1DB2482` / `0x1DB2480`); scope surface object `0x1ED214` (256×256); scope camera forward row at `0x1CEB210`.

- [ ] **Step 1: Measure the sign conventions.** Headless run into stage 1 holding the stick right (`$CLAUDE_JOB_DIR/tmp/ssrun.sh ss1 <dir> 1500` with `RECOMP_PAD_SCRIPT=...,16000:lright:4000`), logging `(int16)MEM16(0x1CEB75E)` and pitch once a second from the `sub_000588C0` wrapper (`SS1_AIM_LOG=1`). Record in `vr_rifle.c`'s header which sign is right and up.
- [ ] **Step 2: Read `sub_00076490` and `sub_00075F40`** (lifted C in `games/ss1/generated/`) and list every global they write besides draw submission; the replacements must keep those writes. Record the list in `vr_rifle.c`'s header comment.
- [ ] **Step 3: Implement `vr_rifle.c`:**
  - After `sub_000588C0_gen` (inside the existing wrapper, after the 16:9 step): when the rifle is on, write yaw/pitch as `lround(angle * 65536 / 2π)` with the measured signs, saturating to int16.
  - `sub_000744B0` replacement: run `_gen`, set `s_scope_drawn_frame = <frame counter 0x1CEAF90>`; on its first run publish the scope surface's colour address via `vc3_vr_lens_surface_set` (read from the D3D surface object at `MEM32(0x1ED214)`; confirm the value matches the 256×256 target address in the renderer's log).
  - `sub_00075F40` replacement: when the rifle is on, compute what the original writes to `0x1ADFC8` (from Step 2), multiply by the zoom, write it, draw nothing; otherwise `_gen`.
  - `sub_00076490` replacement: when the rifle is on and the scope pass ran this frame, perform only the non-draw writes from Step 2 and the `0x1ADFC8` write; otherwise `_gen` (so the marks stay when there is no scope picture).
  - `ss1_vr_cull`: stores the half-angle; `ss1_wide_camera` uses `W = 2·D·tan(half)` for the main view (W 640) when it is set, taking precedence over the 16:9 factor.
  - `SS1_VR_RIFLE_TEST=yaw_deg,pitch_deg,zoom`: at boot, call `ss1_vr_rifle` once with those values and `on = 1` (test only).
- [ ] **Step 4: Re-lift and build:** `scripts/lift.sh ss1 && ninja -C build-vk ss1 vc3`. Expected: both link; `sub_000744B0_gen`, `sub_00075F40_gen`, `sub_00076490_gen` exist in `games/ss1/generated`.
- [ ] **Step 5: Verify on the Mac.** Headless stage-1 runs with frame dumps (`CHIHIRO_VULKAN_DUMP_*`):
  - `SS1_VR_RIFLE_TEST=0,0,1`: no scope circle and no aim marks on the screen during play; the 256×256 target dump shows the scope picture.
  - `SS1_VR_RIFLE_TEST=20,10,1`: the shot direction (scope forward row at `0x1CEB210`, logged) is within 0.5° of yaw 20°, pitch 10° relative to the main camera matrix `0x1CEB084`; bullet marks appear there in the screen dump.
  - `SS1_VR_RIFLE_TEST=0,0,4`: the scope target shows 4× the magnification of the `0,0,1` dump (feature sizes ×4 ±10%).
  - Unset: the game is unchanged (circle and marks drawn, 30 fps).
- [ ] **Step 6: Commit** `ss1: rifle aim, zoom, no scope circle in VR, lens surface, VR culling`.

### Task 4: The lens picture and immersive Silent Scope on the Mac

The renderer publishes the lens surface with each immersive frame; Silent Scope's immersive view, HUD tiles and lens focal are checked with the desktop stand-in (`SS1_IMMERSIVE_TEST`, `runtime/src/host/vr_view_desktop.c`).

**Files:**
- Modify: `runtime/src/gpu/vulkan/vk_gpu_mailbox.h/.c` (`ChVkImmFrame`: `lens`, `lens_offset`, `lens_w`, `lens_h`; publish copies the lens target after the HUD)
- Modify: `runtime/src/gpu/vulkan/vk_immersive.c` / `vk_backend.c` (track "drawn into since the last flip" for the lens surface; hand its target to the publish)
- Modify: `games/ss1/src/game.c` (`lens_k`, `hud_tiles` measured here)

**Interfaces:**
- Consumes: `vc3_vr_lens_surface()` (Task 3).
- Produces: `ChVkImmFrame.lens` (1: a fresh lens picture follows at `lens_offset`, `lens_w × lens_h` RGBA8, straight alpha 1), 0 otherwise.

- [ ] **Step 1: Check the scope pass stays in its target in immersive mode.** Run ss1 headless with `SS1_IMMERSIVE_TEST=0,0,100` and `SS1_VR_RIFLE_TEST=0,0,1`, dumping eyes and the 256×256 target. Expected: the eyes show the world once (no scope-view geometry in them), the scope target shows the scope picture. If the classifier routes the scope pass into the eyes, restrict eye routing to screen-shaped surfaces before continuing.
- [ ] **Step 2: Lens freshness.** Implement the publish: `lens = 1` only when a draw targeted the lens surface since the previous flip. Test with a log line per second (`CHIHIRO_VULKAN_IMMERSIVE_LOG`): during play `lens fresh` on every frame; during the stage-intro cut-scene and the reload animation, `lens 0` (the game skips the pass); back to fresh afterwards.
- [ ] **Step 3: Measure Silent Scope's numbers.** Log the recovered projection (`gp[0]`, the main view's kx at 640) and set `lens_k` to it (expected ≈ 555, i.e. 240 / tan(23.4°)). From a HUD-layer dump during play, set `hud_tiles` to the boxes around each HUD group (score/time, life, ammo, others), in 640×480 pixels.
- [ ] **Step 4: Verify VC3** (Task 1 Step 3 run). Expected unchanged; `lens` stays 0.
- [ ] **Step 5: Commit** `runtime: publish a lens picture with immersive frames; ss1 lens focal and HUD tiles`.

### Task 5: The rifle model and its textured lens in the overlay

**Files:**
- Create: `runtime/src/xr/quest_rifle_geometry.inc` (boxes, as `quest_gun_geometry.inc`)
- Modify: `runtime/src/xr/quest_overlay.h` (`QoFrame`: `int weapon; void *lens_tex; int lens_fresh;`), `runtime/src/gpu/vulkan/vk_overlay.c/.h`
- Modify: `runtime/src/xr/quest_vulkan.inc` (upload the published lens into `s_vk_lens_tex` like `s_vk_hud_tex`; pass it in `QoFrame`)
- Test: `tests/vr_rifle_test.c` (geometry checks)

**Interfaces:**
- Produces in `quest_rifle_geometry.inc`: `RIFLE_LENS_C[3]` (rear lens centre, gun space), `RIFLE_LENS_R` (radius, ≈0.024 m), `RIFLE_MUZZLE_Z`; gun space as the handgun's (x right, y up, −z the barrel, origin the controller's aim pose).
- `ch_vk_overlay_read` draws the rifle when `f->weapon == CH_VR_RIFLE`; the lens is a 32-segment disc facing +z (towards the eye), textured with `lens_tex` (uv: the disc mapped onto the square picture's inscribed circle), or dark glass (rgb 0.03) when `!lens_fresh || !lens_tex`. Drawn with `ch_vk_target_draw_texture_read` as `vk_hud_draw` does.

- [ ] **Step 1: Write the failing geometry tests** (`rifle_lens_on_scope_line`: the lens centre lies on the scope tube's axis, above the bore by the tube's height, behind the receiver; `rifle_fits_hand`: overall length 0.9–1.15 m, grip box contains the origin within 3 cm).
- [ ] **Step 2: Run** `scripts/vr-rifle-test.sh`. Expected: FAIL (geometry missing).
- [ ] **Step 3: Implement** the geometry (stock, grip, receiver, barrel, scope tube with front and rear bells, rear lens) and the overlay drawing above; the handgun path is untouched when `weapon == CH_VR_HANDGUN`.
- [ ] **Step 4: Run** `scripts/vr-rifle-test.sh` (pass) and the VC3 Mac run (unchanged); build `CHIHIRO_RENDERER=vulkan quest/build.sh vc3` (links).
- [ ] **Step 5: Commit** `xr: a sniper rifle with a textured scope lens in the overlay`.

### Task 6: The host's rifle input, panel rows and controls page

**Files:**
- Modify: `runtime/src/xr/quest_host.c` (per-frame gun code `~2255-2410`, `menu_items`, `controls_items`, row list)
- Create: `runtime/src/host/retail_pad.c/.h` (VR buttons → the emulated Xbox pad through `xbox_InputKeySet`)
- Test: `tests/retail_pad_test.c` (built by `scripts/vr-rifle-test.sh` too)

**Interfaces:**
- Consumes: `vr_aim_angles`, `vr_steady`, `vr_zoom_step` (Task 2); `profile->vr.rifle`, `weapon` (Task 1); `QoFrame.weapon/lens_*` (Task 5).
- Produces:
  - `void retail_pad_map(int buttons, float stick_x, float stick_y, int scope_on, RetailPadKeys *out);` pure: which pad keys are down. `RetailPadKeys` holds `int start, a, b, x, y, rt, lt, up, down, left, right;`.
  - `void retail_pad_apply(const RetailPadKeys *k);` presses/releases through `xbox_InputKeySet` with the keyboard map's VK codes (Enter Start, Z A, X B, C X, V Y, 3 right trigger, 1 left trigger, arrows d-pad), only on changes.
  - `void retail_pad_stick(int gun_hand, const float sticks[2][2], float out[2]);` the gun hand's thumbstick (x, y): `sticks[0]` left controller, `sticks[1]` right; `gun_hand` 0 left, 1 right.
- Mapping (the rifle; VC3 keeps its own path when `platform` is Chihiro): trigger → `rt` (fire); gun-hand A/B (X/Y on the left) → pad A/B; either menu/stick-click → Start; the grip is unassigned (Silent Scope reloads by itself); other-hand trigger, grip and stick keep VC3's meanings (settings panel, gun hand swap, recentre). Gun-hand stick up/down → zoom while `scope_on`, else d-pad up/down (left/right always d-pad).

- [ ] **Step 1: Write the failing tests** in `tests/retail_pad_test.c`:
  - `trigger_fires`: trigger bit → `rt` only.
  - `stick_zooms_with_scope`: `scope_on = 1`, stick_y 0.9 → no d-pad keys.
  - `stick_routes_to_dpad_without_scope`: `scope_on = 0`, stick_y 0.9 → `up`; −0.9 → `down`; stick_x 0.9 → `right`; |v| < 0.5 → none.
  - `panel_open_freezes_rifle`: the panel-open bit → no keys at all.
  - `left_hand_uses_left_stick`: `retail_pad_stick(0, {{0.1,0.9},{0,-0.9}}, out)` → out = (0.1, 0.9); with `gun_hand` 1 → (0, −0.9).
- [ ] **Step 2: Run** `scripts/vr-rifle-test.sh`. Expected: FAIL.
- [ ] **Step 3: Implement** `retail_pad.c`, and in `quest_host.c` for `weapon == CH_VR_RIFLE`: the controller's aim direction into the game camera's view space (the same anchor/yaw transform the immersive aim uses, `lo`/`ld`), `vr_steady` with the current zoom, `vr_aim_angles`, zoom by `vr_zoom_step` while the scope is on, then `profile->vr.rifle(&r)` every frame (`on = 0` while the panel is open, `zoom` frozen); `zoom = 1` on "Reset game". Fill `QoFrame.weapon`, `lens_tex`, `lens_fresh`; no beam, no dot. `scope_on` = the last published frame's `lens`.
- [ ] **Step 4: Panel rows.** For `weapon == CH_VR_RIFLE` (or a retail platform) leave out Sight, Laser, Coins, Insert a coin, Arcade Test, Arcade Service and Screen; force immersive on. Controls page rows: "Trigger: Fire", "Thumbstick up/down: Zoom (menus: move)", "A / B: Confirm / Back", the other-hand rows as VC3; no Grip, foot pedal or CHANGE rows.
- [ ] **Step 5: Run** `scripts/vr-rifle-test.sh` (pass); VC3 Mac run (unchanged); `CHIHIRO_RENDERER=vulkan quest/build.sh vc3` (links).
- [ ] **Step 6: Commit** `xr: rifle input, steadying and zoom; retail pad from the controllers; rifle panel rows`.

### Task 7: The Silent Scope Quest app

**Files:**
- Create: `quest/ss1/` (copy of `quest/vc3/`: `build.gradle`, `AndroidManifest.xml`, `res/values/strings.xml`, icons, `java/org/sscrecomp/SilentScopeVR.java`, `Restart.java`) with `games/ss1/game.conf`'s `NAMESPACE`, `PKG`, `ACTIVITY`, `APP`, `HEADSET_DIR`.
- Modify: `quest/build.sh` (game.conf `RENDERER=vulkan` sets `CHIHIRO_RENDERER` when not given), `quest/install.sh` (the data folder from game.conf `DATA_DIR`: VC3 `media`, ss1 `data1`), `games/ss1/game.conf` (`RENDERER=vulkan`, `DATA_DIR=data1`, `XBE=ss1.xbe`)
- Java: `SilentScopeVR.java` checks `ss1.xbe` and the `data1` folder in `/sdcard/SilentScope`.

- [ ] **Step 1:** Create the module and the script changes above.
- [ ] **Step 2: Build:** `quest/build.sh ss1`. Expected: `quest/SilentScopeVR.apk`, `libmain.so` from `build-quest-ss1`.
- [ ] **Step 3: Build VC3 too:** `CHIHIRO_RENDERER=vulkan quest/build.sh vc3`. Expected: unchanged APK name, links.
- [ ] **Step 4: Commit** `quest: Silent Scope VR app (ss1)`.

### Task 8: On the headset (each step only after asking the user)

- [ ] **Step 1: Ask the user** for the go-ahead to use the Quest; check `adb shell pidof org.sscrecomp.ss1` and `org.vc3recomp.vr` are empty.
- [ ] **Step 2: Install:** `quest/install.sh ss1 ~/Downloads/VR/SSC/extracted`. Expected: APK installed, files in `/sdcard/SilentScope`.
- [ ] **Step 3: Overlay check:** run with `SS1_XR_FAKE_GUN=1 SS1_XR_DUMP=1` (the VC3 fake-gun dump path, now drawing the rifle) and `SS1_VR_RIFLE_TEST=0,0,1`; pull the dumps. Expected: the rifle in hand, the lens showing the scope picture, no circle on the world.
- [ ] **Step 4: User play test** of stage 1: rifle, lens, zoom, steadiness, shots at the scope centre, menus with the stick. Collect `ss1.log`; fix what they report in the owning task's files.
- [ ] **Step 5: Pipeline seeds:** after a user session, harvest per `runtime/src/gpu/vulkan/PIPELINE_PACKAGING.md` into `games/ss1/pipeline-cache/quest3/`, rebuild, reinstall (with permission). Expected: no first-time hitches on the second run.
- [ ] **Step 6: Commit** `ss1: Quest 3 pipeline seeds` and any fixes.

### Task 9: Docs and notes

- [ ] **Step 1:** README: a "Silent Scope VR" section (controls, zoom, how to install with `quest/install.sh ss1`). `docs/PLAN.md`: the new profile fields.
- [ ] **Step 2:** Final checks: `scripts/vr-rifle-test.sh` passes; VC3 Mac run unchanged; both Quest builds link.
- [ ] **Step 3: Commit and push** (toolkit first if it changed): `git push git@github.com:lialan/vc3-recomp.git HEAD:ssc`.
