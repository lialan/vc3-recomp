# Alpha-test shader split and command-batch timing

The OpenGL reference has separate fragment programs without alpha testing.
Its source explains that a discard-capable shader can prevent early depth/tile
rejection even when the alpha-test branch is disabled. Native Vulkan now uses
the same split for plain, textured, register-combiner and overlay-effect shaders.
All variants compile the existing shared fragment source; enabled alpha testing
retains its original comparison and discard behavior. Pipeline keys distinguish
the two modules. The split remains **off by default**: the initial Quest pair
regressed startup and early scenes. `CHIHIRO_VULKAN_NOAT=1` or the Quest runner's
`--no-alpha-shader` enables the experiment. Generic modules remain the default.

The SPIR-V audit confirms one OpKill in each generic fragment module and none
in its alpha-free counterpart. A real Quest GPU test compares every color,
depth and stencil byte, plus precise visibility counts, across disabled,
rejected, accepted and disabled-again alpha-test transitions. It also verifies
distinct pipeline keys and reuse. The complete Quest GPU, deferred submission,
guest layout and injected-XR suites pass. The Mac guest startup smoke test
reaches four frames without OpenGL imports. The refreshed APK passes the
OpenGL import/dependency audit; it has not been installed.

Profiling now includes command batches. GPU timestamps bracket the commands,
and results are retrieved after the existing completion fence. Host recording,
submission, fence wait and sample retirement are counted separately. This adds
no completion wait. The fixture verifies all 130 synchronous draws and all
three batches containing 130 draws are counted exactly. These diagnostics do
not cover separate color/depth transfer submissions or the XR compositor.
`CHIHIRO_VULKAN_GPU_TIMESTAMPS=0` (runner `--no-gpu-timestamps`) disables GPU
queries while retaining CPU diagnostics and guest-completion intervals, so the
measurement overhead can be checked independently.

## Same-binary combat experiment

Both modes reached 2160 guest frames with the same write tracking, 512MiB cache,
autokeys, capture frame and instrumented executable. This is one pair with
guest-timer/cache/driver variability, not a statistical performance acceptance.

| Window | Alpha-free FPS / p99 | Generic FPS / p99 |
| --- | --- | --- |
| 121..241 | 31.13 / 42.87ms | 44.24 / 28.15ms |
| 601..721 | 16.70 / 72.65ms | 20.23 / 58.10ms |
| 1681..1801 | 6.42 / 175.96ms | 6.22 / 296.51ms |
| 1921..2041 | 5.04 / 293.12ms | 4.86 / 305.27ms |

The split does not solve combat performance. The initial earlier-window
regression was not reproduced by the warm startup controls below, so it cannot
be attributed conclusively to the shader change. No verified benefit justifies
enabling it by default. Main-target reports now expose hundreds of
milliseconds of batch GPU work in approximately one-second reporting windows;
the old synchronous-only GPU logs excluded that work. Most fixed-function
combat draws still use the general register-combiner fragment shader. Unlike
the reference GL plain-shader selection, Vulkan currently restricts its exact
modulation reduction to decoded geometry with constant projective q. This is a
concrete next investigation, not a reason to drop combiner or projective
semantics. Actual game captures also have different timers/rotating HUD state;
they are not asserted byte-identical. Exact shader transition equivalence is
established by the controlled GPU fixture above.

Four warm 360-frame startup runs then shared one uninterrupted device lock.
They used the final same binary, no autokeys/captures, and the same 512MiB cache
and write tracking. The 121..241 completion window measured:

| Shader | GPU timestamps | FPS | p99 |
| --- | --- | ---: | ---: |
| Generic | on | 44.85 | 30.46ms |
| Generic | off | 45.69 | 27.56ms |
| Alpha-free | off | 45.41 | 27.85ms |
| Alpha-free | on | 45.47 | 28.33ms |

This small check does not reproduce the initial startup regression and shows
no meaningful alpha-free gain. It also checks the timestamp-disabled path,
including absence of GPU-query logs. The generic timestamp pair differs by
about 2% in this startup window; combat measurement overhead remains unbounded.
The complete Quest fixture suite passes again with the final generic default.

## Cache control before this change

The preceding write-watch binary was run on Quest for 2160 frames with a
512MiB cache, compared with the earlier 64MiB diagnostic. Uploads/evictions fell
from 14,362/14,246 to 2,208/1,856. The larger cache retained 352 entries at
509.5MiB. Combat still measured 6.34 FPS over flips 1681..1801, with p99
175.00ms. The 1921..2041 window measured 5.04 FPS and p99 293.20ms. This ruled
out cache capacity as a sufficient fix and motivated measuring batched GPU
work before choosing the next optimization.

The 512MiB recording contains 26,369 user CPU samples. Its self percentages
include memcpy 11.78%, memcmp 9.81%, texture decoding 6.90%, fixed-function
preparation 4.37%, color row conversion 3.95% and depth transfer conversion
3.76%. These are whole-recording CPU percentages, not frame-wall-time or GPU
percentages, and do not establish a same-window speedup over the earlier run.

All Quest sessions use the exclusive whole-session shared lock and private
temporary files. The original OpenGL APK hash is verified unchanged. Actual
combat, immersive attachment and sustained display frame pacing remain open.
The saved combat/cache logs retain Vulkan diagnostic lines; full raw guest logs
remain in the original `/tmp/vc3-quest-*` runner directories.
