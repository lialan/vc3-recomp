# Earlier combat reference and bounded native attempt

Using the shared flip clock, the GL sequence12:coin,13:start,15:fire,
20:start,22:start,25:start,30:start reaches the bomb-target battle by source
frame2400. The retained real frame has the gameplay HUD, target marks and live
countdown. The full scripted environment and binary hash are in gl-report.json.
The process was deliberately terminated after its three requested captures.

The immutable packed-depth Quest build attempted the same sequence, with a
2500-frame gate and300-second watchdog. It reported through frame1980 before
the watchdog and never produced the requested2400-frame captures. Logged inputs
and continued drawing do not establish its combat state or image correctness.
This attempt is not a combat or performance acceptance pass. Its private UUID
tree was cleaned and the original GL APK hash remained unchanged.

The newer periodic capture option avoids losing all visual evidence when a
future long diagnostic run misses its final capture gate. Those capture runs
must remain separate from frame-pacing acceptance because they add file I/O.
