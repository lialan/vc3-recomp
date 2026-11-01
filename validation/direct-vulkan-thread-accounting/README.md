# Corrected shared thread accounting and slow-motion diagnosis

2026-11-01. Baseb260315; shared executor fix9df0dd4c. Local Vulkan simulator/headless only.

The first diagnostic run reproduced two reporter starts. Concurrent first callers raced on g_thread_acct_on and launched duplicate reporters sharing last-sample state; alternating near-zero/full samples were invalid. Do not use /tmp/vc3-completion-accounting/game.log for per-thread CPU conclusions. No claim that this diagnostic-only race caused normal profiling-off frame jitter.

Initialization now uses pthread_once / Windows InitOnceExecuteOnce, atomically publishes the enabled state, and disables accounting if reporter creation fails. All enabled-state accesses use the shared atomic helper. Root runner now records RECOMP_THREAD_LOG in provenance. The focused standalone POSIX race test launches32 callers and32000 initialization calls in each enabled/disabled process, asserting exactly1/0 reporter creations. Native build passes. Corrected real-game run starts exactly one reporter, reaches2100frames, exits0. Windows branch not executed here. This does not claim to fix every lifetime/snapshot concurrency concern in the accounting facility.

Run uses RECOMP_THREAD_LOG=1 and RECOMP_FRAME_TRACE pointing to a nonexistent file: once-per-second frame summaries enabled, detailed batch trace never armed. Per-draw Vulkan/executor/GPU timestamp diagnostics remain off. Report preserves flags and binary hash. Full rawlog /tmp/vc3-completion-accounting-fixed/game.log; accepted binary /tmp/vc3-accounting-fixed-vc3. Saved excerpts/samples use the latest logged completed-frame marker, so scene alignment is approximate (one-second windows, markers every60frames).

| Approximate scene | samples | Executor CPU ms/s | Executor sleep ms/s | Guest main CPU ms/s | Guest main object wait ms/s |
| --- | ---: | ---: | ---: | ---: | ---: |
| Combat, markers1320–1500 | 4 | 339.2 | 391.8 | 79.8 | 920.0 |
| Slow motion, markers1560–1860 | 6 | 471.3 | 190.3 | 93.2 | 906.0 |

CPU values include host command processing/driver work and exclude blocked/descheduled time and other workers; they are not CPU shader-emulation measurements. Completed-wait counters can cross reporting boundaries and are not a strict per-window partition. Guest main uses much less CPU than the executor, so these samples do not support guest instruction simulation as the dominant slow-motion cost. Final command segment execution typically rises from~7ms combat to~10–11ms slow motion; late segments reach~13–16ms. Walk starts commonly occur3–4ms after vblank. Frame trace calls a flip late when its vblank-count delta>=2; catch-up flips can share a vblank. Those markers are not display-drop counts and do not establish steady60.

Next causal target: nv2a_ack_thread idle polling uses RECOMP_ACK_IDLE_US=100 after200 idle passes. On macOS short nanosleeps can overshoot; inspect wake/poll latency rather than assume its nominal100us delay. A bounded existing-knob control may distinguish it from execution time before designing a shared event/wake mechanism. Do not ship permanent busy-spinning or a realtime GPU thread just to pass a benchmark. Prior QoS and flip-hold probes remain inconclusive. No rendering fixture matrix rerun for this diagnostic fix.

Reproduce focused race check:

```
cc -std=c11 -Wall -Wextra -Werror -pthread tools/thread-acct-init-test.c -o /tmp/thread-acct-init-test
/tmp/thread-acct-init-test 0
/tmp/thread-acct-init-test 1
```
