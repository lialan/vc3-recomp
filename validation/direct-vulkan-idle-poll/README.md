# Executor idle-poll diagnostic

2026-11-01, base7ff46df. Same /tmp/vc3-accounting-fixed-vc3 binary, local Vulkan simulator/headless only. No headset operations or rendering changes. One2100frame pair: existing RECOMP_ACK_IDLE_US=100 default versus0 (yield-only after idle spins). Both retain corrected thread accounting and aggregate frame traces, identical populated cache copies, dirty pooling enabled, QoS unchanged, flip hold unset. Both exit0/reach limit. Runner now records RECOMP_ACK_IDLE_US in provenance. No rendering fixtures/build matrix repeated for an existing-knob diagnostic.

| Scene | Default FPS | No-sleep FPS | Default p95 ms | No-sleep p95 ms |
| --- | ---: | ---: | ---: | ---: |
| Combat early | 60.02 | 59.96 | 21.84 | 20.42 |
| Combat late | 60.01 | 59.99 | 21.23 | 20.81 |
| Activation | 59.83 | 59.87 | 21.98 | 19.84 |
| Held early | 59.64 | 59.93 | 23.04 | 19.79 |
| Held late | 60.34 | 60.00 | 20.32 | 21.22 |

Approximate slow-motion thread samples (six once-per-second reports, aligned to preceding60-frame markers) show executor CPU475.5 ->606.3ms/s, +27.5%. Accounted sleep96.3 ->48.3ms/s; yield and other sleep sites remain accounted even with idle sleep disabled. This is not a strict per-window partition. p95 improves in activation/earlyheld but worsens later; activation maximum28.60 ->31.53ms. No steady60 or consistent tail-win claim. The comparison suggests polling may contribute to pickup latency, but cannot isolate host variability or establish that polling is the dominant issue. Default remains100us; no permanent busy-spin or realtime GPU policy introduced.

Full environments/hashes in reports, selected warm thread/frame summaries in logs. Rawlogs /tmp/vc3-poll-{control,no-sleep}/game.log. Guest completions/vblank-delta markers are not display cadence or display-drop counts. Diagnostics perturb execution; this is causal exploration rather than acceptance. All task processes terminal.

Next concrete shared-runtime bundle: record_batch currently repeats vkCmdBindPipeline, vkCmdSetBlendConstants and vkCmdPushConstants on every draw. Cache unchanged command state within a command buffer, resetting at batch start and after depth-quantization helpers that bind their own state. Preserve layout compatibility, descriptor lifetimes and per-draw geometry offsets; validate changing-state/quantization sequences once as a bundle. This targets executor/driver CPU cost without spending an extra core polling. Avoid repeating idle/QoS/flip-hold sweeps without new evidence.
