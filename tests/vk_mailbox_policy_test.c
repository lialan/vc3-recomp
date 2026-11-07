/* tests/vk_mailbox_policy_test.c: the GPU mailbox's full-queue rule
 * (vk_mailbox_policy.h). A frame the GPU has not finished must not be
 * dropped for a newer unfinished one: with the game a few frames ahead of
 * the GPU, every waiting frame was replaced before it finished and the
 * headset got none (HOTD3: 0 game frames a second for seconds at a time). */
#include <stdio.h>
#include "../runtime/src/gpu/vulkan/vk_mailbox_policy.h"

static int fails;
static void expect(const char *what, int got, int want) {
    if (got != want) { printf("FAIL %s: got %d, want %d\n", what, got, want); fails++; }
}

int main(void) {
    /* room in the queue: nothing to do */
    expect("room", ch_vk_mailbox_full_action(1, 2, (const int[]){0, 0}), CH_VK_MAILBOX_ROOM);
    expect("empty", ch_vk_mailbox_full_action(0, 2, (const int[]){0, 0}), CH_VK_MAILBOX_ROOM);
    /* full, a newer frame finished: the oldest is superseded, drop it */
    expect("newer done", ch_vk_mailbox_full_action(2, 2, (const int[]){1, 1}), CH_VK_MAILBOX_DROP_OLDEST);
    /* full, only the oldest finished (the headset has not taken it yet):
     * keep it, wait for the next to finish */
    expect("oldest done only", ch_vk_mailbox_full_action(2, 2, (const int[]){1, 0}), 1);
    /* full, none finished (the game ahead of the GPU): wait, never drop */
    expect("none done", ch_vk_mailbox_full_action(2, 2, (const int[]){0, 0}), 1);
    /* depth 1 (newest only, opt-in): as before, the oldest goes */
    expect("depth 1", ch_vk_mailbox_full_action(1, 1, (const int[]){0}), CH_VK_MAILBOX_DROP_OLDEST);
    /* a deeper queue: any newer finished frame supersedes the oldest */
    expect("depth 3 newest done", ch_vk_mailbox_full_action(3, 3, (const int[]){1, 1, 1}), CH_VK_MAILBOX_DROP_OLDEST);
    expect("depth 3 none done", ch_vk_mailbox_full_action(3, 3, (const int[]){0, 0, 0}), 1);
    if (fails) return 1;
    printf("vk_mailbox_policy_test: all passed\n");
    return 0;
}
