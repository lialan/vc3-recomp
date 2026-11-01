# Combined batch depth readback

Depth/stencil reads with pending draws now append the image-to-buffer copy
and host barrier after the render pass in the same command buffer. The
existing real fence still completes before CPU mapping/invalidation and
publication. The separate transfer submission is retained when no batch is
pending, and uploads retain their existing ordering and conversion rules.
The common transfer recorder preserves both image aspects and restores the
depth/stencil attachment layout. Batch GPU timing includes the appended copy.

A focused GPU regression compares exact D32 float/stencil output against the
synchronous path after a draw writes both depth and stencil. It asserts one
combined readback and one batch submission. Specialized-combiner and renderer
snapshot checks pass on Apple M4 Pro/KosmicKrisp. Mac and Android offscreen
builds pass. Packed native D24 runtime verification remains unperformed here.

The earlier same-binary DMA-walker control /tmp/vc3-dma-walk-profile reached
frame 1200; its capture visibly shows the mission intro (PRESS START TO SKIP),
not combat. Window 1081..1201 was 1.32 FPS, p99 2889.42 ms. That run predates
this change and remains a baseline, not evidence of an optimization speedup.
No headset access or Quest performance acceptance occurred.
