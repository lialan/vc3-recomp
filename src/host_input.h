/* Host controls (host_input.c) as other host code sees them. */
#ifndef VC3_HOST_INPUT_H
#define VC3_HOST_INPUT_H

/* A headset's controllers (src/android/quest_host.c): the gun's position in
 * the game's 4:3 picture, 0..1 each way (negative: off the screen), and the
 * buttons held. They take the mouse's place from the first call. */
#define VC3_VR_FIRE   0x01
#define VC3_VR_RELOAD 0x02
#define VC3_VR_START  0x04
#define VC3_VR_COIN   0x08
#define VC3_VR_TEST   0x10
#define VC3_VR_PEDAL  0x20
#define VC3_VR_CHANGE 0x40
#define VC3_VR_SERVICE 0x80
void vc3_input_vr(double ux, double uy, int buttons);

/* Keyboard bindings (the Controls menu): actions, their keys, learning a
 * new key from the next press. Saved as key_<action>= in the settings. */
enum {
    VC3_KEY_COIN1, VC3_KEY_COIN2, VC3_KEY_START1, VC3_KEY_START2,
    VC3_KEY_FIRE, VC3_KEY_RELOAD, VC3_KEY_PEDAL, VC3_KEY_CHANGE,
    VC3_KEY_TEST, VC3_KEY_SERVICE, VC3_KEY_FPS,
    VC3_KEY_COUNT
};
const char *vc3_key_action_name(int action);
const char *vc3_key_name(int action);      /* "" when unbound */
void vc3_key_learn(int action);            /* next key press; Escape: none */
int  vc3_key_learning(void);               /* the action waiting, or -1 */
void vc3_key_defaults(void);

#endif
