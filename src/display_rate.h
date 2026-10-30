/* Display frame rate (display_rate.c): 60 = one shown frame per game frame,
 * 120 = one motion-interpolated frame between each pair, up to VC3_FPS_MAX. */
#ifndef VC3_DISPLAY_RATE_H
#define VC3_DISPLAY_RATE_H

#define VC3_FPS_MAX 240

void vc3_fps_init(void);      /* VC3_FPS, else the settings file, else 60 */
int  vc3_fps_get(void);
void vc3_fps_set(int fps);    /* and save it to the settings file */
void vc3_fps_toggle(void);    /* 60 <-> 120 (F10, the menu's key) */

#endif
