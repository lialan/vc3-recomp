/* retail_pad_test.c - checks of runtime/src/host/retail_pad.c, the VR
 * controllers as a retail Xbox title's pad (scripts/vr-rifle-test.sh). The
 * toolkit's xbox_InputKeySet is a stub here that records each call. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "retail_pad.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* the stub: the calls since the last reset */
static struct { int vk, down; } s_calls[64];
static int s_ncalls;
void xbox_InputKeySet(int vk, int down)
{
    if (s_ncalls < 64) {
        s_calls[s_ncalls].vk = vk;
        s_calls[s_ncalls].down = down;
    }
    s_ncalls++;
}

static int keys_down(const RetailPadKeys *k)
{
    return k->start + k->a + k->b + k->x + k->y + k->rt + k->lt + k->up + k->down + k->left + k->right;
}

static void trigger_fires(void)
{
    RetailPadKeys k;
    retail_pad_map(RETAIL_PAD_TRIGGER, 0, 0, 0, &k);
    CHECK(k.rt == 1);
    CHECK(keys_down(&k) == 1);
    retail_pad_map(RETAIL_PAD_TRIGGER, 0, 0, 1, &k);
    CHECK(k.rt == 1);
    CHECK(keys_down(&k) == 1);
}

static void buttons_map(void)
{
    RetailPadKeys k;
    retail_pad_map(RETAIL_PAD_A, 0, 0, 0, &k);
    CHECK(k.a == 1 && keys_down(&k) == 1);
    retail_pad_map(RETAIL_PAD_B, 0, 0, 0, &k);
    CHECK(k.b == 1 && keys_down(&k) == 1);
    retail_pad_map(RETAIL_PAD_START, 0, 0, 0, &k);
    CHECK(k.start == 1 && keys_down(&k) == 1);
    retail_pad_map(0, 0, 0, 0, &k);
    CHECK(keys_down(&k) == 0);
}

static void stick_zooms_with_scope(void)
{
    RetailPadKeys k;
    retail_pad_map(0, 0, 0.9f, 1, &k);
    CHECK(!k.up && !k.down && !k.left && !k.right);
    retail_pad_map(0, 0, -0.9f, 1, &k);
    CHECK(!k.up && !k.down && !k.left && !k.right);
}

static void stick_routes_to_dpad_without_scope(void)
{
    RetailPadKeys k;
    retail_pad_map(0, 0, 0.9f, 0, &k);
    CHECK(k.up == 1 && keys_down(&k) == 1);
    retail_pad_map(0, 0, -0.9f, 0, &k);
    CHECK(k.down == 1 && keys_down(&k) == 1);
    retail_pad_map(0, 0.9f, 0, 0, &k);
    CHECK(k.right == 1 && keys_down(&k) == 1);
    retail_pad_map(0, -0.9f, 0, 0, &k);
    CHECK(k.left == 1 && keys_down(&k) == 1);
    retail_pad_map(0, 0.49f, -0.49f, 0, &k);
    CHECK(keys_down(&k) == 0);
    /* left/right stay the d-pad's with the scope on */
    retail_pad_map(0, 0.9f, 0, 1, &k);
    CHECK(k.right == 1 && keys_down(&k) == 1);
}

static void panel_open_freezes_rifle(void)
{
    RetailPadKeys k;
    retail_pad_map(RETAIL_PAD_PANEL | RETAIL_PAD_TRIGGER | RETAIL_PAD_A | RETAIL_PAD_B | RETAIL_PAD_START,
                   0.9f, 0.9f, 0, &k);
    CHECK(keys_down(&k) == 0);
    retail_pad_map(RETAIL_PAD_PANEL, -0.9f, -0.9f, 1, &k);
    CHECK(keys_down(&k) == 0);
}

static void left_hand_uses_left_stick(void)
{
    const float sticks[2][2] = { { 0.1f, 0.9f }, { 0, -0.9f } };
    float out[2];
    retail_pad_stick(0, sticks, out);
    CHECK(out[0] == 0.1f && out[1] == 0.9f);
    retail_pad_stick(1, sticks, out);
    CHECK(out[0] == 0 && out[1] == -0.9f);
}

/* the keyboard map's keys (toolkit input/keyboard.c), pressed and released
 * only when they change */
static void apply_presses_on_change(void)
{
    static const struct { int vk; size_t off; } want[] = {
        { 0x0D, offsetof(RetailPadKeys, start) }, { 'Z', offsetof(RetailPadKeys, a) },
        { 'X', offsetof(RetailPadKeys, b) },      { 'C', offsetof(RetailPadKeys, x) },
        { 'V', offsetof(RetailPadKeys, y) },      { '3', offsetof(RetailPadKeys, rt) },
        { '1', offsetof(RetailPadKeys, lt) },     { 0x26, offsetof(RetailPadKeys, up) },
        { 0x28, offsetof(RetailPadKeys, down) },  { 0x25, offsetof(RetailPadKeys, left) },
        { 0x27, offsetof(RetailPadKeys, right) },
    };
    const int n = (int)(sizeof want / sizeof *want);
    RetailPadKeys none, k;
    memset(&none, 0, sizeof none);

    s_ncalls = 0;
    retail_pad_apply(&none);                    /* nothing held, nothing changes */
    CHECK(s_ncalls == 0);
    for (int i = 0; i < n; i++) {               /* each key alone: its own VK, down then up */
        k = none;
        *(int *)((char *)&k + want[i].off) = 1;
        s_ncalls = 0;
        retail_pad_apply(&k);
        CHECK(s_ncalls == 1 && s_calls[0].vk == want[i].vk && s_calls[0].down == 1);
        s_ncalls = 0;
        retail_pad_apply(&k);                   /* held: no call */
        CHECK(s_ncalls == 0);
        retail_pad_apply(&none);
        CHECK(s_ncalls == 1 && s_calls[0].vk == want[i].vk && s_calls[0].down == 0);
    }
    /* the trigger held while the d-pad changes: only the d-pad's calls */
    k = none;
    k.rt = 1;
    s_ncalls = 0;
    retail_pad_apply(&k);
    k.up = 1;
    s_ncalls = 0;
    retail_pad_apply(&k);
    CHECK(s_ncalls == 1 && s_calls[0].vk == 0x26 && s_calls[0].down == 1);
    s_ncalls = 0;
    retail_pad_apply(&none);
    CHECK(s_ncalls == 2);
}

int main(void)
{
    trigger_fires();
    buttons_map();
    stick_zooms_with_scope();
    stick_routes_to_dpad_without_scope();
    panel_open_freezes_rifle();
    left_hand_uses_left_stick();
    apply_presses_on_change();
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("all passed\n");
    return 0;
}
