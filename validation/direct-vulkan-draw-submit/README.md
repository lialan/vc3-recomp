# Shared draw-submission overhead reduction

- Combine up to five descriptor writes into one vkUpdateDescriptorSets call per draw. Preserve distinct per-draw descriptor sets and snapshot lifetimes; image and buffer info arrays stay live through the call.
- Flush only the newly written geometry range on non-coherent allocations, rounding the start down and end up to nonCoherentAtomSize. Whole-allocation tail fallback is retained. Cache that immutable device limit at target creation instead of querying it for every draw.
- No guest clock, pacing target, GPU completion, or resource lifetime changes.

Native VC3 and Android xbox_kernel/chihiro_rt/chihiro_vulkan builds passed. Texture-coherence and depth/readback fixtures passed with synchronization validation enabled; logs contain no VUID or synchronization hazard. Local hardware may select coherent geometry memory, so the non-coherent range change is not claimed as an exercised hardware speedup.

One local 2,100-frame gameplay run completed successfully with automated combat and slow-motion inputs. report.json records the executable hash, environment and frame windows. Cache input was copied from the fence-event experiment cache (71,705,540 bytes); production fence code was restored and rebuilt. This is a smoke/performance observation, not a matched A/B speedup measurement. No new captures were requested. CPU geometry, strict depth, resident-depth conversion and color conversion counters remained zero; CPU texture fallback remained the startup 1,030,144 pixels.

No headset access, APK installation, or Quest performance claim. The unrelated host_input.c working-tree changes were present in the local executable and are excluded from this commit. The goal of smoother frame pacing remains open.
