# The House of the Dead III (Sega Chihiro) on the Mac — plan

**Goal:** the arcade *House of the Dead III* statically recompiled on the shared
vc3-recomp runtime, playable on the Mac: attract, coin, start, the chapters with
a mouse gun, sound and movies, at the arcade's 60 fps at native size. VR and the
Quest come after, as a separate plan (HOTD4 VR's menu, settings and controls).

**Approach:** HOTD3 is the same kind of program as Virtua Cop 3: a debug-keyed
Chihiro XBE that drives the arcade hardware through Sega's library linked into
its own code (JVS I/O board, baseboard, backup RAM, EEPROM, RTC, media board).
That layer, the kernel bridge, the NV2A push-buffer executor, the Vulkan
renderer and the test tools already exist for VC3. HOTD3 gets a
`games/hotd3/` folder holding only its addresses and its own quirks; anything
shareable goes into the runtime.

## What we have (checked 2026-11-06)

**Game files.** The NAS has only MAME's arcade set: `roms/hotd3/gdx-0001.chd`
(the encrypted GD-ROM; its SHA-1 matches MAME's) and `hotd3.zip` (80 bytes of
security-chip data, `317-0348-com.data`). There's no decrypted repack like
VC3's and no Xbox retail copy. The image is decrypted the way MAME's DIMM board
does it:

1. `chdman extractcd` (Homebrew `rom-tools`) unpacks the tracks; track 3
   starts at sector 45000.
2. The chip data names `BFN.BIN`, which points to `HOD3AC_S.BIN` (374 MB), and
   gives its DES key.
3. Decrypting that file 8 bytes at a time with MAME's DES code
   (`naomigd.cpp`) gives a FATX volume. Its FAT is sized for a 512 MB volume,
   so the data starts at 0x11000.

The result is in `~/Downloads/VR/HOTD3/extracted/`: 36 files, 356 MB.

| File | What it is |
|---|---|
| `hod3xb.xbe` (2.1 MB) | The game. Debug-keyed, built 2002-10-29, XDK 4831. Base 0x10000, entry 0x000B8BAC, SHA-256 `89a0e777…71de` |
| `acdemoT.xbe` (40 KB) | The TestLaunch stub, like VC3's `vc3_t.xbe`. Ignored |
| `FS/*.AFS` (19 files) | CRI archives: models, motions, sprites, sound effects, music, voices |
| `MVI/*.SFD` (14 files) | CRI Sofdec movies: title and chapter intros, US and JP |
| `boot.id` | GD boot ID |

**XBE shape.** Same 13 sections as VC3 (D3D, DSOUND, PSGSFD, D3DX, XGRPH,
XPP, DOLBY), so the same libraries are linked in, but from the older XDK 4831
(VC3 is 5028). 108 kernel imports: VC3's set minus five, plus
`KeInitializeSemaphore`, `KeReleaseSemaphore` and `NtClearEvent`. All three are
already in the toolkit's bridge. Its strings show the same Sega hardware layer
as VC3: JVS, `mbcom:`, backup data, `JammaFirmwareLoad`, two guns with GUN
ADJUSTMENT, and TDATA/UDATA storage.

**Known differences from VC3:**
- The game reads `d:\firmware.bin` and `d:\firmware2.bin` (baseboard
  firmware), which this disc image lacks; VC3's repack had them. The runtime
  already stubs the upload (`baseboard.c`); the file reads need stubbing too,
  or harmless stand-ins.
- XDK 4831's D3D differs from 5028's, so the D3D fence wait, device pointer,
  kickoff and `XLaunchNewImage` have to be found again, not copied.
- The I/O check ("does not fullfill the game spec") may want a different JVS
  board identity or capability list than VC3's.

## Rules

- **One runtime.** HOTD3's code lives only in `games/hotd3/src/` (profile,
  address replacements, game hooks). Where it differs, add a profile field or a
  weak hook whose default is today's behaviour. No copied runtime files.
- **No regressions.** After each runtime or toolkit change: build `vc3`, `ss1`,
  `ss2`, `ss3` and `ssc` (Mac Vulkan); boot VC3 to attract at 60 fps; run ss1's
  rifle test; run `scripts/vr-math-test.sh`, `scripts/vr-rifle-test.sh` and
  `tests/`. All five Quest apps must still build.
- **Mac on the Vulkan back end**, the renderer the headsets use, so GPU work
  carries over.
- Re-lift with the current lifter before every build and test. No game data,
  decrypted images or lifted code in git.
- **Git:** commit straight to `main` at each milestone, push; toolkit changes on
  the toolkit's `vc3` branch, pushed first. The Quest isn't touched.

## Phase 0: reproducible game files

