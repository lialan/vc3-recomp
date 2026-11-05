/* retail_pad.h - a headset's controllers as a retail Xbox title's pad (the
 * rifle, Silent Scope): the keys held go into the toolkit's keyboard table,
 * which the title reads as player 1's pad (toolkit input/keyboard.c). */
#ifndef CH_RETAIL_PAD_H
#define CH_RETAIL_PAD_H

/* retail_pad_map's buttons: what the controllers hold this frame */
enum {
    RETAIL_PAD_TRIGGER = 1 << 0,    /* the gun hand's trigger: fire */
    RETAIL_PAD_A       = 1 << 1,    /* the gun hand's lower button (A; X on the left) */
    RETAIL_PAD_B       = 1 << 2,    /* its upper button (B; Y on the left) */
    RETAIL_PAD_START   = 1 << 3,    /* a thumbstick click, either hand */
    RETAIL_PAD_PANEL   = 1 << 4,    /* the settings panel is open: the game gets nothing */
};

/* The pad's keys, 1 held */
typedef struct { int start, a, b, x, y, rt, lt, up, down, left, right; } RetailPadKeys;

/* Which pad keys are down. stick: the gun hand's thumbstick; up/down past
 * half way is the d-pad's, except while scope_on (the scope's zoom then);
 * left/right always the d-pad's. */
void retail_pad_map(int buttons, float stick_x, float stick_y, int scope_on, RetailPadKeys *out);
/* Presses and releases them through xbox_InputKeySet with the keyboard
 * map's keys (Enter Start, Z A, X B, C X, V Y, 3 right trigger, 1 left
 * trigger, the arrows the d-pad), each only when it changes. */
void retail_pad_apply(const RetailPadKeys *k);
/* The gun hand's thumbstick (x, y): sticks[0] the left controller's,
 * sticks[1] the right's; gun_hand 0 left, 1 right. */
void retail_pad_stick(int gun_hand, const float sticks[2][2], float out[2]);

#endif
