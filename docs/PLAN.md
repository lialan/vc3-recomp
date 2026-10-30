# Virtua Cop 3 → macOS (static recompilation) — plan

Target: native arm64 macOS build of *Virtua Cop 3* (Sega Chihiro, 2003) using
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) (pinned submodule,
`external/xboxrecomp`, v0.16.0 / f0f8a46). No game files and no lifted code are
committed to this repo.

## What we have (verified 2026-10-29)

**Game files.** These come from the Batocera v43 image on the NAS
(`roms/chihiro/Virtua Cop 3.squashfs`, the "Xbox-compatible" repack that xemu +
Cerbios runs). They are unpacked to `~/Downloads/VR/VC3/extracted/`:

| File | What it is |
|---|---|
| `vc3.xbe` (2.1 MB) | The game. Debug-keyed XBE, built 2003-02-26, XDK 5028. Base 0x10000, entry **0x000B6407**, image 5.1 MB |
| `vc3_t.xbe` (40 KB) | "TestLaunch" stub that only chains to `d:\vc3.xbe`. Ignored |
| `media/` (185 files, 456 MB) | Assets: `mdl_*.bin`, `tex_*.bin`, `*.xts`, `movie_xpr.gz`, `rs_*.ecd` |
| `boot.id`, `firmware*.bin`, `marble_firm.bin` | Chihiro GD boot ID and the JVS/baseboard firmware blobs the game uploads |

The MAME set (`roms/vcop3/gdx-0003a.chd`) is the encrypted GD-ROM. It is not needed.

**XBE shape.** It has 13 sections (`.text` is 0xF5410 bytes). D3D8, D3DX8, XGRAPHC,
DSOUND and the PSGSFD/XPP/DOLBY libraries are linked statically, and so is LIBCMT.
All **110 kernel imports are standard retail ordinals** (there is no Chihiro-only
kernel API), and every one is in xboxrecomp's 371-ordinal bridge.

**Chihiro hardware access is in game code.** It is the SEGA library linked into
`.text`:

* **JVS / baseboard:** `JVS_SendCommand`, `JvsNodeSendPacket/ReceivePacket` (I/O
  board: gun X/Y, trigger, coins, start, test/service), `JvsEEPROM_*`,
  `JvsBACKUP_*` (backup RAM: settings, bookkeeping, rankings), `JvsRTC_Read`,
  `Jvs[Sc]Firmware{Up,Down}load`, `JvsSc*Midi/Rs323c`.
  Error paths: "Valid baseboard not found", "JVS I/O board is not connected".
* **Media board:** `mbcom:` and `IdexChannelObject` (direct access to the IDE
  channel).
* **Storage:** `\Device\Harddisk0\partition1\{TDATA,UDATA}`, `\Device\CdRom0` (= D:).

**Toolkit fit.** xboxrecomp runs on Apple Silicon (BLiNX 2 and Burnout 3 have run
on a Mac), with an OpenGL 3.3 + SDL2 backend there. It supports a 128 MB devkit RAM
layout (Chihiro has 128 MB) and comes with a template game project. Nothing in it
knows about Chihiro. That is the new work.

## Approach

Lift `vc3.xbe` to C with the stock pipeline. Link it against xboxrecomp's runtime
(kernel, D3D8→GL, DSOUND/APU, input). Then add a small **Chihiro HLE layer** that
replaces the SEGA hardware functions at their guest addresses through
`recomp_lookup_manual` / manual overrides. The game then never touches a
baseboard, a media board or an IDE channel. This is how Cxbx-Reloaded handles
Chihiro, by patching JVS at the function level, and it fits recomp better than
emulating the USB/IDE transport.

```
vc3.xbe ──parse/disasm/func_id/abi──► analysis/out/  (gitignored)
        └─recomp --all --split 250──► src/recomp/gen/ (gitignored; private repo later)
src/main.c          boot: 128 MB layout, D:→game dir, partition1→save dir
src/chihiro/jvs.c   JVS I/O: mouse→gun, keys→coin/start/test/service
src/chihiro/backup.c EEPROM/backup RAM/RTC → files in save dir
src/chihiro/mb.c    media board + baseboard detect: stubs that report success
src/recomp_manual.c address → override table
```

