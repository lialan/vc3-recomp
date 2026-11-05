/* retail_pad.c - a headset's controllers as a retail Xbox title's pad. */
#include "retail_pad.h"

#include <stddef.h>
#include <string.h>

void xbox_InputKeySet(int vk, int down);    /* the toolkit's key table (input/keyboard.h) */

void retail_pad_map(int buttons, float stick_x, float stick_y, int scope_on, RetailPadKeys *out)
{
    memset(out, 0, sizeof *out);
    if (buttons & RETAIL_PAD_PANEL)
        return;
    out->rt = (buttons & RETAIL_PAD_TRIGGER) != 0;
    out->a = (buttons & RETAIL_PAD_A) != 0;
    out->b = (buttons & RETAIL_PAD_B) != 0;
    out->start = (buttons & RETAIL_PAD_START) != 0;
    if (!scope_on) {
        out->up = stick_y >= 0.5f;
        out->down = stick_y <= -0.5f;
    }
    out->right = stick_x >= 0.5f;
    out->left = stick_x <= -0.5f;
}

void retail_pad_apply(const RetailPadKeys *k)
{
    /* the keyboard map's keys (toolkit input/keyboard.c), Windows VK codes */
    static const struct { size_t off; int vk; } keys[] = {
        { offsetof(RetailPadKeys, start), 0x0D },   /* Enter */
        { offsetof(RetailPadKeys, a), 'Z' },
        { offsetof(RetailPadKeys, b), 'X' },
        { offsetof(RetailPadKeys, x), 'C' },
        { offsetof(RetailPadKeys, y), 'V' },
        { offsetof(RetailPadKeys, rt), '3' },
        { offsetof(RetailPadKeys, lt), '1' },
        { offsetof(RetailPadKeys, up), 0x26 },      /* the arrows */
        { offsetof(RetailPadKeys, down), 0x28 },
        { offsetof(RetailPadKeys, left), 0x25 },
        { offsetof(RetailPadKeys, right), 0x27 },
    };
    static RetailPadKeys held;
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        int now = *(const int *)((const char *)k + keys[i].off) != 0;
        int *was = (int *)((char *)&held + keys[i].off);
        if (now != *was) {
            xbox_InputKeySet(keys[i].vk, now);
            *was = now;
        }
    }
}

void retail_pad_stick(int gun_hand, const float sticks[2][2], float out[2])
{
    const float *s = sticks[gun_hand ? 1 : 0];
    out[0] = s[0];
    out[1] = s[1];
}
