# Full depth/stencil replacement avoids importing old pixels

A validated clear replacing both depth and stencil over the whole target can
bind its resident D24 owner without importing values it immediately overwrites.
Exact superseded ownership is discarded; changed zeta/extent/pitch/layout still
publishes the previous interpretation first. Current guest RAM is snapshotted
before clearing so old CPU edits cannot be resurrected as new edits afterward.
Padding remains untouched. Partial rectangles, depth-only and stencil-only
clears retain preservation/import behavior. Native D24 residency is required;
other depth paths are unchanged.

The complete Quest GPU suite passes. New assertions cover two consecutive full
clears across targets, dirty GPU contents and intervening CPU writes, no imports
or premature publication, later binding/flush, exact depth/stencil and padding.
The coverage predicate rejects partial rectangles and component clears. Existing
component-merge, overlapping zeta, palette/texture alias, target-resize and real
executor semaphore visibility fixtures continue to pass. An initial fixture
padding assertion incorrectly reused bytes that an earlier shifted-zeta test
had legitimately modified; seeding the padding for this independent case fixed
the fixture, without relaxing the preservation assertion.

The same O2/A78 binaries both reach240 startup frames. First120 intervals report
14.44FPS / p99 226.95ms with old-content imports, versus15.54FPS / p99 216.41ms
when skipped. Maxima remain299.97ms and313.64ms respectively. At frame240, imports
fall from1068 to591, with1184 clear bindings skipped. These single samples are
not a combat benchmark or a claim that frame pacing is fixed. Wall-time-dependent
startup means frame-indexed game images are not guaranteed to be synchronized.
`CHIHIRO_VULKAN_DEPTH_CLEAR_SKIP=0` retains the diagnostic import control.

The fresh APK builds with no OpenGL imports and has not been installed. Sessions
held the shared Quest lock for the entire suite/A-B runs, cleaned their UUID
directories and verified the original GL APK hash was unchanged. Full gameplay,
immersive attachment and sustained Quest combat/frame-time acceptance remain open.
