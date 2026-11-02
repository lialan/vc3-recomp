# Native strips A/B: reverted after completion/pacing concern

Sequential2100frame local comparison, control /tmp/vc3-dynamic-stencil-vc3 (e61468f production code) versus /tmp/vc3-native-strip-vc3 (b308719). Both preserved executable hashes matched their original reports before execution. Both used independent byte-identical copies of /tmp/vc3-native-strip-warm.cache (seed SHA256 in comparison.json), identical settings except cache/output/save/config directory paths, and reached2100frames/exit0. Both include preexisting unrelated host_input.c edits. One sequential pair, not randomized repeated trials; workload counts and compilation differ, so it cannot establish universal driver causality.

Non-compilation frame means, control->candidate, combat/activation/held:
- GPU batch:3.348->2.666 /4.071->4.479 /3.862->3.256ms.
- Completion wall:4.699->5.578 /4.915->6.077 /4.999->5.794ms.
- Batch fence wait:4.818->6.526 /6.052->10.164 /5.776->7.955ms.
- Draw CPU:1.764->1.899 /3.045->3.165 /2.931->3.013ms.
- Prepare CPU:0.211->0.212 /0.296->0.267 /0.305->0.276ms.
These scopes overlap; do not sum. Batch timestamps omit some queue/upload work, so lower batch GPU time does not prove improved total utilization.

Control target FPS59.88–60.06, P9521.20–22.79ms. Candidate combat59.80/60.14, activation57.85, held59.21/59.44, P9522.74–25.18ms. Neither had target-window frames over33ms. Full-run pipeline creation1036.905ms(control) versus104.618ms(candidate); non-compilation means exclude frames reporting pipeline_ms>0 but do not eliminate all potential cross-frame effects.

The earlier unmatched strip runs also had higher completion times. Despite reduced index payload, current evidence does not support enabling this change by default. Reverted all seven production/test files changed in b308719 to65cf65d versions, preserving rejected-native-strips.patch. Native and Android shared Vulkan rebuilds passed after restoration. No additional repeated correctness tests: restored sources are the already-tested baseline. /tmp/vc3-native-strip-vc3 is now REJECTED; do not package it. Current build-vulkan-direct/vc3 uses restored list topology. The cache remains data usable by either version.

No claim that strips are universally slower or unsuitable for Quest. Revisit only with evidence addressing end-to-end completion/pacing, including target hardware when permitted. Next priority is separating queue/upload/completion latency rather than optimizing only the measured render-batch interval. No headset access, APK, push or goal completion.
