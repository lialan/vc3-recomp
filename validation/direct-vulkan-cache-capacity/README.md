# Direct Vulkan texture capacity experiment

Quest GPU tests cover immutable texture ownership, cache hits and eviction,
CPU texel/palette changes, sampled mip/cube images and a new 96-image working
set under a 4096-byte estimated payload budget. Deferred completion modes,
layout transfers and the injected-XR ownership tests also passed.

Mac game reports use the same native executable and strict GPU fence setting.
Only capacity changes between 64 and 2048 slots; both have the same estimated
64 MiB payload budget. Inputs are scheduled by wall time, so identical input
scripts do not imply identical guest state or draw workload. These runs and
the cinematic capture are not Quest performance or combat acceptance.

The fresh-marker Quest startup log confirms the candidate and new tail-frame
logs, but the XR session repeatedly lost focus/running state and the headset
showed Finding position in room. No dialog was bypassed. Candidate stopped,
environment restored and proximity override disabled before releasing the
shared lock; original GL APK SHA256 remained
3a0660db0d8a5d8e6d95a555dc087bd8f9351f2f8df91e601599ae2789b53ad6.
The app remains far below the combat target. No system screenshot is retained
in this evidence directory.
