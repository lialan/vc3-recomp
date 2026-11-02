# Independent color completion submission

Color-only targets now submit pending readbacks before depth-owner completion waits. Eligible targets must have resident dirty color, pending work, no dirty depth ownership, and no overlap with any retained depth guest range (existing conservative check includes padding). Routing hooks retain original observation order. CPU color edits are reconciled before submission; if that reconciliation retires the batch, ordinary handling remains. Already-queued colors are not observed/submitted again. Guest color publication remains in original order after depth publication and readback retirement. Guest completion still waits for all targets.

Native VC3/Android shared Vulkan builds pass. Synchronization-validation texture fixture passes exact guest-memory comparisons. Added two cases with independent pending color/depth targets, including CPU color edit reconciliation, across strict and both quantized GPU depth modes. Existing alias, padding, Morton, sample lifetime and resize coverage remains. No validation errors/SYNC-HAZARD. No headset/APK/push.

Sequential identical-seed 2100-frame A/B, both exit0: control /tmp/vc3-staging-maintenance-vc3, candidate /tmp/vc3-completion-overlap-vc3. Paths /tmp/vc3-completion-overlap-{control,candidate}. Seed /tmp/vc3-write-leases.cache (cache data only, not rejected code); hash recorded in comparison.json. Both local binaries include unrelated existing host_input.c edits, excluded from commit.

Candidate4765 early color readbacks. Noncompile completion wall combat/activation/held control->candidate4.547->4.353 /5.431->5.159 /5.137->4.291ms; CPU1.597->1.520 /1.797->1.636 /1.763->1.620ms. Improvement in this one pair; not repeatability proof. GPU timing changed too, so do not attribute the entire wall reduction solely to earlier scheduling.

Control combat59.93/44.68FPS, activation59.89, held60.06/60.03. Candidate60.02/36.17,59.93,59.99/59.91. Both second combat windows include pipeline stalls; these are not fully warm-cache runs. Candidate p9521.45–24.30ms across target windows. No proven pacing or total FPS gain. Goal remains active; Quest presentation/throughput remains untested.
