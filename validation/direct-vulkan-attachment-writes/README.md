# Attachment publication eligibility

Shared Vulkan dirty tracking now considers reachable alpha/depth/stencil outcomes. KEEP-only stencil writes and impossible comparison paths no longer dirty attachments or invalidate color snapshots. Draw execution, imports, validation, and guest completion remain intact. This reduces unnecessary publication for all callers using resident attachments; strict color fallback still publishes as before.

Native VC3 and Android chihiro_vulkan builds passed. Texture and depth fixtures passed with KHRONOS synchronization validation, no validation errors or hazards. Added seven actual GPU raster cases on supported D32/D24 targets: depth NEVER, all KEEP, stencil fail REPLACE, depth fail REPLACE, unreachable zfail, alpha NEVER, and unreachable fail/zfail. They verify exact color/depth-stencil bytes against the predicted write mask, including changes where expected.

Local run: /tmp/vc3-attachment-writes, preserved binary /tmp/vc3-attachment-writes-vc3. 2100 frames, exit 0. Seed copied from preceding snapshot-ownership candidate. No headset or APK changes. Local binary includes unrelated existing host_input.c edits, excluded from this commit.

743 draws avoided false depth/stencil dirty marking; zero color cases. This is not 743 eliminated readbacks. Final depth writebacks 11427 versus previous run 11445, but workload counts also varied, so no causal savings or FPS gain is claimed from that comparison. No matched A/B was run for this small change.

Combat windows: 59.98 / 60.03 FPS; slow activation 59.84; held slow 60.11 / 59.95. Their p95 frame times 20.05 / 22.94 / 22.97 / 22.95 / 21.62 ms. Pacing remains unresolved. Later 1921-2041 window includes 301.1 ms maximum and 53.36 FPS; do not describe the entire run as steady 60. Noncompile completion means combat/activation/held 5.136 / 4.962 / 4.891 ms; completion CPU 1.638 / 1.732 / 1.781 ms. No consistent improvement against preceding run.

CPU geometry, resident depth layout/float and strict depth/color counters remain zero. Texture fallback cumulative 1054720 pixels / 6.644 ms for entire run, slightly above prior 1030144; not a claim of zero CPU graphics work. Real Quest throughput and presentation remain untested.
