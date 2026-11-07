# Quest3 Vulkan pipelines (yours, not in git)

Everything here is made from the game's own shaders, so none of it is
committed: like the game files, each user makes their own on their own Quest
(Adreno 740 driver, not the macOS simulator). Three kinds of file, all
packaged into the APK by `quest/build.sh`:

- `*.cache`: the driver's pipeline cache (a "seed"). Tied to the device and
  driver build; a Quest system update may need a new one.
- `*.recipes`: every pipeline the game has built -- what the runtime
  precompiles before the game starts, so a first launch never compiles one in
  play (vk_target.c). All files are joined.
- `*.specialized`: the shaders specialized on the CPU for those pipelines
  (~0.5 s each to make on the headset; the newest file is used). Without it a
  first launch spends tens of seconds specializing.

Harvest them after playing (or running the scripted routes in `tools/`) with
`scripts/harvest-pipelines.sh <game>`: it starts the app once with
`CHIHIRO_VULKAN_PIPELINE_EXPORT`, which copies all three out of the app's
private folder, pulls them here and stops the app. Then build. With none here
the APK still works: the game records what it builds on the headset and
precompiles that at the next start, so only a first launch's first sight of a
scene can stutter.

Earlier training evidence: validation/direct-vulkan-quest-driver-cache.
