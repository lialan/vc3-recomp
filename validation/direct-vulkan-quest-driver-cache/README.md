# Actual Quest 3 driver cache checkpoint

Quest 3 Adreno 740, vendor 0x5143, device 1124403968, UUID 6cc61008435100000000000b05430000. The committed seed contains 1,224,137 driver bytes; the CHVK file SHA256 is a00b3fec4f8c0241188827f927462696132e394cb25403f3f9fadc9460b7c2b6.

Cold training and a fresh writable-cache packaged-seed test both completed 2,100 frames on the actual device using the native offscreen runner. This route was the shooting-range/tutorial, not full combat. Across 74 graphics pipeline creations, cold total/max were 44,607.491/36,948.179 ms; packaged total/max were 466.077/15.568 ms, with zero creations over 20 ms. The packaged test loaded 1,224,137 packaged bytes and zero writable-cache bytes. The default 30-second fence watchdog passed the packaged test; training needed the explicit 600,000 ms override. The override extends the timeout without acknowledging unfinished work.

These results establish driver-cache reuse, not steady 60 FPS. Late packaged offscreen windows were about 10.35–10.47 FPS. Frames 1600, 1720, 1840 and 1960 matched exactly; other captures differed. No OpenXR presentation or full-game parity claim. Unseen pipeline states and driver updates can still compile.

A subsequent corrected-input training run was cancelled and cleaned when the user requested a checkpoint installation. It did not replace this seed. The checkpoint APK and installation provenance are in /Users/lialan/Downloads/VC3-Quest3-PipelineCache/. Automated VC3 testing stops while the user tests the installed checkpoint.
