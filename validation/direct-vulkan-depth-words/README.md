# Linear guest depth transfers

Commits ad66469 and 46a7c1f specialize linear Z16/Z24S8 transfers into
unaligned-safe word loads/stores. Morton storage retains its previous path.
Double-precision conversion/rounding, row padding, and validation-before-store
remain unchanged. Guest access must remain serialized by the renderer.

The CPU layout fixture passes, including a scalar byte oracle for odd widths,
unaligned bases, padded strides, exact loaded floats and stencil. Mac main and
Android offscreen targets build. No headset access occurred.

A local 640x480 Z24S8 store microbenchmark against 9c011c0 measured
0.517-0.532 ms before and 0.214-0.218 ms after (2.39-2.47x), with identical
full output buffers. Source and executable: /tmp/vc3-depth-bench. This is a
microbenchmark, not a game FPS claim.

The 2160-frame baseline /tmp/vc3-dynamic-blend-profile aborted (exit -6)
after over 1000 flips with BlockOnTime(0x14B5), head 0x14C7, semaphore 0x149B.
No gameplay acceptance follows from this run. Its CPU sample identifies
substantial guest depth publication work. The optimized follow-up is
/tmp/vc3-depth-words-profile; inspect its live report before drawing conclusions.
