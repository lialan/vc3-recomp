# VC3 direct Vulkan continuation

Branch: work/vulkan-direct-20261031
Base: 63d8907, the original OpenGL VC3 worktree baseline.

The direct Vulkan module, tests, shared overlay geometry and NV2A interface
extensions were recovered from checkpoint 493ae0b. They had not been committed
on a distinct pre-Zink branch. The Zink implementation and later rendering
diagnostics remain saved on work/vulkan-zink-render-diagnosis-20261031 at db0f4a9.

This branch retains the original OpenGL renderer, Quest host and APK packaging.
Vulkan is optional through CHIHIRO_BUILD_VULKAN and can also be built standalone
from runtime/src/gpu/vulkan. It is not yet wired as a playable game renderer.
The library contains targets, textures, NV2A state/batches, combiners, overlays,
menus, frame submission and OpenXR device/swapchain helpers. Recorded tests
under validation/vulkan-* describe earlier specific binaries, not a new run.

The preserved working APK is validation/baseline/VirtuaCop3VR-working.apk,
SHA256 3a0660db0d8a5d8e6d95a555dc087bd8f9351f2f8df91e601599ae2789b53ad6.
Keep that reference and the installed original package intact.

Continue from the shared OpenGL semantics and Nv2aBackend interface. Complete
native Vulkan game/Quest integration and parity before performance optimization.
Do not use Zink or copy speculative rendering fixes wholesale. The headset is
busy with the user; no device access until they make it available. The owned
read-only q3test simulator on port 5556 was stopped during the switch.

Restoration check: tools/build-vulkan-offscreen.sh compiled the standalone
Android direct Vulkan validation binary successfully. It was not run on a
device during this branch restoration. OpenGL renderer, texture decoder, game
main, Quest host and APK build script match base 63d8907 exactly.
