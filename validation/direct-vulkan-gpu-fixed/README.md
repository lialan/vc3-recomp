# Shared fixed-function GPU vertices

The reference GL VS_FF arithmetic and role mapping are now shared headers.
Native Vulkan performs the fixed transforms, skinning, lighting, texgen and fog
in its vertex shader; the CPU only unpacks raw attributes and triangle topology.
Decoded and raw draws share texture resolution, cache pins, surface coherence,
blend/depth/stencil, visibility queries and completion behavior.

C scalar state declarations generate the std430 layout plus C sizeof/offsetof
assertions. Both local Vulkan and OpenXR factories enable shaderClipDistance
and depthClamp when supported. Vulkan defaults RECOMP_GPU_XF to ff and uses the
shared executor CPU fallback for vertex programs, unsupported raw metadata or
GPU transform-check requests. CHIHIRO_VULKAN_GPU_FIXED=0 disables the raw GPU
callback for diagnostics. Host draw/observation hooks retain their CPU path
until raw immersive routing is wired. Native production code imports no GL.

Actual Quest GLES/Vulkan parity uses the GL backend's real VS_FF source, not a
separate reference arithmetic implementation. All48 cases pass within 1 UNORM
step; precise visibility matches coverage exactly. Cases include all skin
modes, lighting/materials, four-stage texgen/matrices, fog, invalid w and w/near
clipping. The registered raw callback test covers rebased strips/fans, exact
query 256, resident color publication and CPU decline before unsupported data
can draw. Other GPU/coherence/XR ownership checks also pass. Test sessions use
the whole-session shared lock and retain the original GL APK hash.

The first game run correctly exposed a startup override: the native backend
reported enabled, but main still forced RECOMP_GPU_XF=0 and no raw batches were
offered. After fixing that override, the enabled game run records 75,426 native
fixed batches by frame 1,380, zero backend declines and no rejected draw before
its 300-second timeout. It does not reach the requested 1,600-frame gate. The
frame 1,250 capture is still the title cinematic, not verified combat. Timed
inputs/workload differ between these runs; no speedup or pixel parity claim
is made from their frame counts. Reports identify the actual binaries.

The APK build/import audit passes and is not a full-game installation claim.
GPU vertex programs, immersive host routing, batching/transfers, full gameplay
and Quest combat/frame-time acceptance remain open.
