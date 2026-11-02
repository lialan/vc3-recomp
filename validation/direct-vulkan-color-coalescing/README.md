# Coalesced color-pack accesses

Shared compute color packing keeps64invocations/workgroup and fourpixels/invocation, but each loop iteration assigns consecutive pixels to neighboring lanes instead of stride4. Dispatch size/count, storage formats, arithmetic, barriers and CPU work unchanged. Linear conversion remains in-place with unique per-pixel ownership. Morton input/output uses separate regions. No workgroup-size requirement increase or CPU graphics fallback.

Native VC3 and Android shared Vulkan builds regenerated embedded SPIR-V and passed. Existing texture/depth exact-byte fixtures passed under synchronization validation, including odd dimensions, linear/Morton, rectangular Morton, uploads/readbacks, staging reuse and paired depth/color. No validation errors or SYNC-HAZARD. No extra regression fixture needed for this assignment change.

Sequential same-seed2100frame A/B, bothexit0. Control /tmp/vc3-pipeline-lookup-vc3; candidate /tmp/vc3-color-coalescing-vc3. Runs /tmp/vc3-color-coalescing-{control,candidate}. Seed /tmp/vc3-multiwait-probe.cache is cache data only, not the removed diagnostic code; hash recorded. Both local binaries include unrelated existing host_input.c edits, excluded from commit.

Noncompile completion wall combat/activation/held control->candidate4.998->4.592 /5.161->4.654 /5.111->4.575ms. GPUbatch3.481->3.348 /4.238->3.854 /3.915->3.632ms. Onepair only; scene/workload and compilation-frame counts varied, so repeatability and isolated shader throughput unproven. GPU batch time includes more than packing and excludes separate depth uploads.

Candidate combat59.97/59.98FPS,activation59.89,held60.07/60.03;p9520.68–21.57ms. Control last heldwindow52.89FPS/max212.18ms includes compilation; do not attribute that FPS difference to coalescing. Pacing remains above16.67ms tails. No Quest performance claim.

No headset/APK/push. This changes shader identity: existing Quest driver-cache seed does not precompile the new color-pack pipeline. Refresh real Quest cache when device access resumes; current Android library includes compiled SPIR-V, not new device driver binaries. Goal remains active.
