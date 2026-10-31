#!/usr/bin/env bash
# quest/devtest.sh [--apk path] [--only name,...] [--secs n]: every number the
# Quest build is judged by, from one run on the headset.
#
# Installs the APK, then for each scenario: a settings file of its own
# (/sdcard/VirtuaCop3/testcfg, so the player's settings are never touched),
# an env.txt with the sound off, the diagnostics on and a scripted coin,
# start and shots, a run of --secs seconds, the log. Then one line per
# scenario (quest/tools/devtest_sum.py): frame rates in the intro and in
# combat, the frame-time spread, how many display frames showed each game
# frame, GPU time, the executor's walk. Logs land in quest/devtest-out/.
#
# Whatever happens -- an error, Ctrl-C -- it leaves the headset as the player
# wants it: env.txt, testcfg and dump deleted, the game stopped (sound on, no
# scripted input next launch). It refuses to start while the game is in
# focus: the player may be in it.
set -u
SERIAL=${SERIAL:-2G97C5ZJ0G02DQ}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK=${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}
A="$SDK/platform-tools/adb -s $SERIAL"
APK=$ROOT/quest/VirtuaCop3VR.apk
PKG=org.vc3recomp.vr
SECS=85
ONLY=
OUT=$ROOT/quest/devtest-out
while [ $# -gt 0 ]; do
    case $1 in
    --apk) APK=$2; shift ;;
    --only) ONLY=$2; shift ;;
    --secs) SECS=$2; shift ;;
    *) echo "unknown option $1"; exit 2 ;;
    esac
    shift
done

cleanup() {
    $A shell am force-stop $PKG >/dev/null 2>&1
    $A shell rm -rf /sdcard/VirtuaCop3/env.txt /sdcard/VirtuaCop3/testcfg /sdcard/VirtuaCop3/dump >/dev/null 2>&1
    echo "headset left for play: env.txt, testcfg and dump removed, game stopped"
}

$A get-state >/dev/null 2>&1 || { echo "headset $SERIAL not attached"; exit 1; }
if $A shell dumpsys window | grep mCurrentFocus | grep -q $PKG; then
    echo "the game is in focus on the headset -- the player may be in it; not starting"
    exit 1
fi
# Clean up once, on exit; a signal exits rather than returning into the loop
# (which would push the next scenario's env.txt after the cleanup).
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

echo "installing $APK"
$A install -r "$APK" | tail -1
$A shell appops set --uid $PKG MANAGE_EXTERNAL_STORAGE allow
mkdir -p "$OUT"
STAMP=$(date +%H%M%S)

