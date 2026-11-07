#!/usr/bin/env bash
# scripts/harvest-pipelines.sh [game] [adb serial]: copy the game's Vulkan
# pipeline cache, recipes and CPU-specialized shaders off the headset into
# games/<id>/pipeline-cache/quest3/, where quest/build.sh packages them: a
# first launch of that APK then compiles every pipeline the game has been
# seen to use before it starts, not in play (vk_target.c, recipes).
#
# The headset keeps them in the app's private folder, so the app is started
# once with CHIHIRO_VULKAN_PIPELINE_EXPORT (env.txt in its game folder): the
# runtime copies both out at start-up (vk_runtime.c), and the app is stopped
# again. They hold everything the app has built on this headset so far --
# play the game through (or run its scripted routes) first for coverage.
# Made from the game's shaders: never committed (.gitignore).
#
# Refuses while the game is running (don't harvest while someone plays).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/game.sh" "$@"; set -- "${GAME_ARGS[@]}"
A="adb${1:+ -s $1}"
: "${PKG:?game.conf has no PKG}" "${ACTIVITY:?}" "${HEADSET_DIR:?}"
OUT="$GAME_ROOT/pipeline-cache/quest3"
EXPORT="$HEADSET_DIR/pipelines-export"

$A get-state >/dev/null
if [ -n "$($A shell pidof "$PKG" | tr -d '\r')" ]; then
    echo "$PKG is running; close it first" >&2; exit 1
fi
if $A shell "[ -e $HEADSET_DIR/env.txt ]"; then
    echo "$HEADSET_DIR/env.txt exists (a test run's?); remove it first" >&2; exit 1
fi
cleanup() {
    $A shell am force-stop "$PKG" >/dev/null 2>&1 || true
    $A shell am broadcast -a com.oculus.vrpowermanager.automation_disable >/dev/null 2>&1 || true
    $A shell rm -rf "$HEADSET_DIR/env.txt" "$EXPORT" >/dev/null 2>&1 || true
}
trap cleanup EXIT

$A shell "mkdir -p $EXPORT && echo CHIHIRO_VULKAN_PIPELINE_EXPORT=$EXPORT > $HEADSET_DIR/env.txt"
# runs without anyone wearing the headset (proximity sensor overridden;
# undone on exit)
$A shell am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null
$A shell am start -n "$PKG/$ACTIVITY" >/dev/null
# the export happens once the pipelines are loaded, before the game starts
for _ in $(seq 1 60); do
    sleep 2
    n=$($A shell "ls $EXPORT 2>/dev/null | grep -c -E '\.(cache|recipes|specialized)\$'" | tr -d '\r' || true)
    [ "${n:-0}" -ge 3 ] && break
done
sleep 2
$A shell am force-stop "$PKG"
mkdir -p "$OUT"
got=0
for f in $($A shell "ls $EXPORT" | tr -d '\r'); do
    case "$f" in *.cache|*.recipes|*.specialized) ;; *) continue ;; esac
    $A pull "$EXPORT/$f" "$OUT/$f" >/dev/null
    echo "harvested $OUT/$f ($(wc -c < "$OUT/$f" | tr -d ' ') bytes)"
    got=$((got + 1))
done
[ "$got" -gt 0 ] || { echo "nothing exported (did the app start? see $HEADSET_DIR/$LOG)" >&2; exit 1; }
echo "now: CHIHIRO_RENDERER=vulkan quest/build.sh $GAME"
