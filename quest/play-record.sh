#!/usr/bin/env bash
# quest/play-record.sh [--secs n] [--env file] [--out dir] [--launch]:
# evidence from real play. While the player plays as usual (headset worn, on
# USB), the headset's display is recorded in back-to-back screenrecord
# segments of 30 s for --secs seconds (default 180) and logcat is kept
# whole (a 64 MB ring, streamed to the Mac). Nothing about the game is
# changed: no mute, no scripted input, no proximity override, no boundary
# pause. --env pushes that file as env.txt first -- meant for the
# diagnostic-only ones in quest/playrec-env/ (each read at the game's
# launch, so the game must be started after it: --launch starts it, else
# the player does). Each segment's start is written into logcat
# ("PLAYREC segment <k> start") so quest/play-analyse.py can put the
# recordings and the log on one clock.
#
# At the end (or on Ctrl-C) it pulls the segments and vc3.log into --out
# (default quest/playrec-out/<time>), deletes the segments from the
# headset, and removes the env.txt it pushed (putting back one that was
# there before). It never stops the game and never touches the player's
# settings. The recording costs the headset a video encoder's work.
set -u
SERIAL=${SERIAL:-2G97C5ZJ0G02DQ}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK=${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}
A="$SDK/platform-tools/adb -s $SERIAL"
. "$ROOT/scripts/game.sh" vc3               # PKG, ACTIVITY, HEADSET_DIR
SECS=180; ENVF=; OUT=; LAUNCH=0; SEG=30
while [ $# -gt 0 ]; do
    case $1 in
    --secs) SECS=$2; shift ;;
    --env) ENVF=$2; shift ;;
    --out) OUT=$2; shift ;;
    --launch) LAUNCH=1 ;;
    *) echo "unknown option $1"; exit 2 ;;
    esac
    shift
done
OUT=${OUT:-$ROOT/quest/playrec-out/$(date +%H%M%S)}
mkdir -p "$OUT"
$A get-state >/dev/null 2>&1 || { echo "headset $SERIAL not attached"; exit 1; }

pushed=0; backup=0; lc=; nseg=0
finish() {
    [ -n "$lc" ] && kill "$lc" 2>/dev/null
    for k in $(seq 1 $nseg); do
        $A pull /sdcard/playrec_$k.mp4 "$OUT/seg_$k.mp4" >/dev/null 2>&1
        $A shell rm -f /sdcard/playrec_$k.mp4
    done
    $A pull $HEADSET_DIR/vc3.log "$OUT/vc3.log" >/dev/null 2>&1
    if [ $pushed = 1 ]; then
        $A shell rm -f $HEADSET_DIR/env.txt
        [ $backup = 1 ] && $A shell mv $HEADSET_DIR/env.txt.playrec-bak $HEADSET_DIR/env.txt
    fi
    echo "out: $OUT ($nseg segments, logcat $(wc -l < "$OUT/logcat.txt" 2>/dev/null) lines)"
    echo "env.txt on the headset: $($A shell ls $HEADSET_DIR/env.txt 2>/dev/null || echo none)"
}
trap finish EXIT
trap 'exit 130' INT TERM HUP

if [ -n "$ENVF" ]; then
    if $A shell ls $HEADSET_DIR/env.txt >/dev/null 2>&1; then
        $A shell mv $HEADSET_DIR/env.txt $HEADSET_DIR/env.txt.playrec-bak && backup=1
    fi
    $A push "$ENVF" $HEADSET_DIR/env.txt >/dev/null && pushed=1
    cp "$ENVF" "$OUT/env.txt"
    echo "env.txt pushed ($ENVF): the game reads it at launch"
fi
$A logcat -G 64M >/dev/null 2>&1
$A logcat -c
$A logcat -v threadtime > "$OUT/logcat.txt" 2>&1 &
lc=$!
if [ $LAUNCH = 1 ]; then
    $A shell am start -n $PKG/$ACTIVITY >/dev/null
    echo "game started"
fi
echo "recording ${SECS} s in ${SEG} s segments -- play as usual (Ctrl-C ends early)"
t0=$(date +%s)
while [ $(( $(date +%s) - t0 )) -lt "$SECS" ]; do
    nseg=$((nseg + 1))
    $A shell log -t PLAYREC "segment $nseg start"
    $A shell screenrecord --time-limit $SEG /sdcard/playrec_$nseg.mp4
done
