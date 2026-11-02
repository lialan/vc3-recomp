# Consecutive pipeline lookup

Shared target lookup checks the previously selected pipeline with the complete key before scanning the bounded cache. Full-key validation, LRU accounting, eviction fences, descriptor updates and command recording remain unchanged. Failed fast lookup avoids comparing the same previous entry twice during fallback, but still includes it in eviction selection.

Native VC3 and Android shared Vulkan builds passed. Existing texture/coherence GPU fixture passed with synchronization validation; it exercises repeated state, state changes, multiple shader/depth modes and resource retirement. No new standalone mirrored lookup test. No validation errors/SYNC-HAZARD.

One local2100-frame run /tmp/vc3-pipeline-lookup, executable /tmp/vc3-pipeline-lookup-vc3, exit0. Cache copied from preceding completion-overlap candidate.291093 consecutive hits bypassed scans;2818639 fallback entries scanned on live targets. This is a count of avoided scans, not a measured speedup. No matched A/B. Combat59.97/59.95FPS,activation59.95,held59.97/60.01; p9521.29–22.26ms. Noncompile drawCPU combat/activation/held1.813/3.020/2.892ms, completion4.686/4.686/4.751ms. No causal FPS/pacing gain claimed. CPUgeometry/depth/colorzero; startup texturefallback1030144pixels/6.493ms cumulative.

No headset/APK/push. Local executable includes unrelated preexisting host_input.c edits, excluded from commit. Goal remains active; frame pacing and real Quest performance remain unproven.
