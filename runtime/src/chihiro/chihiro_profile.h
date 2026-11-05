/*
 * chihiro_profile.h - what a Chihiro game tells the runtime about itself.
 *
 * The runtime (the baseboard and JVS HLE, D3D's fence wait, the GL back end,
 * the Quest host) is shared by every Chihiro game. Where it needs a game's
 * own number -- an address in its XBE, its display modes, how its gun reads,
 * where its HUD sits -- it takes it from the game's profile, one block in the
 * game's games/<id>/src/game.c; a field left 0 (or NULL) keeps the runtime's
 * default. The CHIHIRO_* / <env_prefix>_* environment switches (ch_env.h)
 * still override any of it, for a test run.
 *
 * Game-specific code -- a replaced guest function (recomp_manual.c), the
 * game's idle loop -- stays in the game's own files; this is only for
 * settings and the few hooks the runtime calls back. Modelled on the
 * Lindbergh runtime's LgGameProfile (lindberghrecomp/src/runtime/
 * game_profile.h).
 */
#ifndef CHIHIRO_PROFILE_H
#define CHIHIRO_PROFILE_H

#include <stdint.h>
#include "host/vr_rifle.h"

enum { CH_VR_HANDGUN = 0, CH_VR_RIFLE = 1 };

/* A display mode the DIP switches can pick: the value the switches read,
 * and the frame the toolkit's window is sized for. */
typedef struct {
    const char *res;            /* <prefix>_RES value that picks it ("800"; NULL: the default) */
    uint32_t dip;               /* what the baseboard DIP switches report */
    const char *fb_size;        /* RECOMP_FB_SIZE, "WxH" */
    const char *window_scale;   /* RECOMP_WINDOW_SCALE */
} ChihiroDisplayMode;

/* What the game was made for. A retail Xbox title has no baseboard and
 * reaches its controller through USB (the toolkit's OHCI model); the
 * Chihiro-only blocks below (xpp, modes' DIP values, jvs, aim) stay 0. */
enum {
    CHIHIRO_PLATFORM_CHIHIRO = 0,   /* a Sega Chihiro game (the default) */
    CHIHIRO_PLATFORM_XBOX = 1,      /* a retail Xbox title */
};

