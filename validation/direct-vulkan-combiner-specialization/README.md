# Native combiner pipeline specialization

Vulkan's general fragment path interprets register-combiner instruction words
from an SSBO for every fragment. The GL reference usually selects a small plain
shader. The new optional path keeps the full combiner arithmetic and supplies
36 immutable instruction words as Vulkan specialization constants. Factors,
final constants, fog, textures and vertex data remain per-draw inputs. No
OpenGL runtime dependency is added.

`CHIHIRO_VULKAN_SPECIALIZE_RC=1`, or the Quest runner's
`--specialize-combiners`, enables it. **The default remains off** while first-use
pipeline creation and combat pacing are unresolved. The program cache is bounded
to 128 pipelines per target when enabled; the generic path retains its 32-slot
bound. Inactive instruction words are canonicalized, and dynamic factor edits
reuse pipelines. C and GLSL specialization offsets come from one scalar layout
with generated compile-time assertions.

The first specialization version retained an interpreter loop. Its actual Quest
run reached 2160 frames in 236.9 runner seconds. The refined version explicitly
calls the same stage arithmetic at eight constant stage indices and compiles
that module with optimization. A known four-stage program shrinks from 245,872
to 5,876 SPIR-V bytes when frozen and optimized offline. This is a compiler
diagnostic, not a claim about actual driver code size or GPU execution time.

Pipeline eviction now drains only when the victim is referenced by pending GPU
commands. A completed victim can be destroyed without flushing unrelated
pending draws. The fixture verifies 140 changing factor payloads across program
LRU eviction produce exact additive pixels in three submissions. Per-target
diagnostics also report total and maximum host pipeline-creation time.

## Correctness checks

The Quest suite passes for 32 combiner cases spanning 0..8 stages, input/output
mappings, mux/dot flags, dynamic factors/fog, normal/cube/missing samplers and
stage-fetch modes. Specialized/interpreter color differs by at most one UNORM
byte, matching the existing CPU-reference tolerance. Visibility counts, depth
and stencil match exactly. Additional cases retain fragment-stage projective q
division in the alpha-free variant, and verify dynamic factors survive batching
and pipeline eviction. Existing GPU, deferred-submission, guest-layout and
injected-XR fixtures also pass at this checkpoint. Later edits only added
pipeline-creation timing and clarified shader selection; no further Quest suite
was run after the user made the headset unavailable.

Mac tests independently pass the full specialized fragment fixture and 256
GPU compute cases against the shared CPU evaluator (2e-5 tolerance). These can
now be built without EGL/OpenGL using `CHIHIRO_VULKAN_SHADER_TESTS=ON`:

```sh
rtk proxy cmake -S runtime/src/gpu/vulkan -B /tmp/vc3-mac-shader-checks -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCHIHIRO_VULKAN_SHADER_TESTS=ON
rtk proxy cmake --build /tmp/vc3-mac-shader-checks --target \
  vulkan_combiner_check vulkan_specialized_combiner_check -j4
rtk proxy env CHIHIRO_COMBINER_SHADER=/tmp/vc3-mac-shader-checks/combiner_test.spv \
  /tmp/vc3-mac-shader-checks/vulkan_combiner_check
rtk proxy /tmp/vc3-mac-shader-checks/vulkan_specialized_combiner_check
```

Set `VK_ICD_FILENAMES` to the desired local Vulkan driver. Recorded Mac checks
use Mesa KosmicKrisp on Apple M4 Pro. They establish local shader behavior,
not Quest or immersive performance. The refreshed APK passes the OpenGL import
audit and remains build-only; specialization is opt-in.

## Actual Quest guest run

The refined run reaches all 2160 frames in 144.443 recorded seconds (146.1
runner seconds including setup/collection), with 18,283 CPU samples and none
lost. It uses scale 1, flat offscreen output, native fixed-function transforms,
exact write tracking and a diagnostic 512MiB texture cache. It records:

| Completion window | FPS | Mean | p99 | Maximum |
| --- | ---: | ---: | ---: | ---: |
| 601..721 | 43.68 | 22.89ms | 30.04ms | 30.80ms |
| 1321..1441 | 14.19 | 70.48ms | 138.71ms | 153.41ms |
| 1681..1801 | 12.57 | 79.54ms | 92.49ms | 94.85ms |
| 1801..1921 | 12.65 | 79.04ms | 88.89ms | 89.77ms |
| 1921..2041 | 8.86 | 112.82ms | 180.27ms | 181.78ms |

This is encouraging relative to earlier roughly 6 FPS combat diagnostics, but
the same-binary generic control was interrupted on the user's request to stop
using the headset. Its locked cleanup completed and the original OpenGL APK
was unchanged. **No completed same-binary A/B speedup is claimed.**

Pacing is still unacceptable. Other windows include a 4.02-second maximum, and
some individual pipeline creations take about 500ms. These measurements do not
attribute every long frame to pipeline creation. In the final 60 seconds,
sampled user CPU self percentages include memcpy 12.07%, memcmp 10.03%, texture
decoding 6.06%, fixed-geometry preparation 5.89%, color-row conversion 3.89%,
depth transfer conversion 3.66% and write tracking 2.91%. Texture-cache lookup
accounts for about 6.2% of CPU samples through its memcmp callchains. These are
CPU sample shares, not percentages of frame wall time or GPU time.

Saved logs retain Vulkan diagnostics. Raw captures/profiles remain in the
original `/tmp/vc3-quest-specialize-*` output directories and immutable binary
snapshots in the ignored build tree. The captured game frame has live timer/HUD
state and is not asserted identical to another unsynchronized game run.

All completed device work held the exclusive whole-session Quest lock and
verified the working OpenGL APK unchanged. No app was installed. Headset access
is now stopped at the user's request; subsequent work is local. Full gameplay,
immersive attachment and steady combat/display frame pacing remain open.
