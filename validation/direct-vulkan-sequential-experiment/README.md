# Rejected sequential-index experiment

Tried detecting identity indices during mandatory index validation, skipping their upload/binding and using vkCmdDraw in both batched and synchronous paths. Exact GPU tests compared identity and cyclic index arrays, color and Z24S8 output, invalid-index rejection, and counters for synchronous/deferred draws. Native/Android builds and texture/depth synchronization validation passed.

Local /tmp/vc3-sequential reached2100frames,exit0. Only38 live-target draws qualified, avoiding40128bytes of index uploads. This does not justify adding identity checks to all indices in this workload. Reverted all production/test edits, preserving rejected.patch for diagnosis; rebuilt nativeVC3 and Android sharedVulkan after reverting. /tmp/vc3-sequential-vc3 is EXPERIMENTAL and must not be packaged. Current build sources equal e61468f except unrelated preexisting host_input.c changes.

Run target FPS: combat59.98/53.75, activation58.93, held59.94/60.04. P9520.95–21.58ms. Frames1542/1581 were279.459/75.069ms with248.687/57.632ms pipeline creation. All-run pipeline creation431.096ms. Not matched A/B; no FPS regression/gain attribution. Run used a copy of dynamic-stencil cache; changed draw type can affect driver compilation. CPU geometry0; CPU texture conversion6.445ms cumulative. Raw topology preparation in vk_fixed.c expands strips/fans to triangle lists; native topology preservation is a more promising next investigation than identity indices.

No headset access, APK, push or goal completion. This checkpoint records rejected work and evidence only.
