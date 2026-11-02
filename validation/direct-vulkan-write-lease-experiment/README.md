# Rejected write-lease syscall experiment

Candidate skips redundant writable protection for already-writable watched page runs, keeping lease/generation updates, page locks, alias protections and invalidation. Added actual nested and repeated lease checks: 128 nested writes through an alias, observer coherence, zero extra protection calls, subsequent rearming and real fault behavior.

Native VC3/Android shared Vulkan builds and guest_write_watch_test passed; Vulkan texture/coherence fixture passed under synchronization validation. Candidate then completed 2100 local frames, exit0, at /tmp/vc3-write-leases; executable /tmp/vc3-write-leases-vc3 is REJECTED and must not be packaged. Local binary includes existing unrelated host_input.c changes.

Protection calls140244 versus prior production run140214, effectively unchanged despite synthetic savings. Workload counts vary slightly; this is not a matched A/B. No significant VC3 benefit demonstrated, so restored both source and test to HEAD and rebuilt native/Android production targets. Patch archived only. Production implementation remains 5f954a8.

FPS windows combat60.00/23.69, activation59.97, held60.00/59.96; combat drop includes frame1479 pipeline1475.405ms and frame1480 pipeline1617.202ms. Do not present this as a fully warm-cache run or attribute those stalls to the page-protection change. Pipeline cache grew from75070268 to84175308 bytes. Other warm windows p9521.40–21.94ms. Noncompile completion combat/activation/held5.449/4.719/4.924ms, CPU1.618/1.645/1.712ms. No net performance conclusion.

No headset/APK/push. Goal active. Avoid further tuning this syscall path without evidence of real gameplay redundancy. Larger remaining completion/readback serialization is the next focus; color-only completions are currently queued after depth-owner readbacks have already retired, but any overlap optimization must preserve depth/color alias and CPU edit reconciliation.
