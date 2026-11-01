# Exact guest write tracking

The native renderer can now skip an unchanged color/depth observation using
host page generations. `CHIHIRO_VULKAN_WRITE_WATCH=1` enables it; the default
remains off. The Quest runner exposes `--write-watch`. Actual mapped contiguous
and tiled views come from the kernel registry, and initially writable permissions
are verified. Unsupported pages/mappings retain the exact byte comparison path.
CPU edits still merge by pixel/depth/stencil component. No sampled-hash
approximation or OpenGL renderer dependency is introduced.

The shared tracker handles write faults across aliases, independent observers,
and concurrent reprotection. Neighboring pages are protected together. Both
POSIX file-read entry points and GPU publication hold write leases; protection
and unmap operations invalidate tracking first. Read/execute faults retain normal
handling. See `docs/vulkan-coherence.md` for lifecycle and concurrency contracts.
The kernel/platform changes are checkpointed at submodule `4e81d95`.

Mac and Quest standalone tests pass for alias writes, independent subscribers,
1000 concurrent writable/rearm cycles, actual write/NX fault decoding, a blocked
kernel read lease, external protection changes and teardown. The Quest GPU suite
also verifies tracked resident color/depth edits, 100 unchanged draws, exact
EQUAL visibility, GPU neighbor preservation and protection-change fallback.
Mac platform and kernel regressions exercise protected ReadFile/NtReadFile
destinations, EOF/error leases, errno preservation, permission/unmap notifications
and the actual relocated alias registry across shutdown/reinitialization.

Two same-binary Quest startup runs reach 240 frames with the same capture range.
Their first 120 guest-completion intervals are:

| Mode | FPS | Mean | p99 | Maximum |
| --- | ---: | ---: | ---: | ---: |
| Tracking off | 28.03 | 35.67 ms | 177.32 ms | 258.07 ms |
| Tracking on | 36.77 | 27.19 ms | 161.21 ms | 322.44 ms |

This is one startup pair, with cold-driver and guest-timer variability, not
sustained combat or pacing acceptance. Tracking reports 26,296 unchanged checks
out of 29,257 by frame 240.

The longer tracked run reaches all 2160 frames in 278.4 recorded seconds,
collecting 29,727 user CPU samples with none lost. It skips 829,654 of 862,142
observation checks (about 96%) and handles 34,200 CPU write faults. The bomb
battle capture at source frame 1920 matches the prior untracked native 256MiB
capture outside the blinking INSERT COINS prompt: the only 1299 changed pixels
are within x429..574/y439..449, with maximum channel delta 9. The builds/cache
budgets differ, so this is narrow capture evidence, not synchronized A/B or
whole-game correctness acceptance.

Combat throughput is still unacceptable. Windows at 1561..1681 and 1681..1801
measure 6.94/6.39 FPS; the latter has p99 174.26 ms. The 1921..2041 transition
falls to 3.81 FPS with p99 539.47 ms. In the final 60 seconds, memcmp is only
4.11% of sampled user CPU; texture decoding (`sample_tex`) is now 39.61%.
The cache reaches 14,362 uploads/14,246 evictions at 2160 frames with a 64MiB
budget. Texture working-set/upload behavior is the next performance target.
This trailing window differs from earlier 1800-frame profiles; CPU percentages
must not be interpreted as a same-window speedup estimate.

The refreshed APK audit finds no OpenGL imports, but is build-only. All Quest
sessions held the whole-session shared lock, cleaned private files and verified
the original OpenGL APK unchanged. No app was installed. Raw recordings remain
in the ignored build profile tree and original runner outputs; relevant logs,
reports, the capture and CPU summary are committed here. Full gameplay,
immersive attachment, and sustained Quest combat/frame-time acceptance remain
open.