## Phases and exit criteria

0. **Repo + assets** (done). The repo, the submodule, the ROM unpacked outside the
   repo, venv + SDL2/libepoxy installed.
1. **Static analysis.** Run `scripts/analyze.sh` for functions, xrefs, the
   classification and the ABI. Then find the address of every `Jvs*`, `JVS_*` and
   `mbcom` function from its string xrefs, along with the baseboard probe and the
   gun-to-screen calibration code. Write the results to `docs/chihiro-symbols.md`.
   *Exit:* every hardware entry point has an address and a recovered ABI. The
   function count and the unresolved indirect-call count are recorded.
2. **Lift.** Run `tools.recomp --all --split 250 --gen-dir src/recomp/gen`.
   *Exit:* the output compiles with Apple clang on arm64. Lifter gaps get fixed
   upstream-style in the submodule, on our fork's branch.
3. **Host project.** Adapt the template `CMakeLists.txt`/`main.c` for macOS
   (entry 0xB6407, 128 MB layout, path map: `D:`/`CdRom0` → extracted dir,
   `partition1\TDATA|UDATA` → `~/Library/Application Support/vc3-recomp`).
   Default to muted audio. *Exit:* the binary links and enters `xbe_entry_point`,
   then reaches the first crash, which gets logged.
4. **Chihiro HLE.** Write `Jvs*` replacements from the protocol (JVS packets:
   switch, coin and analog/gun-position reports). `EEPROM`/`BACKUP`/`RTC` persist to
   files. Firmware down/upload and media-board calls return success.
   *Exit:* the game gets past baseboard/JVS checks with no "not found" errors.
5. **Boot to attract.** Work through kernel, D3D→GL and NV2A gaps in order of
   which one crashes first: SEGA logo → attract → title. *Exit:* attract mode
   loops with correct visuals for 5 minutes.
6. **Playable.** Map the mouse to the gun (button = trigger, off-screen or right
   button = reload). Map keys for coin/start/test/service; the test menu covers gun
   calibration. Lock timing to 60 Hz. Audio stays muted by default and is verified
   with level meters. *Exit:* stage 1 can be played start to finish; the test menu
   and the settings saved in backup RAM persist between runs.
7. **Polish and packaging.** Crosshair, window scaling/fullscreen, config file, a
   `scripts/build-macos.sh` that produces a `.app` pointing at the user's game
   folder. *Exit:* a clean build from a fresh clone and a documented setup.

Later and out of scope here: a 2P (second gun) mapping, a Quest VR build like the
Lindbergh ports, and Ghost Squad / HOTD3 (also on Chihiro, in the same Batocera
folder), which should reuse `src/chihiro/`.

## Status (2026-10-30)

Playable end to end on Apple Silicon: boot, region warning, attract, coin,
START, Mission Select, mission briefing, combat (hits score, ammo and lives
count down), CONTINUE?, with the stage, enemies and HUD all drawn. Runs
at about 45 fps through the GL back end.

What it took, beyond the phase 0-3 setup:

* **Chihiro hardware** (`src/chihiro/`): backup RAM, EEPROM and RTC on host
  files; the baseboard firmware handshake; a software JVS I/O board answering
  the game's own packets (switch replies sized to the feature list); the
  Chihiro-hardware flag (`[0x36983C]`, set when memory > 64 MB) forced on, or
  the game reads Xbox gamepads and never polls JVS; DIP switches selecting
  640x480 progressive (no board means 1024x768, but the 2D is laid out for
  640x480). TestLaunch data only with `VC3_TEST_LAUNCH=1` (boots to test mode).
* **D3D/NV2A**: VGA AV pack, the push-buffer executor, vblank/KEVENT pacing,
  the fence wait (`src/d3d_fence.c`), relaunch on leaving test mode.
