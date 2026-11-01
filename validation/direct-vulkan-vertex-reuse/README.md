# Indexed programmable vertex reuse

Shared executor commit `8bc09c6` adds a 64-entry, direct-mapped table of prior
vertex occurrences, reset for each draw. An exact index hit copies the already
transformed output instead of fetching attributes and interpreting the program
again. Collisions execute normally and replace that slot. The table is 256
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
