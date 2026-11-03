# Quest3 Vulkan pipeline seed (yours, not in git)

A pipeline-cache seed holds GPU code compiled from the game's own shaders, so
none is committed: like the game files, each user makes their own. Train one on
your Quest (Adreno 740 driver, not the macOS simulator) by playing or with the
scripted routes in `tools/`, and put the resulting `adreno740-<uuid>.cache` in
this folder; `quest/build.sh` packages every `*.cache` here into the APK and
then requires it to be valid. With no seed the APK still works: the game
records each pipeline it builds on the headset and compiles the recorded ones
at the next start, so only the first time a scene is seen can stutter.

Seeds are tied to the device and driver build; a Quest system update may need
a new one. Earlier training evidence: validation/direct-vulkan-quest-driver-cache.
