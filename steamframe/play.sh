#!/bin/sh
# play.sh <label> [seconds] -- VC3 VR plays on by itself
# (headset off) for a scene sweep: sweeping aim, a shot every 0.25 s, reloads,
# the pedal, Start every 2 s and a coin every 6 s to continue. Runs on copies of the
# installed game's settings and save (pipeline cache and recipes), never on
# them. Log: /tmp/vc3p/<label>/log. Extra env comes from the caller.
LABEL=$1; SECS=${2:-480}
BIN=${BIN:-$HOME/work/vc3-fix/build-xr/vc3}
OUT=/tmp/vc3p/$LABEL; rm -rf $OUT; mkdir -p $OUT/save $OUT/cfg
if pgrep -x vc3 >/dev/null; then echo "vc3 RUNNING - skipped"; exit 1; fi
cp ~/Games/VC3/config/settings $OUT/cfg/
cp ~/Games/VC3/save/vulkan-pipelines.cache ~/Games/VC3/save/vulkan-pipelines.recipes $OUT/save/ 2>/dev/null
export LD_LIBRARY_PATH=$HOME/work/vc3-deps
E=$SECS
export CHIHIRO_AUTOKEYS="12:coin,13:start,15:fire,20:start,22:start,25:start,26-$E/0.25:fire,26.1-$E/1.75:reload,27-$E/4:pedal,30-$E/6:coin,30.5-$E/2:start"
export CHIHIRO_AUTOKEYS_CLOCK=flips VC3_AUTOFIRE_MS=80 VC3_AUTOAIM=${AUTOAIM:-sweep:26}
export VC3_SOUND=${VC3_SOUND:-0} VC3_PACE_LOG=1 VC3_VULKAN_FRAME_LIMIT=$((SECS*60))
export VC3_SAVE_DIR=$OUT/save VC3_CONFIG_DIR=$OUT/cfg
timeout $((SECS*3/2+60)) "$BIN" ~/work/vc3-game > $OUT/log 2>&1
echo "== $LABEL exit $?"
grep -E '\[FILE\] D:.media.(mdl|tex)_' $OUT/log | grep 'opts 0x60' | sed 's/.*media.//; s/ (disp.*//' | tr '\n' ' '; echo
grep '^\[stats\]' $OUT/log | awk '{print $2}' | tr '\n' ' '; echo
