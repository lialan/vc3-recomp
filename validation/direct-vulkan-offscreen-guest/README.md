# Actual Quest guest execution without XR

The optional Android CLI links the actual generated guest and native Vulkan
runtime. It calls chihiro_main directly, bypassing Java storage lookup and
shared env/log setup, with isolated save/config/capture directories. Normal
headed Android startup still uses the OpenXR-selected device. No GL fallback
exists. The reusable runner is tools/run-vulkan-quest-offscreen.py; its entire
invocation must run through the shared quest3-lock wrapper.

The initial four-frame probe passed on Adreno740. A 240-frame run of the
reusable runner also passed and captured frame180. Its first120 guest intervals
average12.24FPS, mean81.72ms, p95 97.17ms, p99 230.56ms, max292.05ms. All120
intervals exceed33ms. This is poor startup performance, not an acceptance pass.
These binaries use generated-code O1 and single-thread submission, so this is
not a release-build comparison or a measurement of VR/compositor frame times.
The report identifies all copied binaries by hash.

The longer run requested6100 frames with a240-second watchdog. It exited137
at the watchdog after reporting frame1560, before the frame6000 capture gate.
The retained trace has203025 GPU fixed batches by frame1440 and zero backend
declines; resident command batches and native D24 residency are active. Early
3D/title playback reports3.72FPS around frame1448. It never reaches the first
scripted coin at executor flip1800 and does not establish combat correctness.
The post-run provenance file records the copied binary hashes before any later
rebuild. Both sessions cleaned their UUID directories and verified the original
GL APK was unchanged. No app installation, launch, tracking/Guardian or
proximity changes were made.

The shared executor flip clock drives the same input scheduler on GL and Vulkan.
Bounded GL captures now retain the source executor flip through delayed PBO
publication. With later Start presses, GL reaches a combat Continue screen at
source flip6000. This is a reference endpoint, not synchronized Vulkan parity.
The Mac Vulkan comparison was deliberately stopped after frame2820 once actual
Quest profiling became available; unsupported resident D24 makes its strict
transfer path unsuitable for Quest performance inference. It did not reach its
5400-frame gate and produced no requested4900-frame captures.

The next performance work is to remove unnecessary CPU depth round trips on
exact shared-zeta target switches, retain coherence at real guest completion
boundaries, and compare with the existing OpenGL production build settings.
An O2/A78 Android build is separate from these O1 measurements. Full gameplay,
immersive host attachment and sustained Quest combat/frame-time acceptance
remain open.
