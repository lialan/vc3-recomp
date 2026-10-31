#!/usr/bin/env bash
# quest/devtest.sh [--game id] [--apk path] [--only name,...] [--secs n]: every number the
# Quest build is judged by, from one run on the headset.
#
# Installs the APK, then for each scenario: a settings file of its own
# ($HEADSET_DIR/testcfg, so the player's settings are never touched),
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
GAME=vc3                                    # --game <id>: a folder in games/
prev=
for a in "$@"; do [ "$prev" = --game ] && GAME=$a; prev=$a; done
. "$ROOT/scripts/game.sh" "$GAME"           # games/<id>/game.conf: PKG, APK, HEADSET_DIR, ...
P=$ENV_PREFIX                               # the game's env prefix (VC3)
APK=$ROOT/quest/$APK.apk
SECS=85
ONLY=
OUT=$ROOT/quest/devtest-out
while [ $# -gt 0 ]; do
    case $1 in
    --apk) APK=$2; shift ;;
    --only) ONLY=$2; shift ;;
    --secs) SECS=$2; shift ;;
    --game) shift ;;                        # read above
    *) echo "unknown option $1"; exit 2 ;;
    esac
    shift
done

cleanup() {
    $A shell am force-stop $PKG >/dev/null 2>&1
    $A shell rm -rf $HEADSET_DIR/env.txt $HEADSET_DIR/testcfg $HEADSET_DIR/dump >/dev/null 2>&1
    $A shell am broadcast -a com.oculus.vrpowermanager.automation_disable >/dev/null 2>&1
    echo "headset left for play: env.txt, testcfg and dump removed, game stopped, proximity override off"
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

# As if worn: unworn, the headset may stop the display or the audio.
$A shell am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null 2>&1
echo "installing $APK"
$A install -r "$APK" | tail -1
$A shell appops set --uid $PKG MANAGE_EXTERNAL_STORAGE allow
mkdir -p "$OUT"
STAMP=$(date +%H%M%S)

KEYS=30:coin,31:start,40:fire,44:fire,46:fire,48:fire,50:fire,55:fire,60:fire,65:fire,70:fire,75:fire
PEDAL2=$(for t in $(seq 98 0.5 106) $(seq 120 0.5 132); do printf '%s:pedal,' $t; done)
PEDAL2=${PEDAL2%,}
FIRE2=$(for t in $(seq 86 2 148); do printf '%s:fire,' $t; done)
FIRE2=${FIRE2%,}
PEDAL=64:pedal,64.5:pedal,65:pedal,65.5:pedal,66:pedal,66.5:pedal,67:pedal,67.5:pedal,68:pedal,68.5:pedal,69:pedal,69.5:pedal,70:pedal,70.5:pedal,71:pedal,71.5:pedal,72:pedal,72.5:pedal,73:pedal,73.5:pedal,74:pedal,74.5:pedal,75:pedal,75.5:pedal,76:pedal,76.5:pedal,77:pedal,77.5:pedal,78:pedal,78.5:pedal,79:pedal,79.5:pedal,80:pedal,80.5:pedal,81:pedal,81.5:pedal,82:pedal,82.5:pedal,83:pedal,83.5:pedal,84:pedal
# name | settings lines (;-separated) | extra env (space-separated)
SCEN=(
  "screen-mono|stereo=0;vr_screen=screen|"
  "screen-stereo|stereo=1;vr_screen=screen|"
  "screen-stereo-1x|stereo=1;vr_screen=screen;vr_screen_res=scale|"
  "immersive-1x|stereo=1;vr_screen=immersive;vr_res=1x|"
  "immersive-2x|stereo=1;vr_screen=immersive;vr_res=2x|"
  "immersive-native|stereo=1;vr_screen=immersive;vr_res=native|"
  # native's levers: foveation off (on for immersive frames by default),
  # stronger; the frame blitted straight into the swapchain (off by default:
  # Meta's runtime stalls on it, see quest_host.c direct_on)
  "immersive-native-nofov|stereo=1;vr_screen=immersive;vr_res=native|CHIHIRO_FOVEATION=0"
  "immersive-native-direct|stereo=1;vr_screen=immersive;vr_res=native|CHIHIRO_XR_DIRECT=1"
  "immersive-native-fovgain8|stereo=1;vr_screen=immersive;vr_res=native|CHIHIRO_FOV_GAIN=8"
  # the mission intro at native with fixed-function draw merging (runtime
  # gl_backend.c, RECOMP_GL_MERGE; off by default), against immersive-native:
  # GL thread per draw ([DEFER] submit busy), [GLMERGE] runs, GPU time.
  # -cmp checks merged against unmerged vertices bit for bit (slow; the
  # [GLMERGE] "differ" count must be 0).
  "immersive-native-merge|stereo=1;vr_screen=immersive;vr_res=native|RECOMP_GL_MERGE=1 CHIHIRO_GL_TIMING=1"
  "immersive-native-nomerge|stereo=1;vr_screen=immersive;vr_res=native|RECOMP_GL_MERGE=0 CHIHIRO_GL_TIMING=1"
  "immersive-native-merge-cmp|stereo=1;vr_screen=immersive;vr_res=native|RECOMP_GL_MERGE=2"
  # attract with no coin and no input (run it with --secs 125): the title
  # logo must give way to the demo -- the attract stall on the device
  "attract-native|stereo=1;vr_screen=immersive;vr_res=native;vr_coins=manual|VC3_AUTOKEYS=none CHIHIRO_IMM_LOG=1"
  # the push-buffer decode and the GL submission on two threads (the Quest's
  # default is one: no gain measured there)
  "immersive-native-decode1|stereo=1;vr_screen=immersive;vr_res=native|RECOMP_PB_DECODE_THREAD=1"
  "immersive-2x-decode1|stereo=1;vr_screen=immersive;vr_res=2x|RECOMP_PB_DECODE_THREAD=1"
  # E.S. mode (the pedal held from 64 s): the game's ~40 full-screen
  # distortion layers over the scene, combat's heaviest fill
  "immersive-2x-es|stereo=1;vr_screen=immersive;vr_res=2x|VC3_AUTOKEYS=$KEYS,$PEDAL"
  "immersive-native-es|stereo=1;vr_screen=immersive;vr_res=native|VC3_AUTOKEYS=$KEYS,$PEDAL"
  "immersive-native-es-diag|stereo=1;vr_screen=immersive;vr_res=native|VC3_AUTOKEYS=$KEYS,$PEDAL CHIHIRO_GL_EFFECT_LOG=1"
  "immersive-native-es-nofov|stereo=1;vr_screen=immersive;vr_res=native|VC3_AUTOKEYS=$KEYS,$PEDAL CHIHIRO_FOVEATION=0 CHIHIRO_GL_TIMING=1"
  # the E.S. stack's effect target (gl_backend.c): off (every layer at full
  # size), at a half and a third each way (the default is a quarter)
  "immersive-native-es-seg|stereo=1;vr_screen=immersive;vr_res=native|VC3_AUTOKEYS=$KEYS,$PEDAL CHIHIRO_GPU_TIME_SEG=1"
  "immersive-native-es-bis-nodepth|stereo=1;vr_screen=immersive;vr_res=native|VC3_AUTOKEYS=$KEYS,$PEDAL CHIHIRO_FX_NODEPTH=1 CHIHIRO_GL_TIMING=1"
  "immersive-native-es-bis-nosmall|stereo=1;vr_screen=immersive;vr_res=native|VC3_AUTOKEYS=$KEYS,$PEDAL CHIHIRO_FX_NOSMALL=1 CHIHIRO_GL_TIMING=1"
  "immersive-native-es-fx8|stereo=1;vr_screen=immersive;vr_res=native|VC3_AUTOKEYS=$KEYS,$PEDAL CHIHIRO_GL_EFFECT_SCALE=8 CHIHIRO_GL_TIMING=1"
  "immersive-native-es-fx2|stereo=1;vr_screen=immersive;vr_res=native|VC3_AUTOKEYS=$KEYS,$PEDAL CHIHIRO_GL_EFFECT_SCALE=2"
  "immersive-native-es-fx1|stereo=1;vr_screen=immersive;vr_res=native|VC3_AUTOKEYS=$KEYS,$PEDAL CHIHIRO_GL_EFFECT_SCALE=1 CHIHIRO_GL_TIMING=1"
  "immersive-native-es-fx3|stereo=1;vr_screen=immersive;vr_res=native|VC3_AUTOKEYS=$KEYS,$PEDAL CHIHIRO_GL_EFFECT_SCALE=3"
  # E.S. bisect switches: the combiner interpreter, the effect target's
  # source at full size; GL errors checked (VC3_GL_ERRCHECK)
  "immersive-native-es-nospec|stereo=1;vr_screen=immersive;vr_res=native|VC3_AUTOKEYS=$KEYS,$PEDAL CHIHIRO_GL_RC_SPEC=0"
  "immersive-native-es-nofxlow|stereo=1;vr_screen=immersive;vr_res=native|VC3_AUTOKEYS=$KEYS,$PEDAL CHIHIRO_GL_FX_LOW=0"
  # E.S. entered and left three times (64-84, 98-106, 120-132 s), firing
  # throughout: run with --secs 150
  "immersive-native-es-multi|stereo=1;vr_screen=immersive;vr_res=native|VC3_AUTOKEYS=$KEYS,$PEDAL,$PEDAL2,$FIRE2 CHIHIRO_GL_EFFECT_LOG=1"
  "immersive-native-xform-cpu|stereo=1;vr_screen=immersive;vr_res=native|RECOMP_GPU_XF=0 CHIHIRO_GL_ERRCHECK=1 CHIHIRO_GL_TIMING=1"
  "immersive-native-es-diagnostic|stereo=1;vr_screen=immersive;vr_res=native|VC3_AUTOKEYS=$KEYS,$PEDAL CHIHIRO_GL_ERRCHECK=1 CHIHIRO_GL_EFFECT_LOG=2 CHIHIRO_GL_TIMING=1"
  "immersive-native-es-errcheck|stereo=1;vr_screen=immersive;vr_res=native|VC3_AUTOKEYS=$KEYS,$PEDAL CHIHIRO_GL_ERRCHECK=1"
  "immersive-2x-foveation|stereo=1;vr_screen=immersive;vr_res=2x|CHIHIRO_FOVEATION=1"
  "immersive-2x-nofov|stereo=1;vr_screen=immersive;vr_res=2x|CHIHIRO_FOVEATION=0"
  "immersive-2x-bigcores|stereo=1;vr_screen=immersive;vr_res=2x|CHIHIRO_BIGCORES=1"
  "immersive-2x-72hz|stereo=1;vr_screen=immersive;vr_res=2x;vr_refresh=72|"
  "immersive-2x-nozoom|stereo=1;vr_screen=immersive;vr_res=2x|CHIHIRO_IMM_ZOOM=0"
  "immersive-2x-oldoverlay|stereo=1;vr_screen=immersive;vr_res=2x|CHIHIRO_OVERLAY_FULLRATE=1 CHIHIRO_OVERLAY_SCALE=0.75"
  "immersive-2x-spin-nolazy|stereo=1;vr_screen=immersive;vr_res=2x|RECOMP_ACK_IDLE_US=0 CHIHIRO_LAZY_RT=0"
  "immersive-2x-oldpaths|stereo=1;vr_screen=immersive;vr_res=2x|RECOMP_ICALL_FAST=0 CHIHIRO_XR_REBLIT=1 CHIHIRO_GL_TEXQUICK=0"
  "immersive-2x-nomips|stereo=1;vr_screen=immersive;vr_res=2x|CHIHIRO_GL_MIPS=0 CHIHIRO_GL_ANISO=1"
  "immersive-2x-zdiscard|stereo=1;vr_screen=immersive;vr_res=2x|CHIHIRO_GL_ZDISCARD=1"
  "immersive-2x-msaa4|stereo=1;vr_screen=immersive;vr_res=2x|CHIHIRO_GL_MSAA=4"
  "immersive-2x-vpcpu|stereo=1;vr_screen=immersive;vr_res=2x|RECOMP_GPU_XF=ff"
  "immersive-2x-vpcheck|stereo=1;vr_screen=immersive;vr_res=2x|RECOMP_GPU_XF_CHECK=300"
  "immersive-2x-vp2dcpu|stereo=1;vr_screen=immersive;vr_res=2x|CHIHIRO_GL_VP_2D=0"
  "immersive-2x-vp2dcheck|stereo=1;vr_screen=immersive;vr_res=2x|RECOMP_GPU_XF_CHECK=300 RECOMP_GPU_XF_CHECK_EVERY=20"
  "immersive-2x-noregrun|stereo=1;vr_screen=immersive;vr_res=2x|RECOMP_PB_REGRUN=0"
  # texture decode (src/gpu/gl_texdec.c): GPU (compute, the default) against
  # CPU, by time; the fragment pass; and the compare modes, which read every
  # upload back and count texels unlike the CPU decode ([TEXCMP] lines)
  "immersive-2x-texgpu|stereo=1;vr_screen=immersive;vr_res=2x|CHIHIRO_TEX_STATS=1"
  "immersive-2x-texcpu|stereo=1;vr_screen=immersive;vr_res=2x|CHIHIRO_TEX_GPU=0 CHIHIRO_TEX_STATS=1"
  "immersive-2x-texfrag|stereo=1;vr_screen=immersive;vr_res=2x|CHIHIRO_TEX_GPU=frag CHIHIRO_TEX_STATS=1"
  "immersive-2x-texcompare|stereo=1;vr_screen=immersive;vr_res=2x|CHIHIRO_TEX_COMPARE=1"
  "immersive-2x-texcompare-frag|stereo=1;vr_screen=immersive;vr_res=2x|CHIHIRO_TEX_GPU=frag CHIHIRO_TEX_COMPARE=1"
  "immersive-2x-texcompare-s3tc|stereo=1;vr_screen=immersive;vr_res=2x|CHIHIRO_TEX_S3TC=1 CHIHIRO_TEX_COMPARE=1"
  # the write-back flush answered by the walker alone, as before (the A/B)
  # the guest's long kernel waits against the vblanks ([WAIT] lines)
  "immersive-2x-waits|stereo=1;vr_screen=immersive;vr_res=2x|RECOMP_WAIT_TRACE=2 RECOMP_WAIT_TRACE_AFTER=60"
  # the guest vblank not aligned to the headset's frames (the A/B for it)
  "immersive-2x-nocadence|stereo=1;vr_screen=immersive;vr_res=2x|VC3_XR_CADENCE=0"
  "immersive-2x-slew20us|stereo=1;vr_screen=immersive;vr_res=2x|RECOMP_VBLANK_SLEW_NS=20000"
  "immersive-2x-noflushack|stereo=1;vr_screen=immersive;vr_res=2x|RECOMP_FLUSH_ACK_US=0"
  # sound on (the others are muted): the audio device's stall and reopen
  # lines ([APU] stopped consuming, [SDL2] reopening) show if it happens
  "immersive-2x-sound|stereo=1;vr_screen=immersive;vr_res=2x|CHIHIRO_SOUND=1"
  "immersive-2x-glerr|stereo=1;vr_screen=immersive;vr_res=2x|CHIHIRO_GL_ERRCHECK=10"
  "screen-stereo-glerr|stereo=1;vr_screen=screen|CHIHIRO_GL_ERRCHECK=10"
)
LOGS=()
for s in "${SCEN[@]}"; do
    IFS='|' read -r name settings extra <<< "$s"
    if [ -n "$ONLY" ] && ! echo ",$ONLY," | grep -q ",$name,"; then continue; fi
    tmp=$(mktemp -d)
    { echo "scale=1"; echo "aspect=16:9"; echo "$settings" | tr ';' '\n'; } > "$tmp/settings"
    {
        echo ${P}_SOUND=0; echo ${P}_FPS_LOG=1; echo ${P}_PACE_LOG=1; echo ${P}_GPU_TIME=1
        echo RECOMP_PROF=1; echo ${P}_FENCE_LOG=1; echo RECOMP_THREAD_LOG=1; echo RECOMP_FRAME_TRACE=1; echo ${P}_CONFIG_DIR=$HEADSET_DIR/testcfg
        echo ${P}_AUTOKEYS=$KEYS; echo ${P}_AUTOAIM=0.5,0.5
        # the stand-in gun: the overlay drawn as in real play (no one holds
        # the controllers in a scripted run)
        echo ${P}_XR_FAKE_GUN=1
        for e in $extra; do echo "$e"; done
    } > "$tmp/env.txt"
    $A shell mkdir -p $HEADSET_DIR/testcfg
    $A shell rm -f $HEADSET_DIR/testcfg/settings
    $A push "$tmp/settings" $HEADSET_DIR/testcfg/settings >/dev/null
    $A push "$tmp/env.txt" $HEADSET_DIR/env.txt >/dev/null
    rm -rf "$tmp"
    echo "== $name ($SECS s)"
    $A shell am force-stop $PKG
    $A logcat -G 16M >/dev/null 2>&1     # the default ring lost the first minute (the intro)
    $A logcat -c
    $A shell am start -n $PKG/$ACTIVITY >/dev/null
    sleep "$SECS"
    log="$OUT/$STAMP-$name.log"
    $A logcat -d | grep -E " $LOGTAG |xr|OpenXR" > "$log"
    $A shell am force-stop $PKG
    grep -E "\[xr\] (extensions|performance|thread hint|display|recommended|game and back end)|\[GL\] foveation|\[CRASH\]|\[TEXDEC\]" "$log" | sed "s/.*$LOGTAG *: /   /"
    # the texture scenarios' totals: the last (cumulative) report of each
    grep -E "\[TEXSTAT\]   total|\[TEXCMP\]   total" "$log" | tail -2 | sed "s/.*$LOGTAG *: /   /"
    grep -E "\[TEXCMP\] MISMATCH" "$log" | head -5 | sed "s/.*$LOGTAG *: /   /"
    LOGS+=("$log")
done
echo
python3 "$ROOT/quest/tools/devtest_sum.py" "${LOGS[@]}"