# name | settings lines (;-separated) | extra env (space-separated)
SCEN=(
  "screen-mono|stereo=0;vr_screen=screen|"
  "screen-stereo|stereo=1;vr_screen=screen|"
  "immersive-1x|stereo=1;vr_screen=immersive;vr_detail=1|"
  "immersive-2x|stereo=1;vr_screen=immersive;vr_detail=2|"
  "immersive-3x|stereo=1;vr_screen=immersive;vr_detail=3|"
  "immersive-2x-foveation|stereo=1;vr_screen=immersive;vr_detail=2|VC3_FOVEATION=1"
  "immersive-3x-foveation|stereo=1;vr_screen=immersive;vr_detail=3|VC3_FOVEATION=1"
  "immersive-2x-bigcores|stereo=1;vr_screen=immersive;vr_detail=2|VC3_BIGCORES=1"
  "immersive-2x-72hz|stereo=1;vr_screen=immersive;vr_detail=2;vr_refresh=72|"
  "immersive-2x-nozoom|stereo=1;vr_screen=immersive;vr_detail=2|VC3_IMM_ZOOM=0"
  "immersive-2x-oldoverlay|stereo=1;vr_screen=immersive;vr_detail=2|VC3_OVERLAY_FULLRATE=1 VC3_OVERLAY_SCALE=0.75"
  "immersive-2x-spin-nolazy|stereo=1;vr_screen=immersive;vr_detail=2|RECOMP_ACK_IDLE_US=0 VC3_LAZY_RT=0"
  "immersive-2x-oldpaths|stereo=1;vr_screen=immersive;vr_detail=2|RECOMP_ICALL_FAST=0 VC3_XR_REBLIT=1 VC3_GL_TEXQUICK=0"
  "immersive-2x-nomips|stereo=1;vr_screen=immersive;vr_detail=2|VC3_GL_MIPS=0 VC3_GL_ANISO=1"
  "immersive-2x-zdiscard|stereo=1;vr_screen=immersive;vr_detail=2|VC3_GL_ZDISCARD=1"
  "immersive-2x-msaa4|stereo=1;vr_screen=immersive;vr_detail=2|VC3_GL_MSAA=4"
  "immersive-2x-vpcpu|stereo=1;vr_screen=immersive;vr_detail=2|RECOMP_GPU_XF=ff"
  "immersive-2x-vpcheck|stereo=1;vr_screen=immersive;vr_detail=2|RECOMP_GPU_XF_CHECK=300"
  "immersive-2x-vp2dcpu|stereo=1;vr_screen=immersive;vr_detail=2|VC3_GL_VP_2D=0"
  "immersive-2x-vp2dcheck|stereo=1;vr_screen=immersive;vr_detail=2|RECOMP_GPU_XF_CHECK=300 RECOMP_GPU_XF_CHECK_EVERY=20"
  "immersive-2x-noregrun|stereo=1;vr_screen=immersive;vr_detail=2|RECOMP_PB_REGRUN=0"
  "immersive-2x-glerr|stereo=1;vr_screen=immersive;vr_detail=2|VC3_GL_ERRCHECK=10"
  "screen-stereo-glerr|stereo=1;vr_screen=screen|VC3_GL_ERRCHECK=10"
)
KEYS=30:coin,31:start,40:fire,44:fire,46:fire,48:fire,50:fire,55:fire,60:fire,65:fire,70:fire,75:fire
LOGS=()
for s in "${SCEN[@]}"; do
    IFS='|' read -r name settings extra <<< "$s"
    if [ -n "$ONLY" ] && ! echo ",$ONLY," | grep -q ",$name,"; then continue; fi
    tmp=$(mktemp -d)
    { echo "scale=1"; echo "aspect=16:9"; echo "$settings" | tr ';' '\n'; } > "$tmp/settings"
    {
        echo VC3_SOUND=0; echo VC3_FPS_LOG=1; echo VC3_PACE_LOG=1; echo VC3_GPU_TIME=1
        echo RECOMP_PROF=1; echo VC3_FENCE_LOG=1; echo VC3_CONFIG_DIR=/sdcard/VirtuaCop3/testcfg
        echo VC3_AUTOKEYS=$KEYS; echo VC3_AUTOAIM=0.5,0.5
        # the stand-in gun: the overlay drawn as in real play (no one holds
        # the controllers in a scripted run)
        echo VC3_XR_FAKE_GUN=1
        for e in $extra; do echo "$e"; done
    } > "$tmp/env.txt"
    $A shell mkdir -p /sdcard/VirtuaCop3/testcfg
    $A shell rm -f /sdcard/VirtuaCop3/testcfg/settings
    $A push "$tmp/settings" /sdcard/VirtuaCop3/testcfg/settings >/dev/null
    $A push "$tmp/env.txt" /sdcard/VirtuaCop3/env.txt >/dev/null
    rm -rf "$tmp"
    echo "== $name ($SECS s)"
    $A shell am force-stop $PKG
    $A logcat -c
    $A shell am start -n $PKG/org.vc3recomp.VirtuaCop3VR >/dev/null
    sleep "$SECS"
    log="$OUT/$STAMP-$name.log"
    $A logcat -d | grep -E " vc3 |xr|OpenXR" > "$log"
    $A shell am force-stop $PKG
    grep -E "\[xr\] (extensions|performance|thread hint|display|recommended|game and back end)|\[GL\] foveation|\[CRASH\]" "$log" | sed 's/.*vc3 *: /   /'
    LOGS+=("$log")
done
echo
python3 "$ROOT/quest/tools/devtest_sum.py" "${LOGS[@]}"