* **Rendering** (`src/gpu/gl_backend.c`): an OpenGL 4.1 back end on a
  windowless CGL context. The executor's CPU rasteriser held the game at
  ~7 fps (and its logic ticks per frame). The back end evaluates the register
  combiners in GLSL, samples render targets and the surface being drawn,
  reads no-alpha formats as alpha 1, and resolves drawn surfaces into guest
  memory at the flip. xboxrecomp (`vc3-chihiro`): POSIX raster worker pool,
  back-end flip before present, combiner state/inputs handed to back ends,
  fixed-function vertex blending.

Known issues:

* The full register-combiner path (`VC3_GL_COMBINERS=1`) still rejects the
  lightmapped walls in its alpha test; the default draws texture * diffuse,
  times the stage-1 lightmap where a batch uses one (VC3's walls).
* The attract background is white, identically on the GL backend and the
  software rasteriser, so it is not a renderer bug. The game opens all seven
  movies under `media/movie/` at boot (valid MPEG-PS, `00 00 01 BA`), but no
  `[READ]` ever returns movie data and none is asynchronous: the Sofdec player
  never starts streaming. No YUV textures are bound during attract, no thread
  entry is missing and no kernel call is unimplemented.
  Traced further: Sofdec init (`mwPlyInitSfdFx` 0xC80A0) succeeds, but
  play-movie (0x823C0: sets flags |= 6 at 0x373288, movie index at 0x37328C,
  then 0x82290 creates the player via `mwPlyCreate` 0xC8EE0) is never called,
  nor are any of its seven callers. The white is the movie surface itself:
  `media/movie_xpr.gz` (a 16-byte `VCGZ` header, then gzip of `tex.xpr`) is a
  640x480 LIN_X8R8G8B8 texture, every pixel white, that decoded frames would
  be converted into. The title-movie object is type 0x27 in the object-type
  table at 0x1533B8 (20-byte entries {id, init, update, destroy, extra}):
  init 0x14710 (plays movie 4, looping), update 0x147A0, destroy 0x14780.
  The lookup `0x159D0(id, which)` (sole caller 0x1D760) is never called
  during boot or attract (checked with an entry hook), so no object from this
  table is created there. The attract sequence may be stuck on a placeholder
  screen rather than cycling title movie / demo / ranking. Next: find what
  normally starts 0x1D760 from the attract scene.
  (`vc3_title2_100mh.sfd`, `demo_100mh.sfd` and `bonfire.sfd` are unused
  strings; those files do not ship.)
* Input is verified from SDL's event queue onward (`VC3_INPUT_SELFTEST=1`:
  coin, start, aim, fire, reload through the emulated JVS board). Play with a
  physical mouse is not yet confirmed by a person; `scripts/real-input.swift`
  (OS-level events) needs the terminal to have Accessibility permission.
  Reload uses the gun's off-screen switch (JVS button 2).
* Sound is emulated and measured (RECOMP_AUDIO_WAV) but muted unless
  `VC3_SOUND=1`.

Debug switches: `VC3_GL=0` (software rasteriser), `VC3_GL_STATS`,
`VC3_GL_FTRACE=<flag>`, `VC3_GL_RCDBG=1..7`, `VC3_JVS_TRACE`,
`RECOMP_FFP_TRACE=<flag>`, `RECOMP_FRAME_TRACE=<flag>`, `VC3_AUTOKEYS`,
`VC3_AUTOAIM`, `VC3_INPUT_SELFTEST`.

## Risks

* **The JVS transport is still unknown.** If the high-level `Jvs*` functions are
  inlined or too entangled, we drop one layer and emulate the baseboard's packet
  interface instead.
* **The XBE is a community "Xbox-compatible" repack.** It may already carry
  patches for xemu/Cerbios. We need to compare its behaviour against the
  expectations of the original Chihiro.
* **Lifter and GL backend gaps** are expected on a new title, mostly in NV2A
  combiners and vertex shaders. The PSGSFD/XPP sections may be data or code that
  needs seeding.
* **FMV:** `movie_xpr.gz` looks like XPR texture sequences, not WMV, so the missing
  macOS video decoder should not matter. To confirm in phase 5.
