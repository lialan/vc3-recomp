# Shared raw geometry preparation bundle

2026-11-01. Base 695c522. Local Vulkan simulator/headless only; no headset operations.

The backend now retains bounded vertex/index scratch storage across serialized raw draws (16 MiB vertices + 1 MiB indices maximum). Larger index requests use transient owned storage. Existing owned preparation APIs remain available. Prepared geometry must be released before scratch reuse; backend destruction releases retained buffers. Guest snapshots and GPU upload lifetimes are unchanged.

Attribute format and complete stream bounds are validated once per used attribute, instead of once per vertex. Fixed vertices are fully initialized without redundant calloc; program inputs preserve zeroed unused slots. Encoded guest bytes are still copied on CPU; numeric unpacking, transforms and vertex program arithmetic remain on GPU. These shared runtime changes have no game-specific addresses or assumptions.

Validation: native build and one focused KosmicKrisp synchronization-validation batch passed. Program fixture adds 120 snapshot cases spanning all supported formats/sizes, zero stride, constants, growth/shrink, strip/fan topology and invalid-span recovery; existing fixed/program rendering and texture/coherence fixtures pass. No VUID or synchronization hazards in saved logs. No repeated cross-driver/Android matrix.

One sequential 2100-frame comparison uses identical populated cache copies and frame-only diagnostics. Dirty depth pooling remains explicitly enabled, QoS unchanged, pacing unset. Both runs exit zero and reach frame limit. Reports retain binary hashes and complete environment; raw logs remain in /tmp/vc3-raw-scratch-{control,candidate}/game.log.

| Scene | Control FPS | Candidate FPS | Control p95 ms | Candidate p95 ms |
| --- | ---: | ---: | ---: | ---: |
| Combat early | 60.11 | 60.03 | 22.10 | 21.53 |
| Combat late | 60.00 | 60.01 | 22.71 | 21.86 |
| Slow-motion activation | 57.93 | 59.33 | 21.08 | 21.25 |
| Held early | 58.65 | 60.42 | 21.61 | 20.81 |
| Held late | 60.25 | 58.15 | 23.06 | 23.37 |

Results are mixed, not proof of a throughput improvement: activation improves but later held slow motion regresses. Structural allocation/validation reductions are retained; warm steady 60 FPS remains unproven. These are guest completion intervals, not display cadence. No exclusive machine/GPU claim. Detailed CPU graphics audits were disabled for this low-overhead comparison; earlier GPU-offload evidence remains, but this run does not provide new zero-counter proof.

Next investigate remaining submission/coherence and completion scheduling costs; preserve the requested bundled-fix/focused-validation cadence rather than repeating benchmark matrices for this change.
