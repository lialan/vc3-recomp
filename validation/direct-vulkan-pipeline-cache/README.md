# Backend-scoped driver pipeline cache

The renderer now retains a VkPipelineCache across its targets and target
resizes. Pipeline objects, descriptor pools and the bounded LRU remain local
to each target. The backend owns and destroys the driver cache after all
surfaces; targets borrow it only before their first pipeline creation.
Standalone targets retain their existing private-cache behavior. Access is
serialized through the backend queue lock / single executor contract.

The specialized-combiner GPU fixture shares one cache across interpreter and
specialized targets, destroys/recreates targets, rejects rebinding, and still
passes exact/toleranced rendering and 140-program LRU checks. The renderer
snapshot fixture passes including resize and teardown. Mac executable and
Android offscreen builds pass. These are local correctness/build results;
no actual-game speedup or Quest runtime acceptance is claimed yet.

The separate immutable empty-clip run /tmp/vc3-empty-clip-profile was still
active near frame 1080 when this change was completed. It predates shared
caching and must not be used as evidence for this optimization.
