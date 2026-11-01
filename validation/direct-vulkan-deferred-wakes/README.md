# Wake deferred producers only at their requested boundary

The shared deferred queue previously signaled a blocked producer after every consumed record, even when the producer awaited a later record. It now publishes the absolute requested tail position atomically. The consumer signals once when that position is reached and rechecks/clears the waiter under the mutex, avoiding repeated intermediate wakeups. The sleeping-consumer flag also uses atomic acquire/release operations and is rechecked under the mutex before signaling.

Both sides retain the paired seq-cst fence and progress recheck protocol: either the peer observes the waiter or the waiter sees the published progress before sleeping. This allows untimed condition waits, eliminating the 2ms idle polling loop. Spurious wakes still recheck progress. Ring ownership, GPU/backend flushes, semaphore ordering, guest clocks and simulation speed are unchanged.

Real-queue stress passed 1000 batches each of1/8/128draws, three alternating control/candidate pairs per batch size. Source-mutation isolation, overlapping streams, ring reuse and ordered completion remain checked. Separate flush fixtures passed asynchronous and synchronous-replay modes. Native game and Android shared kernel builds passed. Windows code updated consistently but not executed here.

| 128-record batches, median of3 | Before | After |
|---|---:|---:|
| Process CPU ms |61.076|39.266|
| Wall ms |49.203|32.298|
| Reported involuntary context switches |8966|2306|

Batch8 CPU10.607->8.894ms; batch1 CPU7.924->7.228ms. These are short synthetic queue measurements using macOS getrusage child-process counters, not game FPS or headset measurements. Raw samples retained in queue-benchmark.json; startup outliers are visible there. No repeating full-game sweeps were used.

Portable nested-runtime patch is against3f1ba768. Installed APK unchanged; no headset access.

A fresh sequential local A/B used ordinary defaults, identical diagnostics (including thread CPU accounting), and isolated copies of the same warm cache. Both2100frame runs exited0/reachedlimit. Fourcombat-period thread samples averaged decode76.5->48.75msCPU/s (~36%less) and submission304.75->277.5msCPU/s (~9%less). Sample boundaries differ byup to25frames, so this supports reduced CPUuse rather than an exact per-frame speedup. Excludingpipelinecreationframes, combatdrawCPU2.156->2.026ms and completionCPU1.857->1.740ms; activation/held improvements smaller.

FullFPSwindowscontrol->candidate: combat60.05/59.97->59.92/60.04; activation59.95->59.67; held59.87/60.10->60.23/51.41. The lastcandidatewindow contains frame1855:366.230ms total,353.631ms pipelinecreation; it is not evidence of an added queuewait. Candidatecachegrewto70343412bytes,saveok. Tailsremain~22ms, no steady60orQuestFPSclaim. CPUgraphicsconversioncountersremainzero/flatwarmtexturefallback. No captures requested for this synchronization-only game comparison; deterministicqueuefixtures verifyordering/content.

Runs/tmp/vc3-deferred-wakes-{control,candidate}; candidatebinary/tmp/vc3-deferred-wakes-vc3. All test/run processes terminal. Next work: remaining guest fence polling and submission/completion costs, preserving event ordering and the real game clock.
