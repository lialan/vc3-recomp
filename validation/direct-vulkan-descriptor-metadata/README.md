# Rejected cached descriptor-write metadata

Attempted to construct VkWriteDescriptorSet metadata once per pipeline and retain host image/buffer payload arrays there. Per-draw payloads and dstSet still updated, with distinct descriptor sets and unchanged GPU lifetimes. Runtime requests Vulkan1.0, so native Vulkan1.1/KHR descriptor-update templates were not silently introduced; that would require an explicit capability path. No API or shader change in this experiment.

Native/Android builds and program-render, texture-render, depth-pack GPU fixtures passed synchronization validation. Sequential same-seed local control /tmp/vc3-depth-copy-profile-vc3 versus candidate /tmp/vc3-descriptor-metadata-vc3, identical environment except output/cache/save/config paths, both2100framesexit0. Seed checksum recorded. Detailed original reports include executable hashes. Existing unrelated host_input.c edits are in both binaries.

Non-compilation mean draw CPU control->candidate: combat1.905->1.894ms, activation3.184->3.089ms, held2.998->3.027ms. Normalized by batched draw counts:5.962->5.927 /8.591->8.332 /7.987->8.068microseconds. This includes other draw CPU work, not isolated descriptor overhead. No consistent gain across scenes; per-pipeline storage grows by write/payload arrays. Candidate target FPS59.79–60.15 and P9520.52–23.00ms, control59.82–60.18 and P9522.08–23.52ms. GPU and completion timings also vary, so no causal pacing or FPS improvement claim from one pair.

Reverted the production edit and rebuilt nativeVC3 and Android sharedVulkan. rejected.patch is diagnosis evidence only. /tmp/vc3-descriptor-metadata-vc3 is REJECTED and must not be packaged. Current runtime remains59c5960 behavior. No repeated correctness tests after byte-for-byte restoration of tested source.

Next higher-value work remains depth-copy overlap with explicit source/destination lifetime and command-reuse ownership, as detailed in direct-vulkan-depth-copy-timing. Do not substitute descriptor bookkeeping for that synchronization problem. No headset/APK/push; goal remains active.
