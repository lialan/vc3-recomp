# Native guest-memory coherence

The native backend currently compares complete raw color and depth snapshots
before GPU use, merges CPU edits by pixel/component, and publishes pending GPU
writes before guest completion words. This preserves changes outside sampled
hash locations and preserves GPU depth when the CPU changes only stencil.

The OpenGL renderer uses a different policy. `tex_bind` checks a sampled hash
when a framebuffer is sampled, and samples its existing GPU framebuffer when
the CPU has not taken ownership. Its Quest flip path normally avoids color
readback. Vulkan's exact per-draw RAM comparisons therefore add work that the
OpenGL reference path does not perform.

The 1800-frame actual Quest CPU recording in
`validation/direct-vulkan-idle-coherence` attributes 57.42% of sampled user CPU
in its final 60 seconds to `memcmp`. The leading callchains pass through color
observation before raw/decoded draws (16.15%/9.11%) and depth observation before
those draws (15.09%/8.21%). Completion scans contribute additional work. These
are CPU samples, not percentages of frame time. The recording includes startup
and the bomb battle; the trailing window is selected by sample timestamps.

Idle completion can safely avoid observation when there is no GPU data to
publish. Guest RAM already contains CPU edits, and the next draw/sample/bind
must observe them before GPU use. Color-disabled draws and depth-only clears
also need not import or dirty destination color. Existing GPU writes remain
dirty until published. Host routing notifications retain their existing rules.

## Next structural step: exact write tracking

Removing the remaining full scans needs actual guest-write evidence. Replacing
them with OpenGL's sampled hash would miss edits outside the sampled locations.
A possible implementation is a shared host write tracker, initially limited to
the contiguous-memory allocation and its tiled alias; unsupported mappings
retain exact comparisons. This is a design, not an implemented optimization.

The kernel must enumerate the actual mapped views and their common backing.
Constants alone are insufficient: mirrors can be absent, and contiguous RAM is
separate storage from low guest RAM in this runtime. A per-page generation can
serve multiple overlapping observer ranges. Each observer keeps its own last
generation; one observer must not consume another observer's dirty indication.

To observe, arm read-only protection before reading a snapshot and record the
generation before that read. A subsequent write fault increments the generation
before opening every alias for writes. A write during observation must leave a
new generation for the next observation. Reprotection and fault-driven opening
must serialize, otherwise a delayed fault handler can reopen a page after it was
rearmed and later writes become invisible. Signal-handler state must be
preallocated and use lock-free atomics; protected-memory accesses must not occur
while holding a lock that their own fault handler requires. Unhandled faults
must continue to the existing MMIO/crash handlers.

Kernel copies require special treatment. POSIX `ReadFile` calls `read` directly
into its destination. A read-only destination can produce EFAULT instead of a
user-space write fault. A begin/end write lease around such a syscall must keep
the destination writable for the complete operation, prevent reprotection while
the lease is active, and advance its generation again on completion. All other
syscall destinations must be audited before enabling tracking.

Guest protection changes must invalidate tracking and preserve the requested
permissions. Backend publication must also notify generations, and tracker
shutdown must remove its protections before guest mappings are released. Debug
watchpoints and other protection users require explicit coordination or a
fallback. Never infer that a page stayed unchanged simply because a write fault
was not seen.

Validation must include multiple aliases/subscribers, repeated reprotection,
CPU writes during observation, kernel write leases, protection changes, actual
CPU color/depth merges, GPU completion ordering, and unsupported-layout fallback.
Only after those pass should a same-scenario Quest run establish performance.
