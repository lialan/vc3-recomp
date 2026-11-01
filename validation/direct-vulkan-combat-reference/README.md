# Verified local OpenGL combat sequence

The OpenGL reference now reaches gameplay rather than only the title/mission
cinematics. In an isolated save/config run, coin at 30 seconds and Start at 31
enter mission selection. Aim (0.2, 0.5) and fire at 40 select FILE 01. Start at
60 and 62 skips the intro. Captures at 68 and 78 show the active HUD, ammunition
count and bomb targets; the 90-second capture shows an enemy and the aiming
reticle. The report records the exact executable hash and input environment.
The process was deliberately terminated after its final 90-second capture.

The 120 FPS display setting enables the existing GL real/interpolated-frame dump;
the game's tick remains 60 Hz. Only the real `_1.bmp` frames were selected for
these PNGs. This run is local GL reference evidence, not a Vulkan comparison,
Quest performance result or immersive validation.

The next comparison must account for the current wall-time input scheduler:
slow Vulkan execution sees those presses at different game frames. Reusing this
wall-time script directly is insufficient for synchronized image parity. A
shared completed-game-frame input clock or observation-driven selection is
needed before attributing scene differences to rendering.
