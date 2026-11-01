# Idle target coherence

Completion now observes and publishes color only when GPU color writes are
pending. A clean target's guest RAM already contains CPU edits; its next GPU
use still observes them. Depth completion likewise returns immediately for an
idle owner. Color-disabled draws and depth-only clears do not import or mark
destination color dirty. Pending writes remain dirty, and routing hooks retain
their previous notification behavior.

New fixtures cover CPU color edits across idle completion, color-disabled draw
and depth-only clear before a partial-channel GPU write. Both edits and GPU
neighbors survive. A depth fixture verifies idle completion performs no merge
or writeback, then a shared-target bind imports the CPU depth/stencil edit.
Packed and float controls pass. The actual Quest renderer, deferred-completion,
guest-layout and injected-XR suites pass; their logs are retained here. Mac and
Android builds and the refreshed no-OpenGL APK audit pass. No APK was installed.

Two actual-guest Quest recordings use the same O2/A78 settings, 64MiB cache,
flip-clock input script, 1800-frame gate, no captures, and scoped user CPU
sampling at199Hz. Baseline/candidate finish in226.783/223.676 recorded seconds,
with29649/28958 samples and none lost. Guest timer phases and draw counts
differ; these are diagnostic profiles rather than synchronized scene timings.
The last complete120-frame window is5.54/5.65FPS with p99215.01/244.70ms.
Neither run meets combat throughput or pacing requirements.

The baseline's final60 seconds attribute57.42% of sampled user CPU to memcmp.
Its color-completion callchains contribute3.01% at semaphore completion and
1.14% at flip. In the candidate the leading completion chain is0.66%; per-draw
color and depth comparisons still dominate. The candidate logs zero
color-disabled draws in the combat windows, so that optimization has no measured
combat contribution here. Exact CPU-write tracking is the next structural step;
`docs/vulkan-coherence.md` records its alias, syscall, protection and concurrency
requirements. It has not yet been implemented.

`tools/analyze-vulkan-simpleperf.py` reproduces the trailing-window analysis
using sample timestamps and the recording's exact libmain hash. A mismatched
build is rejected before symbol setup. Raw recordings remain in the ignored
build profile tree and original runner outputs; JSON summaries and text reports
are committed here. These percentages describe user CPU samples, not GPU/frame
time. Both whole-session locked tests cleaned their UUIDs and verified the
original OpenGL APK unchanged. Full immersive verification remains open.
