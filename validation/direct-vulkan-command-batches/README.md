# Bounded native command batches

Resident ARGB8888 targets now record up to 64 eligible draws per submission.
Each draw copies its geometry, fixed-function state and combiner uniforms into
an aligned arena slice and receives an immutable descriptor set. Pipeline LRU
eviction and arena growth finish pending work first. Cache pins and owned texture
uploads remain alive through real GPU completion. Bare borrowed texture images
retain the synchronous lifetime contract.

Queries, clears, explicit color/depth transfers, presentation export, target
destruction and guest completion finish pending work. A pending target exposes
no completion fence, so an earlier signaled fence cannot release a guest wait.
The guest backend finishes targets before publishing CPU-visible results.
Resource reuse and resident color are prerequisites. Other callers remain
synchronous unless they explicitly opt in before creating a pipeline.
`CHIHIRO_VULKAN_BATCH_DRAWS=0` supplies the diagnostic control; the native runtime
enables batching by default. RGB565 remains strict. Depth residency retains its
separate opt-in and D24 format requirement.

The actual Quest GPU suite passes, including exact accumulated RGBA output,
changing vertex buffers, arena growth with pending draws, descriptor snapshots,
same-VA texture edits, pinned-cache pressure and owned fallback uploads,
queries, LRU retirement, CPU color edits, depth transfers, raw fixed-function
state snapshots and guest semaphore/report publication. The warmed synthetic
130-draw test reports 28.35 ms synchronously versus 1.52 ms with three batched
submissions. These are fixture timings, not game FPS or frame-time acceptance.
The session used the shared exclusive lock, cleaned its temporary files and
verified that the original GL APK hash was unchanged.

Two runs of the same local executable both reached frame 240. The frame 180
captures are byte-identical with batching enabled and disabled. They cover
startup only. Mac KosmicKrisp cannot use the required resident D24 target;
strict depth transfers still end most batches (14,753 draws / 14,219 batched
submissions in this trace). This is not evidence of Quest combat performance.
The GPU timestamp log explicitly excludes command batches; live target counters
report batched draw and submission totals separately.

The refreshed APK builds and imports no GL. The build report is not an install
or headed validation claim. Sustained Quest combat FPS, frame-time tails,
immersive scene attachment and full gameplay remain open. The acceptance target
is high combat FPS near 60 with no stutter or uneven frame times; occasional
loading or combat dips are acceptable, but average FPS alone is insufficient.