- Add the two tools I used to the repo, `tools/chihiro_gd/gd_decrypt.cpp` (MAME's
  DES, BSD-3, credited) and `tools/chihiro_gd/fatx_extract.py`, plus
  `scripts/hotd3-files.sh`, which goes from the CHD and chip data to the
  `extracted/` folder and checks the XBE's SHA-256.
- *Done when:* a clean run of the script reproduces the same `extracted/` tree.

## Phase 1: analyse and lift

1. Create `games/hotd3/`, modelled on `games/vc3/`:
   - `game.conf`: `XBE=hod3xb.xbe`, its SHA-256, `ENV_PREFIX=HOTD3`,
     `GAME_DIR_DEFAULT=~/Downloads/VR/HOTD3/extracted`, `LIFT_FLAGS=--reg-cache`;
   - `src/game.c`: a Chihiro profile, starting from VC3's;
   - `src/recomp_manual.c` and an empty `seed_functions.json`.
2. Run `scripts/analyze.sh hotd3` and `scripts/lift.sh hotd3`.
3. Find HOTD3's addresses for the profile:
   - **Sega library** (with `scripts/find_chihiro_syms.py` and by matching
     VC3's functions): backup read and write, EEPROM, RTC, JVS send, receive and
     link-up, `JammaFirmwareLoad`, `JvsScFirmwareDownload`, DIP switches, the
     dev-unit probe;
   - **D3D:** BlockOnTime, the device pointer, kickoff;
   - **`XLaunchNewImage`**;
   - the firmware file reads.
- *Done when:* HOTD3's generated code compiles and links with the profile's
  replacements.

## Phase 2: boot to the attract mode

Build `-DCHIHIRO_GAMES=hotd3` in `build-vk` and run it on the extracted folder.
Each step gets committed:
1. Reach the entry point and the first kernel calls. Fix any kernel gaps in the
   toolkit.
2. Add indirect-call targets the lift missed to `seed_functions.json` (VC3
   needed 45), then re-lift.
3. Get through the boot checks: firmware upload, JVS I/O check, backup RAM,
   region.
4. First frame, then title, then attract with the title movie (Sofdec).
- *Check:* frame dumps of the title and attract screens, viewed.

## Phase 3: play chapter 1

1. **Inputs:** coin and start from the keyboard, and the mouse as player 1's
   gun: aim, trigger, reload by shooting off-screen. That needs HOTD3's JVS
   layout (players, switch bytes, analog channels) and its GUN ADJUSTMENT
   calibration (backup offset and defaults) in the profile, as VC3 has.
2. Start a game, play the chapter 1 intro movie, then the chapter itself:
   zombies hit, score and lives count, continue.
3. **Sound:** DirectSound voices, music from `BGM.AFS`, sound effects. As in
   every toolkit title, the DSP effects will be missing.
4. Re-check VC3's fixes for this game: the idle-loop yield, the sound-queue
   lock, the CRI stream library's thread-suspend halt (VC3's `vc3_cri_trap.c`),
   the D3D fence, visibility queries. Move any that HOTD3 also needs from
   `games/vc3/` into the runtime instead of copying them.
- *Check:* a scripted run (`RECOMP_PAD_SCRIPT` / VC3's AUTOKEYS and fake gun
  with scripted aim) that coins up, starts and plays through chapter 1's first
  area, with frame dumps viewed.

## Phase 4: runs well on the Mac

- **Targets:** a steady 60 fps through chapter 1 at native 640×480; no stall
  over 100 ms after warm-up; a clean log (no unimplemented calls, no Vulkan
  errors).
- **How:** measure with VC3's tools: the per-10-second fps lines, GPU timestamps
  (`CHIHIRO_VULKAN_*`), frame pacing, and CPU time per thread. Fix slowness in
  the shared runtime where possible.
- **Done when:** a scripted 3-minute chapter 1 run in two back-to-back builds,
  with the numbers in the commit message. Commit "hotd3 plays on the Mac" and
  push.

## Later (separate plans)

- 16:9 and 2-player on the Mac.
- VR on the Quest 3 and the Steam Frame, following HOTD4 VR's menu, settings
  and controls one to one, on the shared immersive renderer and overlay.

## Risks

- **XDK 4831:** library functions not seen before, possibly with kernel or D3D
  gaps that VC3 never hit. Fix them in the toolkit.
- **The I/O and firmware checks:** the game may refuse to start until the JVS
  identity, capabilities or firmware replies match what it wants. MAME's
  `chihiro.cpp` and VC3's notes are the references.
- **Sofdec movies** are decoded by recompiled game code on the CPU. They may be
  slow, which would need a look like VC3's sound work.
