#!/bin/sh
# harvest.sh [seconds] -- pipeline coverage to bundle with the Frame build:
# play.sh through each mission (Mission Select aims in VC3_MISSIONS, "x,y"
# each), merge the recipes every run recorded with the installed game's (each
# once), then boot once from an empty save holding only those: the boot
# precompile fills a fresh driver cache -- the seed to build in
# (games/vc3/pipeline-cache/steamframe/README.md). Both are made from the
# game's shaders, so neither goes into git.
# Out: /tmp/vc3p/harvest/pipelines.recipes and turnip-seed.cache
SECS=${1:-1200}
BIN=${BIN:-$HOME/work/vc3-fix/build-xr/vc3}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=/tmp/vc3p/harvest; mkdir -p $OUT
if pgrep -x vc3 >/dev/null; then echo "vc3 RUNNING - skipped"; exit 1; fi
RUNS=""
for m in ${VC3_MISSIONS:-0.5,0.5 0.2,0.5 0.8,0.5}; do
    L=mission-$(echo $m | tr ',.' '__')
    [ -n "$SKIP_PLAY" ] || AUTOAIM="sweep:26:$m" BIN=$BIN "$HERE/play.sh" $L $SECS
    RUNS="$RUNS /tmp/vc3p/$L/save/vulkan-pipelines.recipes"
done
python3 - $OUT/pipelines.recipes ~/Games/VC3/save/vulkan-pipelines.recipes ~/Games/VC3/pipelines.recipes $RUNS <<'EOF'
import sys
RECIPE = 2688                      # sizeof(Recipe) in vk_target.c
out, seen, keep = sys.argv[1], set(), []
for path in sys.argv[2:]:
    try: data = open(path, 'rb').read()
    except OSError: continue
    if len(data) % RECIPE: sys.exit(f'{path}: not whole recipes ({len(data)} bytes)')
    for i in range(0, len(data), RECIPE):
        r = data[i:i + RECIPE]
        if r not in seen: seen.add(r); keep.append(r)
open(out, 'wb').write(b''.join(keep))
print(f'{len(keep)} recipes -> {out}')
EOF
T=$OUT/train; rm -rf $T; mkdir -p $T/save $T/cfg
cp ~/Games/VC3/config/settings $T/cfg/
cp $OUT/pipelines.recipes $T/save/vulkan-pipelines.recipes
LD_LIBRARY_PATH=$HOME/work/vc3-deps VC3_SAVE_DIR=$T/save VC3_CONFIG_DIR=$T/cfg VC3_SOUND=0 \
    VC3_VULKAN_FRAME_LIMIT=1200 timeout 300 "$BIN" ~/work/vc3-game > $T/log 2>&1
grep -E 'Vulkan pipelines\]|pipeline cache\]' $T/log | head -4
cp $T/save/vulkan-pipelines.cache $OUT/turnip-seed.cache && ls -la $OUT
