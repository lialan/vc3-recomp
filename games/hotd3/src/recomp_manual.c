/**
 * The House of the Dead III: the guest functions it replaces, and
 * recomp_lookup_manual().
 *
 * Every replacement here is the shared runtime's, the same one VC3 uses
 * (runtime/src/chihiro/): HOTD3 links the same build of Sega's I/O library
 * as VC3 -- each function the same size, 0x2D560 further on (VC3's VA +
 * 0x2D560) -- and the XDK's D3D and XAPI, from XDK 4831. The addresses
 * were found with scripts/find_chihiro_syms.py and by matching VC3's
 * functions (docs/chihiro-symbols.md); see each line. The diagnostics the
 * lifted code calls are the runtime's (host/recomp_diag.c).
 */

#include <stdio.h>
#include <stdint.h>

/* Thread-local guest registers, MEM/XBOX_PTR, recomp_func_t (see VC3's
 * recomp_manual.c: a plain extern g_eax would not be the lifted code's). */
#include "recomp_types.h"

/* ── Replacements (re-lift with --exclude-manual this file) ───
 *
 * A `void sub_XXXXXXXX(void)` defined here replaces the lifted body: every
 * direct call, tail jump and ICALL reaches this definition instead.
 *
 * Chihiro baseboard storage and clock (Sega XPP library, stdcall, 4 args). */
extern void chihiro_backup_read(void);
extern void chihiro_backup_write(void);
extern void chihiro_eeprom_read(void);
extern void chihiro_eeprom_write(void);
extern void chihiro_rtc_read(void);

void sub_0016B2F0(void) { chihiro_backup_read(); }   /* JvsBACKUP_Read  (VC3 0x13DD90) */
void sub_0016B440(void) { chihiro_backup_write(); }  /* JvsBACKUP_Write (VC3 0x13DEE0) */
void sub_0016AF70(void) { chihiro_eeprom_read(); }   /* JvsEEPROM_Read  (VC3 0x13DA10) */
void sub_0016B130(void) { chihiro_eeprom_write(); }  /* JvsEEPROM_Write (VC3 0x13DBD0) */
void sub_0016BB40(void) { chihiro_rtc_read(); }      /* JvsRTC_Read     (VC3 0x13E5E0) */

/* JVS I/O: the game's frames go to the software I/O board (jvs.c). */
extern void chihiro_jvs_send(void);
extern void chihiro_jvs_receive(void);
extern void chihiro_jvs_link_up(void);

void sub_0016B670(void) { chihiro_jvs_send(); }      /* JvsNodeSendPacket    (VC3 0x13E110) */
void sub_0016B590(void) { chihiro_jvs_receive(); }   /* JvsNodeReceivePacket (VC3 0x13E030) */
/* the game's wrapper: JVS_SendCommand(h, 0x15, ...) then 0x169930, as
 * VC3's 0x104F80 */
void sub_00121FF0(void) { chihiro_jvs_link_up(); }

/* Baseboard firmware handshake (baseboard.c). The firmware files
 * (d:\firmware.bin, firmware2.bin; not in this game's folder) are read only
 * inside JammaFirmwareLoad (via 0x16A4D0) and the library's "firmware start"
 * (0x16C110, from JVS_SendCommand), as in VC3. */
extern void chihiro_jamma_firmware_load(void);
extern void chihiro_sc_firmware_download(void);

void sub_0016A680(void) { chihiro_jamma_firmware_load(); }  /* JammaFirmwareLoad     (VC3 0x13D120) */
void sub_0016A870(void) { chihiro_sc_firmware_download(); } /* JvsScFirmwareDownload (VC3 0x13D310) */

/* Baseboard DIP switches: picks the display mode, decoded as VC3's
 * ((~dip >> 1) & 0xB at 0xB1215). 0x11CD10 calls 0x122020 (a jump into the
 * library, 0x169920) and maps -1 to 0x80000000, as VC3's 0xF5220. */
extern void chihiro_dip_switches(void);
void sub_0011CD10(void) { chihiro_dip_switches(); }

/* XLaunchNewImage (VC3 0xB6256; the same 317 bytes, frame 0x384): leaving
 * the test menu reboots into the game -- the runtime's relaunch. */
extern void vc3_relaunch(void);
void sub_000B88D1(void) { vc3_relaunch(); }

/* XDK 4831 D3D's fence wait, against the push-buffer executor (d3d_fence.c;
 * the device's fence fields are 4 bytes on from VC3's: the profile's
 * d3d.dev_time, dev_semaphore). stdcall (time, flags), ret 8. */
extern void chihiro_d3d_block_on_time(void);
void sub_0013BC20(void) { chihiro_d3d_block_on_time(); } /* D3D_BlockOnTime (VC3 0x10CAC0) */

/* CRI's lock, the scoped guest CPU (runtime/src/chihiro/cri_lock.c). The ADX
 * manager registers 0xBE6D0 / 0xBE710 as the file system's lock callbacks
 * ([0x30FDC8] / [0x30FDD0], arg [0x30FDCC]): raise the caller's priority,
 * suspend the idle thread, count in [0x30F708] -- a lock only on one CPU.
 * cdecl, one argument the caller drops. The servers' steps go inside the
 * same lock: the main server 0xC7990 (ADXT, the file system that issues the
 * reads, Sofdec; behind a busy flag), run each vblank by thread 0xBEC20, and
 * the vsync thread 0xBE7F0's (0xC7640) and the file-system thread 0xBE860's
 * (0xC7660). */
extern void ch_cri_lock(void);
extern void ch_cri_unlock(void);
extern void ch_cri_server(void (*step)(void));
void sub_000BE6D0(void) { ch_cri_lock(); g_esp += 4; }     /* ret */
void sub_000BE710(void) { ch_cri_unlock(); g_esp += 4; }   /* ret */
extern void sub_000C7640_gen(void);
extern void sub_000C7660_gen(void);
extern void sub_000C7990_gen(void);
void sub_000C7990(void) { ch_cri_server(sub_000C7990_gen); }
void sub_000C7640(void) { ch_cri_server(sub_000C7640_gen); }
void sub_000C7660(void) { ch_cri_server(sub_000C7660_gen); }

/* ── Manual function overrides ─────────────────────────────── */

/* A function pointer to run for an Xbox VA instead of the dispatch table's,
 * or NULL. Called on every indirect call: keep it fast. */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    (void)xbox_va;
    return (recomp_func_t)0;
}
