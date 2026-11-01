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

## Experimental exact write tracking

`CHIHIRO_VULKAN_WRITE_WATCH=1` enables the shared host tracker implemented in
`runtime/src/gpu/guest_write_watch.c`. The default is off. It tracks the actual
contiguous-memory allocation and tiled alias; unsupported mappings/pages retain
exact comparisons. It does not approximate writes with sampled hashes.

The kernel enumerates the actual mapped views and their common backing.
Constants alone are insufficient: mirrors can be absent, and contiguous RAM is
separate storage from low guest RAM in this runtime. Initially writable,
non-executable permissions are checked through host mapping information. Each
observer keeps independent page generations; one observer does not consume
another observer's dirty indication.

Observers arm read-only protection before reading a snapshot. Write faults
advance a generation before opening every alias. Rearming after a writable
interval also advances the generation, so an observer cannot consume a fault's
generation before its store finishes and then miss that store. Ordered page
locks serialize reprotection and fault-driven opening. Adjacent pages are
protected together. Signal-handler state is preallocated and uses lock-free
atomics; it performs no guest-memory accesses, allocation, logging or Vulkan
calls. Host fault context distinguishes writes from read/execute faults.
Unhandled faults continue to the existing MMIO/crash handlers.

POSIX `ReadFile` and `xbox_NtReadFile` call `read` directly into their destination.
Their paired observer hooks hold a write lease across the complete syscall,
including EOF/error paths, and preserve its errno. Observers cannot reprotect a
leased page; completion advances its generation again. The source audit found
these two guest-memory read destinations. Other fread/configuration buffers are
host-owned. GPU color/depth publication also holds a lease, avoiding a fault per
page for the renderer's own writeback.

`VirtualProtect`, recommit, decommit and unmap operations invalidate affected
pages before changing their permissions; those pages retain exact comparison
fallback. Direct protection changes outside the observer hooks are unsupported.
The runtime rejects simultaneous `RECOMP_WATCH`. Shutdown requires stopped guest
callers and drained GPU work, destroys observer ranges, unregisters the hooks,
restores tracker-owned protections and then releases metadata before mappings
are torn down. It preserves permissions imposed by an external protection call.

Tests cover independent subscribers/aliases, 1000 concurrent writable/rearm
cycles, actual write/NX fault decoding, a blocked kernel read lease, protection
invalidation and teardown on Mac and Quest. A real Quest Vulkan fixture checks
CPU color/depth edits through the alias, 100 unchanged draws, EQUAL visibility,
GPU neighbor preservation and exact fallback after a protection change. Platform,
kernel-file and memory-view registry regressions pass on Mac. Actual Quest runs
reach the bomb battle; their evidence is in `validation/direct-vulkan-write-watch`.
Combat throughput, full gameplay and immersive XR acceptance remain open.
