# Morton arithmetic retained; depth lane reorder rejected

Retained shared ch_morton_linear_index helper replaces dynamic i%width and i/width with mask and shift in color/depth packing. Full rectangular power-of-two layouts are required and validated by callers. Other users of ch_morton_index remain unchanged. Final SPIR-V has no OpUMod; color has no OpUDiv, depth only division by constant4 for packed stencil offset. This is instruction simplification, not an isolated measured FPS gain.

Tested bundle also assigned adjacent depth readback pixels to neighboring lanes, reloading read-only stencil words across four lanes; uploads kept single writers for stencil words. Exact texture/depth fixtures and synchronization validation passed. Native VC3/Android shared builds regenerated embedded shaders and passed.

Sequential identical-seed2100frame A/B bothexit0: control /tmp/vc3-color-coalescing-vc3, bundled candidate /tmp/vc3-depth-coalescing-vc3; runs /tmp/vc3-depth-coalescing-{control,candidate}. Seed copies /tmp/vc3-color-coalescing-candidate.cache. Noncompile completion combat/activation/held4.642->4.979 /4.714->5.007 /4.775->4.972ms; GPUbatch3.297->3.454 /3.892->4.053 /3.743->3.848ms. CPUdraw also increased, so this onepair is not isolated proof of shader regression; it provides no benefit evidence. Rejected the depth lane reordering instead of keeping it speculatively. Candidate archived patch is NOT final code; /tmp/vc3-depth-coalescing-vc3 must not be packaged.

Final code retains previous depth grouping plus the new Morton helper in both packing shaders. Rebuilt native/Android targets and reran full depth/color exact-byte fixture with synchronization validation successfully. No final gameplay rerun or standalone FPS gain claim. The A/B results describe the rejected bundle only.

No headset/APK/push. Local binaries contain unrelated existing host_input.c edits, excluded from commit. Updated packing shader identities require refreshed Quest driver cache when device access resumes; current builds embed SPIR-V only. Goal active; avoid assuming color coalescing benefits transfer unchanged to depth/stencil packing.
