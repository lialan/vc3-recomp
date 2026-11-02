# Queue depth copies with explicit target lifetimes

Shared depth residency now uses a queued-copy API; the original public copy API remains synchronous. For a pending source and an idle destination, append the copy to source rendering and submit without an immediate CPU wait. For a busy destination, first submit its older accesses, then submit source rendering plus the depth copy, then retire only the destination's older fence before recording future destination work. The source and destination use distinct attachments and immutable sampled snapshots; the existing same-queue depth barriers preserve copy/read/write ordering. Copies without pending source work retain the synchronous fallback.

A bidirectional source/dependent link keeps both depth images and the source command/descriptors alive. Source reuse retires its outstanding copy before mutable draw preparation. Destination command recording/submission can overlap it; destination batch retirement also retires the earlier copy. Readbacks, snapshots, standalone operations, guest completion and either destruction order drain the dependency. New copies drain previous edges, preventing cycles. A destination cannot expose an older completion fence while its incoming copy is outstanding. Failure drains submitted work and propagates failed-source state to its dependent. Links detach immediately after fence completion, before retirement callbacks.

Enabled by default; CHIHIRO_VULKAN_ASYNC_DEPTH_COPIES=0 selects the synchronous control. No game-specific paths, shaders, pipeline identities or guest clocks change. An initial idle-only version queued only4 copies over2100frames; supporting busy destinations increased this to1933. Idle-only reports are archived for provenance, not final performance claims.

NativeVC3 and Android sharedVulkan builds passed. Program-render, texture-render and depth-pack GPU fixtures passed synchronization validation in enabled/disabled modes. New direct GPU tests cover13 cases: source/destination readbacks, redraws, reverse copies, snapshots, asynchronous depth reads, busy destinations, destruction of either target, precise copied depth/stencil and completion-fence validity. Existing guest-memory parity cases remain passing. No Validation Error/SYNC-HAZARD.

Final same-binary/same-seed pair: /tmp/vc3-queued-busy-depth-control and -candidate, executable /tmp/vc3-queued-busy-depth-vc3,2100framesexit0 each. Modes0/1 are the only settings difference besides output/save/config/cache paths; seed checksum recorded. Full-run pipeline creation91.594/85.383ms. Workload counts differ slightly; this is one sequential pair, not repeated randomized proof. A final lifetime cleanup moved backlink detachment before retirement callbacks after the pair; native/Android were rebuilt and texture GPU synchronization validation rerun. Final executable /tmp/vc3-queued-depth-copy-final-vc3 is recorded separately; do not overwrite the benchmark binary/provenance.

Non-compilation means control->candidate, combat/activation/held:
- Depth-copy API wall:0.910->0.413 /0.787->0.375 /0.863->0.380ms.
- Draw callback wall:4.138->3.496 /6.553->6.090 /6.286->5.698ms.
- Completion callback wall:4.576->5.098 /4.693->5.066 /4.742->5.113ms.
- Draw CPU:1.869->1.832 /3.088->3.075 /2.933->2.936ms.
- Batch fence-wait totals:4.720->4.843 /5.634->5.729 /5.714->5.623ms.
Draw and completion callback wall totals together are0.120/0.090/0.217ms lower; this is not a critical-path or FPS-gain guarantee. Much of the wait moved from binding to completion. GPU batch timestamps now include depth-copy work that could previously execute in a separate unmeasured copy submission, so comparing their raw totals alone is misleading. Nested wait/CPU/GPU/copy scopes must not be added to callback durations.

Candidate combat59.94/59.94FPS,activation60.01,held59.90/60.11. P9521.28–23.76ms versus control21.34–22.68ms: mixed pacing results, no steady16.67ms proof. Target frames all below33.333ms. The benefit is verified queue overlap and reduced synchronous copy-boundary work, with modest callback-wall reduction in this pair; repeatability and Quest performance remain unproven.

Both local binaries include unrelated preexisting host_input.c edits, excluded from commit. No headset access/APK/push. Earlier native-strip and descriptor-metadata experiments remain reverted. No new driver pipeline training needed specifically for this scheduling change, but prior shader/state cache refresh remains pending device access. Goal stays active.
