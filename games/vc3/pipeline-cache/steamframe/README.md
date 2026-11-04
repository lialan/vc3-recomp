# Steam Frame Vulkan pipeline seed (yours, not in git)

As for the Quest (`../quest3/README.md`), a seed holds GPU code compiled from
the game's own shaders, so none is committed. Make one on the Frame with
`steamframe/harvest.sh`: it plays each mission unattended, merges every
pipeline recipe the runs recorded, and boots once from an empty save with
only those, so the boot precompile fills a fresh Turnip cache. Put its
`turnip-seed.cache` here and configure the Frame build with
`-DCHIHIRO_VULKAN_PIPELINE_CACHE_SEEDS=<this folder>/turnip-seed.cache`: the
binary then starts with every harvested pipeline compiled (no first-time
stall of 100 ms to 1 s per new shader), and copy the merged
`pipelines.recipes` beside the installed game, where the launcher adds it to
the save.

A seed is tied to the GPU and the Mesa build (its pipeline-cache UUID); after a
SteamOS update that changes Mesa the binary ignores it and the recipes are
precompiled at boot instead, from scratch the first time.
