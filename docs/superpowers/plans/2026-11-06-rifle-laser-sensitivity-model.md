# Silent Scope VR: rifle laser, scope sensitivity, detailed model

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Three changes the user asked for after playing Silent Scope VR on the Quest (they liked the scope's size, sharpness and smoothness, which must not change): a laser straight out of the rifle's barrel showing where the shot goes; a way to lower aim sensitivity while aiming through the scope; a detailed, free (CC0/CC-BY) sniper rifle model in place of the box rifle.

**Builds on:** `docs/superpowers/plans/2026-11-05-silent-scope-quest-vr.md` (Tasks 1-8c, all landed; branch `ssc` at `93a6092`) and its spec `docs/superpowers/specs/2026-11-05-silent-scope-quest-vr-design.md`.

**The user opted out of design questions; these are the controller's calls (state them in the final report):**
- **Laser:** a red beam from the rifle's muzzle along the barrel, on by default; a "Laser" row (On / Off) comes back on the rifle's settings panel. No dot (there is no world hit point to put it on). Off while the panel is open (the menu's own pointer beam is used there, as now).
- **Barrel = aim:** the rifle model is drawn along the final aim direction (steadied, then sensitivity-scaled), rotated about the controller's grip point, so the barrel, the laser, the lens crop and the shot always agree. On fast swings the model may trail the hand by the steadying's lag (≤3°, Task 2's test) plus the sensitivity leash.
- **Scope sensitivity:** a "Scope sensitivity" row: 100% / 75% / 50% / 35%, default 50%, saved per game. Active only while the player is looking into the lens (head close to the lens and facing it), the scope picture is up and the panel is closed. While active, each frame's hand rotation moves the aim by that fraction, and the aim never trails the hand by more than a leash (about 4°); outside, the offset decays to zero in about a quarter second.
- **Model:** keep the lens exactly where and as big as it is now (`rifle_lens(qo_scope_scale())`), and the scope proportions the user liked; fit the detailed model's scope to that lens. Box rifle stays as the fallback if the model is missing.

## Global Constraints

All of the previous plan's Global Constraints still hold (one shared runtime; VC3 must not get worse: Mac attract 60 fps same picture, `CHIHIRO_RENDERER=vulkan quest/build.sh vc3` links; Quest only after asking the user, never while the app runs; re-lift ss1 after `games/ss1/src/recomp_manual.c` changes; push over SSH, branch `ssc`; KosmicKrisp for Mac runs). In addition:

- The lens picture, its crop (vr_lens_crop), its size, the zoom and the steadying are unchanged from `93a6092` unless a task says otherwise. The user signed off on them.
- VC3's handgun path (weapon `QO_WEAPON_HANDGUN`) is untouched: no behaviour change.
- Git: call git as `/usr/bin/git` (the rtk hook's `git` rewrite is refused by the worktree guard).
- Third-party assets: only CC0 or CC-BY, with author, source URL, licence and attribution recorded in `third_party/README.md` next to the files.

## Review Focus

1. **Barrel, laser and shot agree:** with sensitivity 35% and a slow sweep, the laser and the lens crosshair stay on the same point the game shoots at. Test: Task 10 `sens_*` unit tests plus the Quest dump in Task 12.
2. **Sensitivity never strands the aim:** a big fast turn still brings the aim along (leash), and looking away from the lens brings the rifle back onto the hand within ~0.25 s. Test: Task 10 `sens_leash_caps_offset`, `sens_disengage_returns_to_hand`.
3. **Settings panel open:** no laser, no sensitivity offset, pointer beam as before.
4. **The model never covers the lens** and the lens stays the size and place the user approved. Test: Task 11 geometry tests.
5. **Overlay cost on the Quest:** the detailed model must not cost the game frames. Test: Task 12 GPU/CPU numbers against `93a6092`.

---

### Task 10: Laser and scope sensitivity

**Files:**
- Modify: `runtime/src/host/vr_rifle.h`, `runtime/src/host/vr_rifle.c` (pure maths)
- Test: `tests/vr_rifle_test.c` (add to `scripts/vr-rifle-test.sh` if a new file)
- Modify: `runtime/src/xr/quest_host.c` (rifle_frame, the rifle's panel rows and settings, the beam for the rifle)
- Possibly: `runtime/src/xr/quest_overlay.c` / `runtime/src/gpu/vulkan/vk_overlay.c` (only if the beam needs a rifle-specific length/look)

**Interfaces (produce):**
```c
/* Scope sensitivity: the aim follows the hand's rotation scaled by gain while
 * engaged, never more than leash radians from the hand; disengaged, the offset
 * decays to zero (time constant ~0.08 s, gone in ~0.25 s). Unit vectors in;
 * first call, bad dt or non-finite input: the output is the hand (or the held aim). */
typedef struct { float aim[3], last[3]; int n; } VrSens;
void vr_sens_step(VrSens *s, const float hand[3], int engaged, float gain, float leash, float dt, float out[3]);
/* Is the head looking into the lens: within max_dist of its centre and facing it within max_angle (radians). */
int vr_eye_on_lens(const float head_p[3], const float head_fwd[3], const float lens_c[3], float max_angle, float max_dist);
```

- [ ] **Step 1: Tests first** in `tests/vr_rifle_test.c`: `sens_gain_one_is_identity`, `sens_halves_small_moves` (gain 0.5, a 1° hand move gives 0.5°), `sens_leash_caps_offset` (gain 0.35, 20° hand sweep: angle(aim,hand) ≤ leash + 1e-3), `sens_disengage_returns_to_hand` (offset < 0.05° after 0.3 s), `sens_ignores_bad_input` (dt ≤ 0 / NaN, non-finite hand), `eye_on_lens_cases` (in front and facing: 1; behind or turned away or too far: 0). Run `scripts/vr-rifle-test.sh`: they fail.
- [ ] **Step 2: Implement** in `vr_rifle.c`. Rotations via axis-angle between successive unit vectors (no Euler); leash by slerping the aim toward the hand. Run the tests: pass.
- [ ] **Step 3: Host.** In `rifle_frame`: hand direction → steadying (as now) → `vr_sens_step(engaged = scope picture up && panel closed && vr_eye_on_lens(head, lens centre in LOCAL, ~25°, ~0.45 m), gain = the row's value, leash ≈ 4°)` → the final aim, used for the game's yaw/pitch and the lens crop (as now, `s_rifle_yaw/pitch`). The lens centre in LOCAL comes from the rifle pose and `rifle_lens(qo_scope_scale())` (the same function the overlay draws with). Then rotate the overlay's rifle (`s_qo.gun_q`) by the rotation taking the hand's bore direction to the final aim (in LOCAL: undo the anchor yaw), about the controller position (`s_qo.gun_p`), so the drawn barrel is the aim.
- [ ] **Step 4: Laser.** For the rifle, `s_qo.beam` = laser on and panel closed; `s_qo.end` = the muzzle plus the final aim direction times a length that reads well in the headset (start at 30 m; a thin beam fades to sub-pixel by itself); `s_qo.dot = 0`. Keep the panel's pointer beam behaviour when the panel is open.
- [ ] **Step 5: Panel rows.** For the rifle: show "Laser" with On / Off (stored in the existing `vr_laser=` key: `off` is Off, anything else On; default On for the rifle), and a new "Scope sensitivity" row 100% / 75% / 50% / 35% (`vr_scope_sens=`, default 50%). The menu must still fit (check `MENU_ROWS` and the panel height); VC3's rows unchanged. Update the rifle's Controls page text if it describes aiming.
- [ ] **Step 6: Verify on the Mac:** `scripts/vr-rifle-test.sh` all passed; `ninja -C build-vk ss1 vc3`; `CHIHIRO_RENDERER=vulkan quest/build.sh ss1` and `... vc3` both build. The overlay only renders on the headset: Task 12 checks it.
- [ ] **Step 7: Commit** `xr: rifle laser from the barrel; scope sensitivity while looking into the lens`.

### Task 11: A detailed rifle model

**Inputs:** the model the research step picked (a GLB under `/Users/lialan/.claude/jobs/ddaa9f7a/tmp/rifle/chosen/`, with its licence facts), given in the task brief.

**Files:**
- Create: `third_party/rifle/` (the source model as received or its GLB, plus licence text) and `third_party/README.md` (attribution)
- Create: `scripts/rifle-model.py` (a one-off converter, run by hand in a venv; its output is committed)
- Create: the converted asset (binary mesh + texture(s), e.g. `runtime/src/xr/assets/rifle_model.bin`) and a generated header with its constants (scope eyepiece, muzzle, bounds)
- Modify: `runtime/src/gpu/vulkan/vk_overlay.c` (draw the textured model), `runtime/src/xr/quest_rifle_geometry.inc` or a new `.inc` (muzzle/scope constants from the model), the runtime CMake/Android build (embed the asset, e.g. with an assembler `.incbin`, so no runtime file access is needed)
- Test: `tests/vr_rifle_test.c` (geometry checks on the converted model)

- [ ] **Step 1: Convert.** In gun space (metres; origin the controller's grip point; barrel along −z; +y up), with the rifle's grip where the right hand holds it. The scope part is scaled about its own mount so its eyepiece is exactly the current lens (`rifle_lens(qo_scope_scale())` centre and radius at the default scale); the rest of the rifle at real size, its bore on the current bore line (`RIFLE_SCOPE_Y` below the scope's axis). If the scope is not a separate mesh in the file, split it by region. Decimate to ≤ 30k triangles if needed; textures ≤ 1024 px (one atlas or ≤ 4 materials); output indexed vertices (position, normal, uv) with indices that fit the overlay's draw path.
- [ ] **Step 2: Tests:** the model's eyepiece matches `rifle_lens()` (centre and radius within 1 mm); no triangle lies between the eye side of the lens and the lens disc within its radius (the lens is unobstructed); the muzzle constant is the model's barrel tip; the grip is within a few cm of the origin. Run: fail before the asset exists, pass after.
- [ ] **Step 3: Draw it** in `vk_overlay.c` for `QO_WEAPON_RIFLE`: textured (vertex colour = the overlay's existing CPU lighting × texture; linear filtering with mips so it doesn't shimmer), depth-tested and opaque, then the lens as now (unchanged), the laser and flash as now. Scope scale changes (`qo_scope_scale()`) rescale the scope part about its mount as the box scope does. The box rifle remains the fallback when the asset is absent.
- [ ] **Step 4: Verify on the Mac:** tests pass; `ninja -C build-vk ss1 vc3`; both Quest APKs build; note the APK size growth.
- [ ] **Step 5: Commit** `xr: a detailed sniper rifle model (licence in third_party/README.md)`.

### Task 12: Headset check (ask the user first)

- [ ] Ask the user whether the Quest is free. Then: `adb shell pidof org.sscrecomp.ss1` empty; install `quest/SilentScopeVR.apk`; run the XR overlay test dump (as in the previous plan's Task 8) to get pictures of the rifle, lens, laser; one profiled 90 s immersive run for GPU/CPU against the `93a6092` numbers (stage 1: 30 fps, GPU 72-81%). Stop the app and remove test files afterwards.
- [ ] Hand over for the user's play test: laser, sensitivity (try 35% / 50% / 100%), the model.
