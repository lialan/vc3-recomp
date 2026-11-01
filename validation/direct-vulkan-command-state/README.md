# Cached Vulkan command state

2026-11-01. Base8af01df. Local simulator/headless Vulkan only.

Shared batched draws skip unchanged pipeline binds, blend constants and fragment push constants. Push caching requires identical pipeline-layout identity and bytes. Batch begin resets all cached state; every depth-quantization helper conservatively invalidates it. Descriptor sets, geometry offsets, submission boundaries and lifetimes remain per-draw/unchanged. Resource counters report issued pipeline/blend/push calls. No title-specific logic or new runtime knob.

Native build plus one focused KosmicKrisp sync-validation batch passes: command limits/lifetimes, dynamic blend changes, alpha push changes, quantization-helper restoration (modes1/2), fragment depth (mode5), exact packed depth/color and registered texture coherence.514 constant-state draws at limit256 issue each cached state operation3times rather than514; limit1 still issues514. Mode3 on the ordinary fixture target was correctly rejected because it lacks input-attachment capability; fixture setup corrected to modes1/2. Modes3/4 use the same unconditional invalidation but were not separately exercised this turn. No VUID/SYNC diagnostics; no cross-driver/Android matrix.

One sequential2100frame warm-cache pair, frame-only profiling, thread/frame trace off, ACK_IDLE_US100, equal populated cache copies, dirty pooling enabled, QoS unchanged/flipholdunset. Both exit0/reach limit. Reports preserve flags/binary hashes. Rawlogs /tmp/vc3-command-state-{control,candidate}/game.log.

| Scene | Control FPS | Candidate FPS | Control p95 ms | Candidate p95 ms |
| --- | ---: | ---: | ---: | ---: |
| Combat early | 59.98 | 59.98 | 21.35 | 20.50 |
| Combat late | 59.98 | 59.93 | 21.57 | 20.72 |
| Slow-motion activation | 59.92 | 59.96 | 20.46 | 20.26 |
| Held early | 60.00 | 60.01 | 20.42 | 20.40 |
| Held late | 59.99 | 59.98 | 20.61 | 19.78 |

Fixture call reduction is established; this single game pair does not establish repeatable speedup or steady60. Guest completion intervals are not display cadence. Per-draw counters disabled here, so no whole-game binding-reduction count or fresh CPU-emulation counter claim. No exclusive-machine claim. Next investigate remaining synchronous transfer/completion boundaries with current costs before adding more micro-optimizations. Preserve user preference for bundled fixes and focused validation.
