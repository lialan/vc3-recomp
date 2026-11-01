# Rejected AArch64 color permutation experiment

The retained color-copy inner loop compiled into seven vector arithmetic/bitwise operations per RGBA-to-BGRA permutation. Experimental vqtbl1q_u8 swapped four pixels in one byte-table instruction, with unaligned loads, scalar row tails and unchanged non-AArch64 fallback. This was shared Vulkan code, with no game-specific heuristics.

The expanded real Vulkan fixture passed with synchronization validation: scalar byte oracle across widths 1..65, all 16 pointer alignments, pitched three-row images, full padding and guard checks; plus existing render-target exact guest-memory cases. Log: /tmp/vc3-color-permutation-edges.log. Main macOS executable built. No Android build or headset use for this rejected candidate.

All four runs completed 2100 frames. Apple M4 Pro / KosmicKrisp, warm identical starting caches, mono headless, sound disabled, 60 FPS cap. Two immutable binaries; first pair control then candidate, repeat in reverse order.

| Pair | Variant | Activation FPS | Early held FPS | Later held FPS |
|---|---|---:|---:|---:|
| First | Control | 42.11 | 50.85 | 53.05 |
| First | Candidate | 41.84 | 51.15 | 54.39 |
| Repeat | Control | 42.11 | 51.83 | 53.21 |
| Repeat | Candidate | 42.94 | 51.07 | 53.55 |

No consistent held-window gain: repeat early held p95 also worsened from 24.43 to 25.07 ms. Reverted runtime and candidate-only test edits. Patch preserved at /tmp/vc3-color-permutation-rejected.patch; raw reports /tmp/vc3-color-permutation-{control,candidate}/report.json and /tmp/vc3-color-permutation-repeat-{control,candidate}/report.json. This does not prove byte permutations are generally unprofitable; it does not justify retaining this implementation on current evidence.
