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

struct ChScopeGame;     /* host/scope_game.h */

/* The gun held in VR (vr.weapon). The shotgun is the handgun in all but
 * what is drawn: a pump-action shotgun (the Vulkan renderer's; the GL
 * renderer draws the handgun), its pump worked by each shot and reload. */
enum { CH_VR_HANDGUN = 0, CH_VR_RIFLE = 1, CH_VR_SHOTGUN = 2 };

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
    /* The game id of the disc's launcher (default.xbe; Silent Scope
     * Complete's "ssc"): another image a title launches runs as the binary
     * beside this one named after it ("d:\ss2.xbe": ss2), default.xbe and
     * the dashboard as this one (main.c chihiro_launch_image). NULL: a
     * launch exits, as before. */
    const char *disc_launcher;

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
        /* Where the device keeps its newest fence time and its pointer to
         * the GPU's progress (0: XDK 5028's and 5558's 0x2C and 0x30;
         * XDK 4831 -- House of the Dead III -- keeps them at 0x30, 0x34) */
        uint32_t dev_time, dev_semaphore;
        /* The device made without anti-aliasing (the game wraps its
         * Direct3D_CreateDevice with chihiro_d3d_create_device): it draws at
         * its own size, as the renderer's screen-shaped surfaces expect */
        int no_multisample;
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
        /* or in EEPROM (HOTD3): three dots' raw readings as dwords, x then y,
         * top-left/centre/bottom-right; read into the layout above as
         * x = reading - 128, y = 128 - reading (units 1) */
        uint32_t eeprom_offset;
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
        /* Vulkan: the game never reads its rendered pixels back from memory,
         * so its flips need not publish or wait for them (vk_backend.h
         * lazy_flip; CHIHIRO_VULKAN_LAZY_FLIP overrides) */
        int lazy_flip;
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
        int weapon;                 /* CH_VR_HANDGUN, CH_VR_RIFLE or CH_VR_SHOTGUN */
        float lens_k;               /* the main view's focal length, pixels at 640 wide (0: 772.5) */
        float render_scale;         /* immersive eyes fixed at this share of native (1: native); 0: automatic */
        /* The main view's projection for a game that draws its 3D with vertex
         * programs only (no fixed-function batch to read one from), as the
         * immersive view recovers it (vr_math.h VrGameProj): screen = (k e.xy
         * / e.z + c) in 640x480 pixels, eye z = clip w * inv_pz (kx 0: none) */
        struct { float kx, ky, cx, cy, inv_pz; } program_proj;
        /* A vertex-program batch whose clip w is over 0 and at most this is
         * screen space, not the world (0: only w 1 is): House of the Dead
         * III draws all its 2D -- HUD, text, copies, sprites -- through
         * D3D's pass-through for pre-transformed vertices with rhw 10, 1/168
         * or 1/560 (its near plane), and its world fixed-function. */
        float program_flat_w;
        /* 2D drawn on the scene before the scene is composited onto the
         * screen is the HUD too, not only 2D drawn on the screen after
         * (House of the Dead III draws player 1's life, RELOAD and CONTINUE
         * into its scene buffer); full-screen 2D there stays in the world */
        int scene_hud;
        /* The immersive eyes draw the world behind the game's camera too: its
         * 3D is not clipped at the camera's plane (clip w > 0) and near plane
         * as the console clips it, only to each eye's view -- for a game
         * that draws what lies behind its camera (House of the Dead III
         * draws its rooms whole); 0: clipped there, the head turned past 90
         * degrees sees nothing */
        int world_behind;
        /* The immersive eyes clear to black, not to the game's clear colour:
         * what shows where the game drew nothing (House of the Dead III
         * clears to its fog colour, red in the lab -- a doorway it never
         * draws from behind showed as a red hole); 0: the game's colour */
        int eye_clear_black;
        /* The HUD's pieces, rectangles of its 640x480 picture, y down (count 0:
         * one quad); dx, dy: which way the piece spreads (-1..1; 0: stays) */
        struct { float x0, y0, x1, y1; signed char dx, dy; } hud_tiles[10];
        int hud_tile_count;
        void (*aim_screen)(int on, int x, int y);   /* the gun's point beyond the screen's edge (NULL: none) */
        void (*cull)(float half_angle_rad);         /* widen the CPU culling to this half angle; 0: the game's own */
        void (*rifle)(const ChVrRifle *r);          /* the rifle's pose and zoom, each frame (NULL: none) */
        /* What the lens picture handed over now was drawn with (vr_view.h),
         * asked by the renderer at each flip with one; 0: not known (NULL: none) */
        int (*lens_aim)(VrLensAim *out);
        /* Non-zero while the game's draws into the screen are a view it
         * then copies (Silent Scope 3's scope): immersive, they are drawn
         * on the game's own surface, flat, for the copy; asked by the
         * renderer at each draw (NULL: never) */
        int (*flat_now)(uint32_t release);
    } vr;

    /* A game aimed through a sniper scope: where its aim, zoom and scope
     * pass live, for the shared VR rifle and 16:9 view (host/scope_game.h;
     * NULL: none) */
    const struct ChScopeGame *scope;

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
