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
