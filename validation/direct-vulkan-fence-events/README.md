# Rejected experiment: notification-driven guest fence waits

A bounded shared queue notification API was implemented and exercised by BlockOnTime instead of its 50 us poll, preserving ring-time comparisons, kickoff, strict watchdogs and synchronous fallback. Real-queue and mock-clock fence fixtures passed timeout/no-premature-release, source lifetime, ordering, wraparound and stuck-fence behavior. Native and Android builds passed. One local 2,100-frame run completed and saved 71,705,540 cache bytes.

The gameplay trace disproved the expected warm-path benefit: nine of ten samples in frames 1320–1920 reported zero slept calls and only 0.03–0.05ms total waiting per roughly one second. The exceptional sample near 1498 had 3 slept calls and 366.77 ms wait, associated with a large stall. Other sleeps clustered around loading/transition/stall regions. Ordinary warm combat/slow-motion fence calls were already complete before checking.

Decision: do not retain the additional condition-variable/notification machinery for negligible warm benefit. Production and test sources restored to HEAD 0980445 / nested c86f8ae0; patches here preserve the rejected experiment and reproduction evidence. No measured FPS gain claimed. This rules out guest BlockOnTime polling as the next warm-rendering target, not all executor or GPU waits.

Binary /tmp/vc3-fence-events-vc3 and run /tmp/vc3-fence-events are EXPERIMENTAL, not the retained implementation. Do not package them. Rebuild the restored tree before subsequent runs. No headset access or APK changes. Focus next on submission/completion work and GPU/readback costs rather than another fence-poll tuning pass.

The restored sources were subsequently rebuilt for native and Android targets as part of the draw-submission optimization. The named experimental binary remains unsuitable for packaging.
