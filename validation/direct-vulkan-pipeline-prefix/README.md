# Bound pipeline maintenance to initialized slots

Track the initialized prefix of the shared target pipeline table. Descriptor-use reset, pending-state reset and destruction visit this prefix instead of all 128 reserved slots. Lookup visits the prefix plus at most one never-used slot: untouched slots all have used==0, so the original ascending-slot selection would choose the first one. Full-key identity checks, capacity, eviction order and GPU fence semantics remain unchanged. Extend the prefix before creation so partially initialized failed creations remain covered by cleanup. No shader, pipeline identity or guest clock changes.

Native VC3 and Android chihiro_vulkan builds passed. Vulkan program-render and texture-render fixtures passed with synchronization validation, no Validation Error or SYNC-HAZARD. Existing coverage includes program identity, indexed rendering, raw formats, snapshots, pinned sampler collisions, eviction, guest-memory parity and target teardown.

One 2100-frame simulator capture passed, seeded from the preceding indexed-texture cache. Combat 60.02/60.01 FPS; activation 59.89; held slow motion 60.06/60.01. Target-window p95 21.26-22.72 ms. One activation interval exceeds 33.333 ms: frame1563 takes48.372 ms, including17.235 ms pipeline creation within37.078 ms draw wall. These nested times must not be summed. Cache seeding does not establish complete pipeline coverage.

Non-compilation means combat/activation/held: draw CPU1.777/3.074/2.903 ms; draw wall3.644/6.209/5.734 ms; completion wall4.739/4.752/5.013 ms. This is one capture, not a fresh matched comparison; no FPS gain is proven. Runtime loop bounds prove avoidance of untouched-slot visits, but their time savings are not isolated.

The local binary includes unrelated preexisting host_input.c edits, excluded from commit. No headset access, APK installation or push. Local guest completion is not Quest/XR presentation evidence. Overall performance and cache coverage remain open.
