# Morton addressing and first native combat capture

The guest surface adapter now computes rectangular Morton offsets using the
shared bit-spreading helper, with the larger dimension's remaining bits appended
after the interleaved portion. It replaces two variable-length deposit loops
per pixel without changing coherence, guest bounds or depth/color arithmetic.

The CPU test compares against the shared mask-deposit reference for every
supported exponent pair (0 through 15 on each axis), both pixel sizes, full and
clipped rectangles. It exhausts smaller allocations and checks deterministic
edge/random coordinates in larger ones. Mac UBSan and the actual Quest renderer,
deferred completion, guest layout and injected-XR suites pass.

A Quest address-only microbenchmark sums identical offsets for 64 passes over
a 640x480 clip in a 1024x512 allocation. Runtime-loaded format fields prevent
constant-folding the deposit masks. The new helper took 52.400/52.355ms and the
generic reference 481.952/445.217ms; all checksums were 13330465423360. This is
only address arithmetic, not a gameplay speedup. The benchmark source is retained
as `address-benchmark.c`.

The separate 300-second actual-guest diagnostic used the immutable preceding
fast-color build, not this addressing change. It requested 2500 frames and was
killed by its watchdog after the last reported frame 2040. Periodic captures
prove it reached the bomb battle: frame1920 shows its HUD/countdown, ammunition
and warehouse scene, consistent in broad content with the GL reference at
source frame2400. Inputs share a flip clock but guest timers and phases differ;
these are not synchronized images or pixel-parity acceptance.

The last measured 120-frame interval was 3.20FPS, mean312.23ms, p99613.09ms,
max629.03ms. Texture cache totals at2040 were7358 uploads/7175 evictions with
63.8MiB live in183 entries of2048. This suggests byte-budget thrashing, to be
tested separately. Capture I/O also affects timings. The combat performance
requirement is plainly unmet; immersive XR and sustained pacing remain open.

Both Quest sessions used the whole-session shared lock and cleaned their UUID
files. The original OpenGL APK hash remained unchanged. No app was installed.
`quest-periodic.log` retains the boot header and relevant Vulkan frame/cache/
draw/capture rows; the full runner log remains at the report's local output.
