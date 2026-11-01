# Reference texture-coordinate forwarding

A native game diagnostic aborts at frame 1218: vertex 757 component 12
(tex0.y) is infinity while stage_program is zero. All texture stages return
constants there; their coordinates cannot affect fragment output. Validating
all 21 interpolants introduced an unnecessary fatal rejection.

The adapter now zeroes unconsumed coordinates using the reference stage rules
in the shared header. Constants/missing samplers consume none; 2D uses xy and
projective q except mode 3; cube uses xyz; PASSTHRU consumes all four.

An intermediate game replay with only that mask change exposed a second
failure at frame 1016: a consumed tex0.y was NaN with stage_program=1. The GL
renderer forwards those shader attributes to the GPU rather than rejecting
the game. Actual Quest GLES/Vulkan differential tests then matched all RGBA
bytes for eight 2D point/linear NaN/Inf cases and three nonfinite cube sampling
modes, in addition to disabled modes with nonfinite inputs. The final adapter
therefore forwards consumed texture coordinates too. No portable sampling
color for NaN/Inf is assumed: the comparisons describe this GPU/driver.

The 2D comparisons also exercise the actual NV2A decoded-batch adapter through
a real sampled texture and compare it against GLES. Cube/2D ignored-component,
missing sampler and PASSTHRU forwarding fixtures pass. Position, clipping,
fog, guest bounds and sampler/state validation remain active. No invalid
buffer access or unsupported draw state is silently accepted.

Native/GLES finite pixel parity, queries, guest coherence/completion, resident
depth, layout and injected-XR ownership tests remain passing. Original GL APK
hash is unchanged; the whole Quest session was cleaned up under the shared
lock. Both Mac renderer builds pass. APK reports describe build/import audits,
not gameplay installation. A separate longer game replay checks the final
forwarding change. Full game/immersive and Quest combat acceptance remain open.
