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
#include <time.h>

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

/* CRI's lock, as the console runs it (runtime/src/chihiro/cri_lock.c). The
 * ADX manager registers 0xBE6D0 / 0xBE710 as the file system's lock
 * callbacks ([0x30FDC8] / [0x30FDD0], arg [0x30FDCC]). Lock: from a count of
 * 0 ([0x30F708]) save the caller's priority ([0x4F9220]), raise it to
 * [0x30F6F4] (15) and resume the spinner thread [0x4F9200] (0xBE7B0, at
 * [0x30F6F8] = 2); unlock to 0 suspends it and gives the releaser the saved
 * priority. So while it is held nothing at 2 or below runs: the vsync server
 * 0xBE7F0 (1), the game thread (0), the file-system server 0xBE860 and the
 * loader 0x7AF10 (-2). cdecl, one argument the caller drops. */
extern void ch_cri_lock(int spin_prio, int hold_prio);
extern void ch_cri_unlock(void);
extern void ch_cri_counter_lock(uint32_t counter_va, int spin_prio, int hold_prio);
extern void ch_cri_counter_unlock(uint32_t counter_va);
#define CRI_SPIN ((int)MEM32(0x0030F6F8))
#define CRI_HOLD ((int)MEM32(0x0030F6F4))
void sub_000BE6D0(void) { ch_cri_lock(CRI_SPIN, CRI_HOLD); g_esp += 4; }   /* ret */
void sub_000BE710(void) { ch_cri_unlock(); g_esp += 4; }                   /* ret */
/* The modules' nesting counters: take the lock at 0, give it back at 0 --
 * ADXT's 0xBFB00 / 0xBFB20 on [0x30F8A0], the stream file system's 0xC8260 /
 * 0xC8280 on [0x30FF30]. No arguments, plain ret. */
void sub_000BFB00(void) { ch_cri_counter_lock(0x0030F8A0, CRI_SPIN, CRI_HOLD); g_esp += 4; }
void sub_000BFB20(void) { ch_cri_counter_unlock(0x0030F8A0); g_esp += 4; }
void sub_000C8260(void) { ch_cri_counter_lock(0x0030FF30, CRI_SPIN, CRI_HOLD); g_esp += 4; }
void sub_000C8280(void) { ch_cri_counter_unlock(0x0030FF30); g_esp += 4; }
/* The spinner (0xBE7B0) only counts in [0x30F710] until told to stop
 * ([0x30F728]); on the console it starves the threads below it while the lock
 * is held, which ch_cri_lock does here. Suspending another thread only counts
 * on the host, so it spun a core all the time: wait for the stop request
 * asleep, then run the original, which exits the thread. */
extern void sub_000BE7B0_gen(void);
void sub_000BE7B0(void)
{
    const struct timespec ts = { 0, 20 * 1000 * 1000 };
    while (!MEM32(0x0030F728))
        nanosleep(&ts, NULL);
    sub_000BE7B0_gen();
}

/* The guns' readings into the players' aim (once a frame, from 0x84E0F),
 * then player 1's from the VR host, which may lie beyond the screen
 * (vr_aim.c). */
extern void sub_00079E80_gen(void);
extern void hotd3_gun_aim(void (*generated)(void));
void sub_00079E80(void) { hotd3_gun_aim(sub_00079E80_gen); }

/* The stage's culling test, a part's bounding sphere against the camera's
 * view (from the stage's drawer 0x13F00), widened while immersive
 * (vr_cull.c). */
extern void sub_00048F50_gen(void);
extern void hotd3_cull_sphere(void (*generated)(void));
void sub_00048F50(void) { hotd3_cull_sphere(sub_00048F50_gen); }

/* ── Manual function overrides ─────────────────────────────── */

/* A function pointer to run for an Xbox VA instead of the dispatch table's,
 * or NULL. Called on every indirect call: keep it fast. */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    (void)xbox_va;
    return (recomp_func_t)0;
}
