# Overlap packed depth upload with CPU draw preparation

Batched packed Z24S8 imports now submit through a lazily allocated command buffer/fence separate from the target draw command. The caller's bytes are copied synchronously into owned staging, then the CPU can prepare and record subsequent draws while the queued upload executes. Existing same-queue image/staging barriers remain. This does not add a transfer queue or claim concurrent GPU graphics/transfer execution.

Completion/lifetime rules:
- Staging mapping/access, repeated uploads and standalone finish wait for the upload fence.
- A later completed draw submission on the same queue also proves the earlier upload finished, allowing retirement without an extra fence wait.
- Destruction drains pending work; error paths retain queue-idle cleanup.
- Nonbatched and unpacked uploads keep the synchronous path. CHIHIRO_VULKAN_ASYNC_DEPTH_UPLOADS=0 selects the old behavior for comparisons.
- One extra primary command buffer and fence per target that actually queues an upload; no extra staging allocation.

Native VC3 and Android shared Vulkan builds passed. Both texture-coherence and depth-pack fixtures passed with synchronization validation in synchronous and asynchronous modes. The depth regression now exercises repeated upload/staging reuse, caller-buffer mutation after return, a LESS draw consuming the uploaded far depth, combined color/depth readback, and destruction with an outstanding standalone upload. Logs record exact D32/D24, linear/Morton and alias/CPU-edit checks.

Sequential local game comparison uses one identical executable (/tmp/vc3-async-depth-vc3), toggling only the async upload setting, independent copies of /tmp/vc3-texture-lookup-candidate.cache, identical automated input and 2,100 frames. Profiling scopes overlap; batch GPU timestamps do not include the independent upload command. No headset access, APK installation, or Quest throughput claim. Unrelated host_input.c edits remain present in both runs and excluded from the commit.

Both game runs reached 2,100 frames and exited 0. Candidate reported 2,032 queued depth uploads. Excluding nonzero pipeline-creation frames, dedicated upload waits changed from 0.153/0.134/0.146 ms (combat/activation/held) to below 0.001 ms. Import scopes changed 2.369 -> 2.146, 3.649 -> 3.388, 3.579 -> 3.508 ms. Completion scopes rose 5.145 -> 5.227, 5.068 -> 5.341, 5.526 -> 5.833 ms; some waiting moves later, and one pair cannot establish a net throughput gain. Draw CPU was lower in combat/activation but higher during held slow motion. Candidate windows were 59.93–60.04 FPS with p95 21.50–22.75 ms. Control stalls at frames 1494/1604 contained 83.209/74.043 ms pipeline creation; their absence is not attributed to async uploads. CPU graphics counters remained unchanged (zero geometry/color, startup texture fallback only). See comparison.json for complete counters and timings. No captures were taken in this performance pair; exact fixture output is the correctness evidence.
