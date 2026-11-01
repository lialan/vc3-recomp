# Split pushbuffer segment regression

The default segment scanner previously discarded unfinished method payloads
at a PUT boundary. The next segment interpreted the remaining parameter as a
command header. A three-parameter increasing packet split after its first
parameter reproduced two callbacks (354 and 0) instead of 354/358/35c.

Submodule 3cb6443 retains the remaining method/subchannel/count across a
contiguous boundary; a discontinuous segment discards the partial packet.
This does not change DMA GET publication or fence completion.

Run the standalone regression from the repository root:

```
cc -O2 tools/vulkan-pb-segment-test.c external/xboxrecomp/src/kernel/nv2a_pb_scan.c -o /tmp/vc3-pb-segment-test
/tmp/vc3-pb-segment-test
```

Every split position matches unsplit callbacks, with increasing and
nonincreasing methods, scalar/bulk dispatch, CALL/RETURN, and discontinuous
restart. Mac guest and Android offscreen builds pass. This proves the parser
bug and focused repair, not the cause of the actual-game transition failure.

The write-watch-disabled control /tmp/vc3-shared-cache-no-watch still aborted
on BlockOnTime(14B1), head 14C3, semaphore 1497. Thus disabling write watch
alone did not resolve the observed class of failure. The patched follow-up
/tmp/vc3-split-packet-profile uses immutable /tmp/vc3-split-packet-vc3 and
RECOMP_PB_SPLIT_LOG=1 to report the first 16 actual split packets. Inspect its
report before claiming game-level resolution. No headset access occurred.
