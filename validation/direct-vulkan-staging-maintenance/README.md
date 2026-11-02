# Staging cache maintenance

Shared Vulkan targets cache staging allocation coherence and size. Host-coherent allocations skip redundant flush/invalidate API calls. Noncoherent allocations maintain only the CPU-accessed color, packed depth, or unpacked depth/stencil region, rounded outward to device cache atoms and capped at the allocation end. Whole allocations remain mapped. GPU barriers and completion/staging reuse fences are unchanged.

Native VC3 and Android chihiro_vulkan builds passed. Texture/depth exact-byte fixtures passed with synchronization validation in default mode and CHIHIRO_VULKAN_FORCE_STAGING_MAINTENANCE=1 mode. The latter exercises explicit aligned ranges on coherent development hardware, not physical noncoherent cache behavior. Existing fixtures cover odd dimensions, D32/D24, linear/Morton, staging reuse, combined readbacks, and CPU edits. No validation errors or SYNC-HAZARD messages. Actual noncoherent hardware validation remains outstanding.

One local 2100-frame run /tmp/vc3-staging-maintenance, binary /tmp/vc3-staging-maintenance-vc3, exit 0. Seed copied from preceding attachment-writes run. Existing unrelated host_input.c changes are in the local binary but excluded from this commit. No headset/APK/push. Not a matched A/B performance claim.

Final live-target counter: 38253 coherent maintenance calls skipped; zero explicit maintenance bytes on this device. This counts removed API calls, not physical bandwidth saved. Combat59.97/60.03FPS, activation59.97, held59.91/60.07. These windows p9521.29–22.20ms. Noncompile completion combat/activation/held5.167/5.103/5.242ms and CPU1.642/1.795/1.814ms. No demonstrated completion or pacing gain. CPU geometry/depth/color conversion counters zero, texture fallback1030144pixels/6.332ms cumulative. Goal remains active.

Next inspected opportunity: guest_write_watch.c visit_write currently invokes protect_pages for watched pages even if already writable. Preserve leases/generation changes and fault/alias semantics when evaluating redundant syscall removal.
