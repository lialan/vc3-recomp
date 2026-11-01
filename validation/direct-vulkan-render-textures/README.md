# GPU render-target texture snapshots

The OpenGL reference samples an existing framebuffer texture. The native
Vulkan backend now has an optional GPU path for exactly matching ARGB8888
render-target bindings, avoiding guest RAM publication/decoding at that sample.
It is enabled with `CHIHIRO_VULKAN_GPU_RENDER_TEXTURES=1` or the Quest runner's
`--gpu-render-textures`. **Default is off; Quest validation remains pending.**

Snapshots use the existing texture cache, sampler keys, byte/entry limits and
completion-gated pins. Color writes and actual CPU pixel imports advance a
content serial. A source change cannot overwrite a pinned older snapshot. A
completed snapshot image can be refreshed without reallocating its image,
sampler, command pool or fence. CPU readback staging is allocated lazily for
GPU-created textures, with its size still reserved in the cache budget.

Full pitched ARGB8888 and exact full-allocation Morton views are eligible.
Subregions, XRGB/other reinterpretations, different layouts and cache pressure
retain the existing decoder path. Fallback publishes pending GPU source data
before a CPU cache lookup or decode. Guest fences/flip still publish dirty
color and depth; no guest-memory completion semantics are bypassed. Mips use
the same GPU generation code as CPU texture uploads.

## Local checkpoint

The focused Vulkan fixture passes on Apple M4 Pro/Mesa KosmicKrisp for all byte
values, orientation and generated mips; version/sampler keys; pin-budget
rejection; completed-image reuse; and destroying a source before pending
sampling completes. Registered renderer callback scenarios compare every guest
memory byte against the strict path for pitched/Morton targets, CPU edits,
self-sampling, resize, sampler changes and format/subview/budget/pin fallback.
The headless driver injects executor registration only and uses an isolated
scalar ARGB decoder; the renderer, cache, Vulkan copies and GPU execution are
real. The full test target instead uses the existing actual executor linkage.
The existing specialized-combiner sampler/cube regression also passed after
the shared texture-allocation refactor. Broader GPU tests were not repeated
after each edit.

Build the focused fixture with `CHIHIRO_VULKAN_SHADER_TESTS=ON`, then run
`vulkan_render_texture_check` using the desired local Vulkan ICD. No EGL or
OpenGL context is needed. Android test binaries and the APK build/import audit
also pass; these are build-only, not headset verification or installation.

Both same-binary Mac guest runs reach 720 startup/loading frames without OpenGL imports.
The enabled run makes 352 GPU snapshots, refreshes 348 existing images, and
reduces CPU texture uploads from 25 to 17. Estimated texture-cache usage falls
from 21.2MiB/25 entries to 17.2MiB/21 entries. There is no meaningful measured
frame-rate gain in this local run: the 481..601 window is 14.69 FPS off versus
14.74 FPS on. Mac depth uses the non-native-D24 path and this guest run does
not use native fixed-function vertex transforms, so it is not representative
of Quest performance or combat. Frame 640 shows the file-loading screen, and
the unsynchronized captures differ by 2114 pixels; byte-identical game output
is not claimed. Raw logs/captures remain in the original
`/tmp/vc3-render-texture-mac-reuse-{off,on}` directories.

All work in this checkpoint is local. The headset remains unavailable and no
device commands, app installations or headset tests were performed. Full
gameplay, immersive rendering, Quest combat throughput and frame pacing remain
open.
