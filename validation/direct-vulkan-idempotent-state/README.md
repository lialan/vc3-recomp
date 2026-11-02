# Preserve batches across idempotent target state

Calling ch_vk_target_depth_quantization(target, 0) previously finished a pending batch even if quantization was already disabled. The shared backend invokes this setter when a non-native target uses a non-Z24 depth format. Repeated calls could therefore force a submission and fence wait between draws. Reapplying the current color layout also finished the batch unnecessarily.

Both no-op cases now return without flushing. Real state changes retain the previous finish path, and failed targets retain their error behavior. No guest timing or completion guarantees change.

The snapshot regression now queues a draw, repeats both no-op setters three times, verifies the draw stays pending and the submission count does not change, then checks the combined texture copy and exact output bytes/mips. Native VC3 and Android shared Vulkan builds passed. The full existing texture-coherence fixture passed with synchronization validation (coherence.log); no validation errors were reported.

No full gameplay rerun for this focused state change. VC3's ordinary Z24 path already avoids the disabled-quantization case and its backend filters unchanged color layouts, so no VC3 FPS improvement is claimed. This is a shared batching fix relevant to other caller/state combinations. No headset access or installed APK changes. Existing host_input.c edits are excluded.