typedef struct ChihiroProfile {
    const char *id;             /* games/<id> */
    int platform;               /* CHIHIRO_PLATFORM_* */
    const char *title;          /* logs, the window, the Quest's menus */
    const char *xbe;            /* the game's XBE in its folder ("vc3.xbe") */
    uint32_t entry;             /* the XBE entry point, as lifted */
    const char *env_prefix;     /* ch_env's alias prefix ("VC3": VC3_FPS = CHIHIRO_FPS) */
    const char *save_name;      /* the save folder under ~/Library/Application Support */

    /* The SEGA XPP library and the game's own wrappers the runtime replaces,
     * by guest address (scripts/find_chihiro_syms.py finds them; the game's
     * recomp_manual.c names the same addresses as sub_XXXXXXXX). 0: absent. */
    struct {
        uint32_t backup_read, backup_write, eeprom_read, eeprom_write, rtc_read;
        uint32_t jvs_send, jvs_receive, jvs_link_up;
        uint32_t jamma_firmware_load, sc_firmware_download;
        uint32_t dip_switches;
        uint32_t devunit_probe;     /* the boot probe's "is a Chihiro" test */
        uint32_t devunit_flag;      /* the flag it sets (written 1 outright) */
    } xpp;

    /* XDK D3D, linked into the XBE (d3d_fence.c) */
    struct {
        uint32_t block_on_time;     /* D3D_BlockOnTime, replaced */
        uint32_t device_ptr;        /* g_pDevice */
        uint32_t kickoff_ret;       /* the return address BlockOnTime's KickOff call pushes */
        void (*kickoff)(void);      /* KickOff(0), the lifted function */
        uint32_t xlaunch;           /* XLaunchNewImage, replaced: a relaunch */
    } d3d;

    /* The baseboard DIP switches: the display mode the boot picks from them
     * (first entry: the default); <prefix>_DIP sets the raw value. */
    ChihiroDisplayMode modes[4];

    /* The JVS I/O board the game is given (jvs.c) */
    struct {
        const char *ident;          /* the identify reply */
        int players, switch_bytes, coin_slots, analog_channels, screen_channels;
        /* the player switch bits the host's inputs press */
        uint16_t trigger, offscreen, change, pedal;
    } jvs;

    /* The light gun: the game's reading of the gun into screen position, and
     * the calibration it applies (host_input.c) */
    struct {
        uint32_t backup_offset;     /* the calibration's record in backup RAM (0x0010 tag, 10 int16) */
        int16_t defaults[10];       /* a real gun's calibration, used when none is saved */
        float dots_x[3], dots_y[3]; /* the calibration dots: left/centre/right x, top/centre/bottom y */
        float units_x, units_y;     /* gun units per reading step across / down (y up) */
        float screen_w, screen_h;   /* the screen the dots are on */
    } aim;

    /* The GL back end (gl_backend.c). Per-draw callbacks are read once. */
    struct {
        /* 16:9: a HUD widget's placement, -1 left edge, 1 right edge, 0 kept
         * with the 4:3 picture (NULL: every widget kept with the picture) */
        int (*hud_place)(float x0, float x1, float screen_w);
        int picture_min_w;          /* a full-screen texture at least this wide is a 4:3 picture */
        int lazy_rt;                /* skip copies into a target nothing samples (-1: off, 0/1: on) */
        float stereo_depth;         /* stereo convergence, clip w (0: 8) */
        float stereo_sep;           /* separation at infinity, NDC (0: 0.04) */
        float units_per_m;          /* game units per metre, immersive (0: 1) */
        int buffers;                /* the shown surfaces remembered (the game's swap chain; 0: 4) */
        float zoom_tolerance;       /* focal lengths this close are one zoom (0: 0.5) */
        /* The effect target (gl_backend.c): a run of blended layers sampling
         * a scene copy drawn at 1/effect_scale each way (0: 2; 1: off),
         * after a frame with effect_min_layers of them (0: 8), into stereo
         * surfaces of effect_min_px pixels an eye or more (0: 2000000). */
        int effect_scale, effect_min_layers;
        long effect_min_px;
    } gpu;

    /* The headset (src/android) */
    struct {
        const char *app_name;       /* OpenXR's application name */
        const char *panel_title;    /* the settings panel's */
        const char *sdcard_dir;     /* the game's folder on shared storage */
        float sight_height[3];      /* iron, red dot, holo, metres over the bore (0: the overlay's) */
        int weapon;                 /* CH_VR_HANDGUN or CH_VR_RIFLE */
        float lens_k;               /* the main view's focal length, pixels at 640 wide (0: 772.5) */
        /* The HUD's pieces, rectangles of its 640x480 picture, y down (count 0:
         * one quad); dx, dy: which way the piece spreads (-1..1; 0: stays) */
        struct { float x0, y0, x1, y1; signed char dx, dy; } hud_tiles[10];
        int hud_tile_count;
        void (*aim_screen)(int on, int x, int y);   /* the gun's point beyond the screen's edge (NULL: none) */
        void (*cull)(float half_angle_rad);         /* widen the CPU culling to this half angle; 0: the game's own */
        void (*rifle)(const ChVrRifle *r);          /* the rifle's pose and zoom, each frame (NULL: none) */
    } vr;

    /* Hooks (NULL: none) */
    struct {
        void (*on_boot)(void);                      /* after the kernel and the HLE are up */
        void (*launch_data)(uint8_t page[4096]);    /* fill <prefix>_TEST_LAUNCH's launch data page */
    } hooks;
} ChihiroProfile;

/* The game's profile (games/<id>/src/game.c). */
const ChihiroProfile *chihiro_game(void);
/* Called first thing in main (and the Quest's SDL_main): the profile in
 * force, its env prefix set. Idempotent. */
void chihiro_profile_apply(const ChihiroProfile *p);
const ChihiroProfile *chihiro_profile(void);

#endif
