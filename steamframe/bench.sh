#!/bin/sh
# bench.sh <label> [binary] -- scripted VC3 combat run on the Frame, VR build by default.
# Extra env comes from the caller. Prints per-window frame-time stats and mean costs.
LABEL=$1; BIN=${2:-$HOME/work/vc3/build-xr/vc3}
OUT=/tmp/vc3b/$LABEL; rm -rf $OUT; mkdir -p $OUT/save $OUT/cfg
cp ~/Games/VC3/config/settings $OUT/cfg/ 2>/dev/null
cp /tmp/vc3s/save/vulkan-pipelines.cache $OUT/save/ 2>/dev/null
if pgrep -x vc3 >/dev/null; then echo "vc3 RUNNING - skipped"; exit 1; fi
export LD_LIBRARY_PATH=$HOME/work/vc3-deps
export CHIHIRO_AUTOKEYS="12:coin,13:start,15:fire,20:start,22:start,25:start,26:pedal,26.5:pedal,27:pedal,27.5:pedal,28:pedal,28.5:pedal,29:pedal,29.5:pedal,30:pedal,30.5:pedal,31:pedal,31.5:pedal,32:pedal,26.5:fire,27.5:fire,28.5:fire,29.5:fire,30.5:fire,30:reload,31:reload"
export CHIHIRO_AUTOKEYS_CLOCK=flips VC3_SOUND=0 VC3_VULKAN_FRAME_LIMIT=2100 VC3_VULKAN_INTERVAL_LOG=1
export CHIHIRO_VULKAN_PROFILE=1 CHIHIRO_VULKAN_FRAME_COSTS=1 CHIHIRO_VULKAN_GPU_TIMESTAMPS=1 VC3_PACE_LOG=1
export VC3_SAVE_DIR=$OUT/save VC3_CONFIG_DIR=$OUT/cfg
timeout 120 "$BIN" ~/work/vc3-game > $OUT/log 2>&1
python3 - $OUT/log "$LABEL" <<'EOF'
import sys, re, collections
log, label = sys.argv[1], sys.argv[2]
W = {"combat": (1322, 1560), "activation": (1562, 1680), "held": (1682, 1920)}
iv = collections.defaultdict(list); cost = collections.defaultdict(lambda: collections.defaultdict(float)); n = collections.Counter()
for l in open(log, errors="replace"):
    if l.startswith("[Vulkan frame costs]"):
        d = dict(re.findall(r"(\w+)=([\d.]+)", l)); f = int(d["frame"])
        for w, (a, b) in W.items():
            if a <= f <= b:
                n[w] += 1; iv[w].append(float(d["interval_ms"]))
                for k, v in d.items():
                    if k.endswith("_ms"): cost[w][k] += float(v)
reached = "frame limit reached" in open(log, errors="replace").read()
print(f"== {label} (reached limit: {reached})")
for w in W:
    x = sorted(iv[w])
    if not x: print(f"  {w}: no frames"); continue
    p = lambda q: x[min(len(x) - 1, int(q * len(x)))]
    c = cost[w]; k = n[w]
    print(f"  {w:10s} n={k:3d} mean={sum(x)/k:6.2f} p50={p(.5):6.2f} p95={p(.95):6.2f} >20ms={sum(1 for v in x if v > 20):3d} | "
          + " ".join(f"{m[:-3]}={c[m]/k:.2f}" for m in ("draw_ms", "completion_ms", "batch_wait_ms", "imports_ms", "clear_ms", "other_ms")))
EOF
