# Resident Quest overlay and shared display cadence

The native Quest gun overlay now enables bounded command batching and defers
color readback. Gun, glass/reticle, laser, dot and flash passes record into the
resident eye target; the final swapchain export finishes GPU work before release.
CPU mesh/state buffers may be freed after recording because the target copies
them into its owned arena. Per-eye target resources persist between frames.

The OpenGL and Vulkan hosts share the same overlay reuse predicate: at refresh
rates above 83 Hz, reuse every other frame unless full-rate mode or the menu is
active. Reuse retains the image's original projection pose. Invalid gun/view
state and frames where the runtime requests no rendering invalidate reuse.
`OVERLAY_FULLRATE=1` retains the full-rate diagnostic option.

On Quest Adreno, all 48 overlay cases match the synchronous Vulkan images
byte-for-byte, with all RGBA pixels within three UNORM steps of the actual GLES
reference. The new fixture asserts that intermediate calls do not touch the CPU
output buffer, pending draws expose no completed fence, and 132 draws finish in
48 submissions using one geometry allocation. All other standalone rendering,
coherence, fixed-function, deferred guest completion and injected-XR checks pass.
The complete session used the exclusive shared lock, cleaned its artifacts and
verified the original GL APK hash was unchanged.

Both Quest OpenGL and native Vulkan builds pass. The native APK import audit
contains no OpenGL imports. This checkpoint has not been installed or verified
in a headed session, and does not prove combat FPS or frame-time acceptance.
Headed validation remains blocked by the observed Guardian system dialog;
no boundary bypass was attempted. Full native immersive scene attachment and
Quest combat validation remain open.
