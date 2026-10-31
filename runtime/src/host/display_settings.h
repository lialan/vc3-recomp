/* Display settings (display_settings.c): how the GL back end renders and
 * presents, never how the game runs. Each is an atomic word, set on the
 * main thread (menu, F10, startup) and read by the executor at a flip. */
#ifndef VC3_DISPLAY_SETTINGS_H
#define VC3_DISPLAY_SETTINGS_H

#define VC3_FPS_MAX   240
#define VC3_SCALE_MAX 4

void vc3_display_init(void);  /* env, else the settings file, else defaults */

/* Frames shown per second: 60 = the real ones; 120 adds one
 * motion-interpolated frame between each pair, up to VC3_FPS_MAX. */
int  vc3_fps_get(void);
void vc3_fps_set(int fps);    /* and save it to the settings file */
void vc3_fps_toggle(void);    /* 60 <-> 120 (F10, the menu's key) */

/* Internal resolution: the game's surfaces drawn at 1..VC3_SCALE_MAX times
 * their size; the window shows the result, guest memory gets 1x. */
int  vc3_scale_get(void);
void vc3_scale_set(int scale);

/* 16:9: the 640x480 surfaces drawn a third wider, the 3D view extended to
 * the sides (hor+), the HUD at the screen edges, movies and menus kept 4:3. */
int  vc3_wide_get(void);
void vc3_wide_set(int wide);

/* Stereo 3D (gl_backend.c): the 3D drawn once per eye, with depth. On the
 * Quest each eye sees its own; on a desktop the two are side by side. */
int  vc3_stereo_get(void);
void vc3_stereo_set(int on);

/* Any "<key>=" line of the settings file (key includes the '='). */
const char *vc3_setting_get(const char *key);
void vc3_setting_put(const char *key, const char *value);

#endif
