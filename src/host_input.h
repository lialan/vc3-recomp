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
void vc3_input_vr(double ux, double uy, int buttons);

#endif
