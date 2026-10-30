# Chihiro hardware entry points in vc3.xbe

Found by `scripts/find_chihiro_syms.py`, which uses string references plus the
disassembler's call graph. All addresses are guest VAs. The names come from the
library's own error strings.

## SEGA I/O library (`XPP` section, 0x135640–0x1401E9, 245 functions)

| VA | Size | Function (from strings) |
|---|---|---|
| 0x0013C430 | 797 | `JVS_SendCommand` |
| 0x0013CF20 | 77 | (called by both packet wrappers; node/lock helper) |
| 0x0013CF70 | 422 | firmware loader: reads `d:\firmware.bin`, `d:\firmware2.bin` |
| 0x0013D120 | 432 | `JammaFirmwareLoad` |
| 0x0013D310 | 514 | `JvsScFirmwareDownload` |
| 0x0013D520 | 435 | `JvsScFirmwareUpload` |
| 0x0013D6E0 | 104 | `JvsScSendRs323c` |
| 0x0013D750 | 109 | `JvsScReceiveRs323c` |
| 0x0013D7C0 | 226 | `JvsScSendMidi` |
| 0x0013D8B0 | 242 | `JvsScReceiveMidi` |
| 0x0013DA10 | 439 | `JvsEEPROM_Read` |
| 0x0013DBD0 | 440 | `JvsEEPROM_Write` |
| 0x0013DD90 | 329 | `JvsBACKUP_Read` |
| 0x0013DEE0 | 329 | `JvsBACKUP_Write` |
| 0x0013E030 | 216 | `JvsNodeReceivePacket` |
| 0x0013E110 | 204 | `JvsNodeSendPacket` |
| 0x0013E1E0 | 490 | `JvsFirmwareUpload` |
| 0x0013E3D0 | 514 | `JvsFirmwareDownload` |
| 0x0013E5E0 | 181 | `JvsRTC_Read` |
| 0x0013EBB0 | 408 | firmware start |
| 0x00136500–0x00136894 | — | 8 entry points called from 0x7C740/0x7C7C0/0x7C8B0 (not yet identified) |

XPP makes no kernel calls and does no port I/O. Its only calls out of the section
go to CRT helpers: Sleep, NtClose, file read/open, pool alloc/free, DbgPrint. The
board is reached through pointers set up elsewhere; the transport is still to be
found and is only needed for the fallback.

## Game-side driver wrappers (`.text`) — the planned HLE seam

| VA | Calls |
|---|---|
| 0x00104C70 | `JvsNodeSendPacket` (+0x13CF20) |
| 0x00104D80 | `JvsNodeReceivePacket` (+0x13CF20) |
| 0x00104F30 | `JvsScFirmwareDownload` |
| 0x00104F50 | `JvsEEPROM_Read`, `JvsBACKUP_Read` |
| 0x00104F80 | `JVS_SendCommand` |
| 0x00105020 | `JvsRTC_Read` |
| 0x00105110 | `JvsBACKUP_Read` |
| 0x00105150 | `JvsScSendRs323c`, `JvsScSendMidi` |
| 0x00105190 | `JvsScReceiveRs323c`, `JvsScReceiveMidi` |
| 0x000F4310 | `JammaFirmwareLoad`, 0x13ED50, 0x13ED70 |

## Game logic that consumes I/O

| VA | What |
|---|---|
| 0x0007D3A0 | baseboard probe ("Fatal error: Valid baseboard not found.") |
| 0x0007DB90 | gun type select (HANDGUN/SHOTGUN/MACHINEGUN) |
| 0x0001E780 | backup data validation |
| 0x000AF560 | test menu: coin bookkeeping |
| 0x000AFE00, 0x000AFFB0 | test menu: gun adjustment, raw GUN-X/GUN-Y |
| 0x000B0BE0 | test menu: backup data clear |
| 0x000F6DC0, 0x000F6F50 | media board (`mbcom:`) |
| 0x000B60CA, 0x000B6256, 0x000B7659 | CD-ROM / partition path setup |

## Lift (phase 2, 2026-10-29)

`tools.recomp --all --split 250` lifted all 6,742 functions with 0 failures:
1.17M lines of C in 27 files and 12 unresolved call targets stubbed. 63
instructions are unimplemented. Most are opcodes that come from data decoded as
code (`aam`, `aas`, `arpl`, `lldt`, `verr`, `bound`, `into`, `iretd`); the rest are
no-op-safe (`cli`, `sti`, `wbinvd`). Any of them that is actually reached will show
up at runtime.
