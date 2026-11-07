/* vk_mailbox_policy.h - the GPU mailbox's full-queue rule (vk_gpu_mailbox.c),
 * apart so it can be tested without a GPU (tests/vk_mailbox_policy_test.c). */
#ifndef CH_VK_MAILBOX_POLICY_H
#define CH_VK_MAILBOX_POLICY_H

#define CH_VK_MAILBOX_ROOM        (-1)   /* the queue has room: publish */
#define CH_VK_MAILBOX_DROP_OLDEST (-2)   /* drop pending[0], then publish */

/* A new frame to publish, `waiting` frames queued (at most `depth`), ready[k]:
 * pending frame k's GPU copy has finished (frames finish in order). A full
 * queue drops its oldest frame only once a newer one has finished -- it is
 * superseded, and that bounds the added latency. Otherwise the publisher
 * waits for pending[1] to finish (the GPU behind the game): dropped
 * unfinished for the next unfinished one, every waiting frame was replaced
 * before the GPU finished it and the headset got none at all. A queue of one
 * (newest only, opt-in) keeps dropping its oldest. Returns a CH_VK_MAILBOX_*
 * action, or the index of the pending frame to wait for. */
static inline int ch_vk_mailbox_full_action(unsigned waiting, unsigned depth, const int *ready)
{
    if (waiting < depth) return CH_VK_MAILBOX_ROOM;
    if (depth < 2 || ready[1]) return CH_VK_MAILBOX_DROP_OLDEST;
    return 1;
}

#endif
