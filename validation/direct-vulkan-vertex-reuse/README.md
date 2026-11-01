# Indexed programmable vertex reuse

Shared executor commit `8bc09c6` initially added a 64-entry, direct-mapped table of prior
vertex occurrences, reset for each draw. An exact index hit copies the already
transformed output instead of fetching attributes and interpreting the program
again. Collisions execute normally and replace that slot. The initial table was 256
bytes on the stack; transformed vertices remain in the existing batch array.
No game addresses, shader signatures or scene conditions are used.

Only CPU programmable draws without CXT_WRITE are eligible. Context-writing
programs must execute once per index occurrence because they can modify
constants. Fixed-function and backend raw GPU paths retain their behavior.
Attributes, constants, program and viewport are not cached across draws.
Default enabled; `RECOMP_VSH_VERTEX_CACHE=0` provides a diagnostic control.

## Local Vulkan evidence

Two 2100-frame pairs used the identical immutable `/tmp/vc3-vertex-reuse-vc3`
binary, switching only the reuse flag. Each run started with a separate copy
of the same warm pipeline cache. KosmicKrisp, 640x480, 60 FPS cap, sound off,
resident fragment-rounded depth. Every run completed with exit 0 and no
runner failure diagnostics. The second pair reversed candidate/control order.

| Pair | Activation FPS control/reuse | Held 1681..1801 FPS control/reuse | Held 1801..1921 FPS control/reuse |
| --- | --- | --- | --- |
| First | 36.69 / 39.66 | 44.07 / 48.14 | 45.66 / 50.24 |
| Reversed repeat | 36.73 / 39.62 | 43.87 / 48.14 | 46.37 / 50.24 |

Repeated held-window improvements are 9.7% and 8.3%. Repeat p95 frame times
improve 28.26 to 26.04 ms and 24.34 to 22.66 ms. Activation maximum is
32.96/30.11 ms with the already-warm pipeline cache. Normal combat remains
approximately 60 FPS. These are local guest-completion rates; no headset or
XR display performance is claimed. Sustained slow motion remains below 60 FPS.

Full selected windows, binary hash and settings are in
[measurements.json](measurements.json). Source reports and captures are under
`/tmp/vc3-vertex-reuse-{control,reuse}` and `/tmp/vc3-vertex-repeat-{control,reuse}`.
The immutable benchmark binary required explicit flag=1; the final source also
enables the same path when the flag is absent. The default is explicitly tested.
Frame 1740 of the repeated candidate was visually inspected: E.S. HUD, enemies,
bullet effects and scene remain present. This is not whole-game pixel parity.

## Correctness

`external/xboxrecomp/tests/nv2a_vertex_reuse` drives real pushbuffer methods
and observes backend vertices. It checks repeated indices, table collisions,
changed vertex storage between batches, program/constants changes and
context-write multiplicity. A test-only wrapper counts actual interpreter
executions: 10 versus 12 for the collision/reuse sequence, and all 12 when
context writes are enabled. Vertex positions/rhw match explicit expected values.
Enabled, disabled and default cases pass in Release and ASan+UBSan builds:
`/tmp/vc3-vertex-reuse-check`, `/tmp/vc3-vertex-reuse-sanitize`.
The existing fixed-function backend regression also passes at
`/tmp/vc3-vertex-ffp-check`. Mac main and Android offscreen builds pass.
Android validation was build-only; no device interactions.

## Capacity investigation

A diagnostic shadow simulation compared 64 and 256 slots over the same index
streams, without changing which vertices were actually transformed. Reports
emitted between completed frames 1560 and 1920 cover 19,403,596 eligible index
occurrences: 8,026,656 potential hits with 64 slots (41.367%) versus 10,591,648
with 256 slots (54.586%). Reporting intervals are wall-time based and may
straddle the frame bounds. Profiling perturbs timing; its FPS is not benchmark
evidence. `/tmp/vc3-vertex-capacity-profile` completed 2100 frames, exit 0.
The temporary instrumentation is saved as
`/tmp/vc3-vertex-capacity-profile.patch`; no shadow work is retained.

The candidate increases the table to 256 slots (1 KiB stack per draw), retaining
the same lifetime, context-write bypass and exact index checks. The regression
now uses indices 0 and 256 to exercise an actual collision at the new size.
Release and ASan+UBSan enabled/disabled/default tests pass. Mac main and
Android offscreen builds pass; no headset/device activity.

Capacity benchmark pairs, each 2100 frames, same warm-cache starting data and
settings, no shadow instrumentation:

| Pair | Activation FPS 64/256 | Held 1681..1801 FPS 64/256 | Held 1801..1921 FPS 64/256 |
| --- | --- | --- | --- |
| First (256 then 64) | 39.43 / 41.07 | 47.79 / 49.38 | 50.43 / 52.60 |
| Repeat (64 then 256) | 39.68 / 41.03 | 47.70 / 49.38 | 50.16 / 52.25 |

Repeat held throughput improves 3.5% and 4.2%. p95 improves 25.93 to 25.71 ms
and 22.56 to 21.89 ms. Activation maximum worsens 32.64 to 42.91 ms in the
repeat, so there is no blanket claim that all pacing improved. Normal combat
remains about 60 FPS. All runs exit 0 with no runner failure diagnostics.
See [capacity-measurements.json](capacity-measurements.json) for exact binary
hashes, settings and window metrics. The 256-entry table is retained.
Immutable binaries: `/tmp/vc3-vertex256-vc3` and `/tmp/vc3-vertex-reuse-vc3`.
The latter enables the old 64-entry implementation explicitly with flag=1.
No headset/device use; sustained E.S. is still below 60 FPS.
